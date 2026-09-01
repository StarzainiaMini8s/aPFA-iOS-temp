// streamer.h — file-backed event pool: identical layout, kernel-managed residency.
//
// The 72-byte PlayEvent pool is aPFA's memory floor: Tau-class MIDIs (6.28M
// notes) commit ~1 GB of pool + pointer table, all of it load-bearing for the
// crash behaviour (note.h). This streamer keeps THE SAME pool — same struct,
// same parse order, same virtual addresses relative to the pool base, same
// sister pointers, same time-sorted events[] walk — but backs it with a
// read-only file mapping instead of anonymous RAM:
//
//   1. The parse writes the pool to a temp file in PARSE order (track by
//      track), with absMicroSec already in µs and `sister` holding the real
//      pointer values, precomputed against a VA region reserved up front.
//   2. The file is mapped PROT_READ | MAP_FIXED over that reservation and
//      unlinked. events[] is a normal resident vector of pointers into the
//      mapping, sorted exactly as midi_parser.cpp sorts it.
//   3. A loader thread (kept off the engine core, like the BASS render
//      threads) runs ahead of the playhead issuing MADV_WILLNEED over each
//      track's upcoming byte range, so the kernel has the read in flight
//      before dispatch/buildVisible asks for it, and MADV_DONTNEEDs ranges
//      the playhead has left behind.
//
// The engine's hot path is untouched — dispatch(), buildVisible(), and frame()
// compile to the same code walking the same addresses; the scatter, the cache
// working set, and the TLB span are the baseline's by construction. What
// changes is only WHO holds the cold majority of the pool: the page cache
// (evictable, disk-backed) instead of the process heap. If the loader ever
// falls behind, a page fault stretches the faulting frame — pressure enters
// the clock the same way every other stall does; audio never skips.
//
// ---- Why the window is sized off the consumer (1.3.0-beta2) ----
//
// Desktop PFA never speculates: it `new`s every event and lets the OS page it,
// so it pays for exactly the bytes it dereferences and degrades as "pagefile
// lag" proportional to the storage underneath. Through 1.3.0-beta1 aPFA did
// the opposite — a fixed 30 s front horizon, faulted in SYNCHRONOUSLY on the
// loader thread. Nothing in the engine reads anywhere near 30 s ahead:
//
//   * buildVisible() reads to clockUs + 3.0s * noteSpeed — 150 ms at the
//     default note speed, 3 s at the maximum (engine.cpp).
//   * dispatch() reads sequentially AT the playhead.
//   * the O(P) note-off scan dereferences every sounding note-on
//     (`ev[active_[i]]->param1`) — arbitrarily far back, but bounded by
//     POLYPHONY, not by time, and already covered by pinnedPages_.
//
// So the real working set is a few MB, and beta1 was warming ~200x past its
// own consumer and physically reading all of it. That made the LOADER's I/O,
// not the engine's, the thing that scaled with storage speed — the opposite
// of PFA, where slow storage costs only what the engine actually touches.
//
// beta2: frontUs_ is derived from the visible band plus a small latency lead
// (setVisibleUs), the bulk window is ADVISED rather than faulted (the engine's
// own demand faults are the mechanism, exactly as in PFA), and the horizon is
// priced against MemAvailable each tick and shrunk until it fits. Explicit
// per-track advisories are still needed because 61-133 interleaved streams in
// one mapping defeat the kernel's own sequential readahead detector.
//
// Residency ~= events[] table + sisterPos + the sliding window
// [playhead - back, playhead + front] (defaults below, env-tunable via
// APFA_WIN_FRONT_US / APFA_WIN_BACK_US, which pin the horizon and disable the
// adaptive sizing). The pool never needs to be resident at once, which is the
// entire point.
//
// Replaying a MIDI is cheap for the same reason it is in PFA, and cheaper: the
// mapping is PROT_READ|MAP_PRIVATE, so pool pages are always CLEAN. Eviction
// never costs a writeback, and MADV_DONTNEED drops only the PTEs — the page
// cache behind them survives, so a second pass through the same file takes
// minor faults instead of disk reads. What defeated that in beta1 was the
// oversized window evicting the page cache faster than it could accumulate.
//
// Everything here is behind the APFA_STREAMING compile flag (default OFF);
// the legacy path is byte-identical when the flag is off. Android-only wiring
// — iOS builds never define the flag (engine signatures unchanged).
#pragma once
#ifdef APFA_STREAMING

#include "midi_parser.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace apfa {

// One mapped piece of the pool file: file bytes [off, off+len) live at `va`.
//
// A 64-bit build always has exactly ONE of these covering the whole pool —
// byte for byte the original single-reservation design. Only a 32-bit process
// ever gets more than one: there the pool's virtual address range is the
// binding limit, not RAM or storage. Measured on an LG X410 (SD425, 32-bit
// ROM) while loading a 14.65M-note MIDI: 3056 MB of user address space, 1305
// MB already mapped (712 MB of it ART's heap), 1751 MB free — but the largest
// single free run was only 980 MB against a 1564 MB pool. Split across the
// four largest gaps it fits with room to spare.
//
// Splitting is safe because nothing outside the loader ever treats the pool as
// one flat array: the engine reaches events only through events[] and through
// `sister`, both of which hold real PlayEvent pointers baked in during pass B.
// Segment boundaries are multiples of lcm(sizeof(PlayEvent), page size), so no
// event ever straddles two segments, and the hot path compiles to exactly the
// same code walking exactly the same addresses.
// ---- sliced pool (32-bit only; see SLICED-POOL-DESIGN.md) -------------------
//
// One slice of the song. Its pool holds, laid out track by track exactly as the
// full pool is:
//   * the BODY, events at positions [firstPos, endPos) of the time-sorted walk;
//   * CARRY-IN, a copy of every note-on still sounding at firstPos, because the
//     O(P) note-off scan dereferences those and they are not in the body;
//   * CARRY-OUT, a copy of the note-off of every body note-on that ends at or
//     after endPos, so that every note-on's `sister` resolves inside its own
//     slice.
// With all three, `events[]` and `sister` are real PlayEvent pointers for the
// whole time the slice is mapped, which is what keeps dispatch()/buildVisible()
// untouched and the hot path free.
struct SlicePlan {
    uint32_t firstPos  = 0;   // first body position in the time-sorted walk
    uint32_t endPos    = 0;   // one past the last body position
    uint32_t carryIn   = 0;   // note-ons sounding at firstPos
    uint32_t carryOut  = 0;   // body note-ons whose note-off is at/after endPos
    uint32_t bodyEvents() const { return endPos - firstPos; }
    uint32_t poolEvents() const { return bodyEvents() + carryIn + carryOut; }
};

// Choose slice boundaries so that every slice's pool fits `budgetEvents`.
//
// Pure and total: one forward pass over the pairing table, tracking how many
// notes are sounding at each position (that count IS the carry-out if the slice
// is cut there, and the carry-in of the one that follows). Split out of the
// streamer so it can be exercised on the host without a phone or a MIDI —
// see tests/slice_test.cpp.
//
// budgetEvents is the arena expressed in events, minEvents stops a pathological
// note-wall (carry alone exceeding the budget) from producing empty slices; a
// slice may exceed the budget only when it cannot be cut any smaller.
std::vector<SlicePlan> planSlices(const std::vector<uint32_t>& sisterPos,
                                  uint32_t budgetEvents,
                                  uint32_t minEvents);

struct PoolSeg {
    uint8_t* va  = nullptr;   // where this piece is mapped
    size_t   off = 0;         // its first byte's offset within the pool file
    size_t   len = 0;         // its length in bytes
};

// Pool byte offset -> mapped address. The single-segment case is the common
// one and stays a plain add.
inline uint8_t* poolAddrIn(const std::vector<PoolSeg>& segs, size_t off) {
    if (segs.size() == 1) return segs[0].va + off;
    for (const PoolSeg& s : segs)
        if (off - s.off < s.len) return s.va + (off - s.off);   // unsigned wrap = below
    return nullptr;
}

// The inverse: mapped address -> pool byte offset.
inline size_t poolOffIn(const std::vector<PoolSeg>& segs, const void* p) {
    const uint8_t* q = static_cast<const uint8_t*>(p);
    if (segs.size() == 1) return static_cast<size_t>(q - segs[0].va);
    for (const PoolSeg& s : segs)
        if (static_cast<size_t>(q - s.va) < s.len)
            return s.off + static_cast<size_t>(q - s.va);
    return 0;
}

class Streamer {
public:
    // sisterPos values for non-note events and note-offs. Everything below
    // kSisNoteOff is a note-on whose value is its note-off's events[] position.
    static constexpr uint32_t kSisNonNote = 0xFFFFFFFFu;
    static constexpr uint32_t kSisNoteOff = 0xFFFFFFFEu;

    ~Streamer() { close(); }

    // Parse `midiPath` and fill `out` with exactly what parseMidi() would have
    // produced — events[], trackColors, programChangeIdx, totalUs, note range,
    // note count, valid — except out.eventPool stays empty: the pool events
    // live in the read-only file mapping instead. The temp pool file is
    // created next to the MIDI (the app cache dir — PlaybackActivity copies
    // the MIDI there) and unlinked once mapped.
    //
    // `poolDir` overrides that location — the app's cache dir on the SD card
    // ("Pagefile on SD Card" in Advanced Settings), so a 10 GB pagefile lands
    // on the card instead of internal storage. Empty = the MIDI's directory,
    // the proven default. An override is PROBED first (create + write + map);
    // if the volume can't back a read-only private mapping the load silently
    // falls back to the internal cache dir rather than failing. Temps there are
    // kept NAMED and unlinked by close() instead of unlinked-while-open:
    // removable volumes reach us through FUSE, where an unlinked mapping is not
    // something to bet a multi-GB load on. Crash litter is swept on the next
    // open() (and by MainActivity at launch).
    //
    // `chunked` selects the sort strategy for pass B/D ("Chunked Disk
    // Streaming" in Advanced Settings, chosen by Engine::load only when
    // needed): false = the whole (key, idx) table and note-pair list stay in
    // RAM until pass D — a 16 B/event + 8 B/note load transient that caps
    // un-chunked loads at roughly 80 M notes on an 8 GB phone; true = keys
    // spill to disk in pre-sorted 32 MB runs and pairs stream through a temp
    // file, k-way merged in pass D — the transient disappears and free
    // storage becomes the only ceiling. Total event order is identical in
    // both modes (same comparator, same tie-break).
    //
    // Returns false on any failure (no space, VA reservation, oversized file)
    // with everything cleaned up, so the caller can fall back to the legacy
    // in-RAM parse — except when diskFull() is set: the statfs guard tripped
    // (the pagefile would leave the volume under ~2% free), and the caller
    // should abort with the "free up space" message instead of falling back.
    bool open(const std::string& midiPath, MidiData& out,
              std::atomic<float>& progress, bool chunked = false,
              const std::string& poolDir = std::string());
    // True when the last open() failed because pool/spill writes would (or
    // did) exhaust the storage volume — see kMinFreeDiskFraction.
    bool diskFull() const { return diskFull_; }
    // True when the last open() failed because the pool would cross the
    // volume's per-file ceiling — a FAT32 SD card stops at 4 GB, which is
    // under half of what a U11-class MIDI needs. Detected up front from the
    // filesystem type where the kernel reports it, and from EFBIG otherwise
    // (Android's FUSE layer hides the real type behind its own magic).
    bool fileTooBig() const { return fileTooBig_; }
    // True when the last open() failed because the pool's virtual address
    // range could not be reserved. The pool is ONE contiguous VA reservation
    // (sister pointers are precomputed against its base), so a 32-bit process
    // — a 64-bit SoC running a 32-bit ROM very much included — tops out around
    // 2-3 GB of user address space for everything. Distinct from the other two
    // because falling back to the in-RAM parse cannot help: a MIDI whose pool
    // will not fit in the address space is exactly a MIDI whose in-RAM parse
    // will not fit in RAM either, so the fallback just guarantees an OOM kill.
    bool noAddrSpace() const { return noAddrSpace_; }
    // Set alongside noAddrSpace() when the un-chunked sort was what did not
    // fit, and the chunked on-disk sort — which spills that table instead of
    // holding it — would have. The load is not impossible, it just needs the
    // other mode ("Chunked Disk Streaming").
    bool needsChunked() const { return needsChunked_; }
    void close();
    bool isOpen() const { return base_ != nullptr; }

    // ---- sliced pool (32-bit only; see SLICED-POOL-DESIGN.md) ---------------
    //
    // True when this load took the sliced path: the full pool file is written
    // exactly as before but NEVER mapped, and the engine walks one slice at a
    // time through a fixed pair of arenas. The address-space cost becomes the
    // arena, not the song, so free storage is the only ceiling left. A 64-bit
    // build never gets here (open() refuses to slice when sizeof(void*) > 4).
    bool isSliced() const { return sliced_; }
    // Where dispatch() has to stop and call advanceSlice(): the current slice's
    // plan boundary, which is exactly where the notes sounding across it become
    // the next slice's carry-in. totalEvents when not sliced, so the engine
    // writes the same bound in both modes.
    size_t dispatchEnd() const { return sliced_ ? cur_.endPos : totalEvents_; }
    // One past the last events[] position that is mapped right now. Always at
    // least one visible band past dispatchEnd(), because buildVisible() reads
    // ahead of the playhead and must not run off the end of the slice.
    size_t readEnd() const { return sliced_ ? cur_.matEnd : totalEvents_; }
    // Retire the current slice, make the next one current and repoint events[]
    // at it. Blocks if the builder has not finished — ordinary storage lag
    // entering the clock, exactly as a major fault does. False at the end of the
    // song or when the build failed.
    bool advanceSlice(int64_t playheadUs);
    // Make the slice holding events[] position `pos` current, building it if it
    // is not. A seek outside the mapped slice is a rebuild, i.e. a pause — the
    // deliberate cost the design accepts for a deliberate user action.
    bool seekSlice(size_t pos, int64_t playheadUs);
    // Event time straight from the resident table, so it works for positions
    // outside the mapped slice (applySeek's binary search, advancePcCursor).
    // Sliced loads only — posUs_ is empty otherwise.
    int64_t usAt(size_t pos) const {
        return static_cast<int64_t>(static_cast<uint32_t>(posUs_[pos]));
    }
    // First events[] position whose time is > us. The sliced twin of
    // applySeek's binary search over ev[mid]->absMicroSec.
    size_t posForTime(int64_t us) const;
    // Time of the first note-on — Engine::load's pre-roll anchor, which cannot
    // walk events[] when nothing is mapped yet.
    int64_t firstNoteUs() const { return firstNoteUs_; }
    // The three payload bytes of programChangeIdx[i], resident, so
    // playSkippedEvents never dereferences an unmapped historical event.
    struct PcEvent { uint8_t code, param1, param2; };
    PcEvent pcEventAt(size_t i) const {
        uint32_t v = pcData_[i];
        return PcEvent{ static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v >> 16) };
    }

    // Fast read-only skim (pass A alone): exact event count, no temp files, no
    // allocations per event. Lets the engine predict the in-RAM parse footprint
    // BEFORE committing to it — 0 on failure/empty.
    static uint64_t predictEventCount(const std::string& midiPath);

    // Peak RAM pass D cannot get rid of for a chunked load of `events` events,
    // however much storage is free: sisterPos_ for the whole session, plus the
    // smallest inv[] window the windowed build will settle for and the room it
    // keeps clear around it. The engine gates on this — owned here so it cannot
    // drift from what pass D actually allocates.
    static uint64_t mergeResidentBytes(uint64_t events);

    // For events[] position `pos`: kSisNonNote, kSisNoteOff, or (for a
    // note-on) the events[] position of its note-off. Lets applySeek rebuild
    // active_ without dereferencing the pool for every historical event
    // (which would fault the entire cold file in random order).
    uint32_t sisterPosAt(size_t pos) const { return sisterPos_[pos]; }

    // How far ahead of the playhead the engine actually reads: buildVisible's
    // 3.0s * noteSpeed band. The front horizon is this plus kLeadUs, so the
    // window tracks the note-speed setting instead of a fixed constant. Call
    // before startLoader (Engine::load); ignored once APFA_WIN_FRONT_US pins
    // the horizon.
    void setVisibleUs(int64_t visibleUs);

    // Start/stop the read-ahead thread. `playheadUs` is the engine's published
    // clock (Engine::pubTimeUs_); `initialUs` seeds the window before the
    // first frame publishes (the pre-roll start). `engineCpuMask` is the
    // engine pin mask — the loader is pinned to the complement, same policy
    // as the BASS render threads.
    void startLoader(const std::atomic<int64_t>* playheadUs, int64_t initialUs,
                     uint64_t engineCpuMask);
    void stopLoader();

    // Window telemetry for Engine::frame's "win:" line, published by the
    // loader tick. Front horizon actually in force (µs), the pool bytes that
    // horizon prices out at, and the MemAvailable it was priced against.
    int64_t  windowFrontUs() const {
        return winFrontUs_.load(std::memory_order_relaxed);
    }
    uint64_t windowBytes() const {
        return winBytes_.load(std::memory_order_relaxed);
    }
    uint64_t memAvailBytes() const {
        return memAvail_.load(std::memory_order_relaxed);
    }

    // Engine thread, during applySeek: reposition the window and synchronously
    // warm what the very next frame reads — the visible band
    // [targetUs, visibleEndUs] plus the still-sounding note-ons in
    // `activePositions` (events[] positions) and their note-offs. The loader
    // thread widens to the full window asynchronously afterwards.
    void warmSeek(int64_t targetUs, int64_t visibleEndUs,
                  const std::vector<int>& activePositions);

    // Honest resident estimate: the tables held in RAM plus the pool bytes the
    // sliding window currently keeps warm (per-track spans + pinned pages).
    // Kernel page cache beyond the window is reclaimable and not counted —
    // it is not RAM the app is holding.
    size_t memoryBytes() const;

    // Exact pool size on disk (the pagefile the load streamed through), plus
    // the two materialised slices a sliced load keeps beside it. 0 when no
    // streaming pool is open, so it doubles as "was streaming used at all".
    size_t diskBytes() const {
        return fullPoolBytes_ + (sliced_ ? 2 * arenaBytes_ : 0);
    }

    // Major faults taken by the loader thread, sampled each 100 ms tick. These
    // are read-ahead working as designed — the engine's own fault count is the
    // one that indicates a stall (see Engine::frame's "fault:" line).
    uint64_t loaderMajFlt() const {
        return loaderMajFlt_.load(std::memory_order_relaxed);
    }

private:
    struct TrackRange { size_t first = 0, count = 0; };   // pool-index range
    struct TrackSample { int64_t us; uint32_t poolIdx; };  // sampled time index

    // One materialised slice: the compact pool for plan_[idx], written to its
    // own temp file and mapped read-only over arena_[arena]. Every slice maps
    // at the SAME address for its parity, which is what lets the materialiser
    // bake real `sister` pointers into the file — the whole reason the hot path
    // pays nothing for slicing.
    struct SliceMap {
        int      idx   = -1;        // index into plan_ (-1 = nothing here)
        int      arena = 0;         // which of the two arenas it maps into
        int      fd    = -1;        // its temp file, open while mapped
        std::string path;           // non-empty only on the SD-card pool dir
        size_t   firstPos = 0;      // == plan_[idx].firstPos
        size_t   endPos   = 0;      // == plan_[idx].endPos: the transition point
        size_t   matEnd   = 0;      // body actually materialised (endPos + lookahead)
        size_t   slots    = 0;      // events in the slice pool
        size_t   bytes    = 0;      // slots * sizeof(PlayEvent)
        // Slot of every body position, indexed by pos - firstPos: the transition
        // fills events[] straight out of this, sequentially.
        std::vector<uint32_t> bodySlot;
        // Carry-in and carry-out positions (ascending) and their slots. Small —
        // bounded by polyphony at the two boundaries, not by song length.
        std::vector<uint32_t> carryPos, carrySlot;
        // The loader's navigation tables, rebuilt per slice against slot
        // indices instead of pool indices.
        std::vector<TrackRange>  trackRange;
        std::vector<uint32_t>    trackSampleOff;
        std::vector<TrackSample> trackSamples;
    };

    void loaderMain();
    void loaderTickLocked(int64_t playheadUs);
    // Touch (fault in) the pool bytes [firstByte, lastByte] — one read per
    // page, synchronous. Reserved for the paths whose data the very next frame
    // is certain to dereference: pins, warmSeek, and the front-edge sisters.
    // The bulk window uses advisePoolRange instead.
    void touchPoolRange(size_t firstByte, size_t lastByte, bool willneed);
    // Hand [firstByte, lastByte] to the kernel as MADV_WILLNEED and return
    // without waiting. This is the loader's normal mode: the read goes in
    // flight, and if it has not landed by the time the engine dereferences the
    // page, the engine takes the fault — which is precisely what PFA does.
    void advisePoolRange(size_t firstByte, size_t lastByte);
    // Price the front horizon against MemAvailable and shrink it until the
    // window fits. Returns the horizon to use this tick; also publishes the
    // win* telemetry. Returns frontUs_ unchanged when the horizon is pinned.
    int64_t budgetedFrontUs(int64_t playheadUs);
    // Per-track pool-index range covering [fromUs, toUs] via the sampled index.
    void trackWindow(int track, int64_t fromUs, int64_t toUs,
                     size_t& outFirst, size_t& outLast) const;
    // Coarse events[]-position for a time (floor of the sampled global index).
    size_t coarsePosOf(int64_t us) const;
    void resetWindowLocked(int64_t aroundUs);

    // ---- mapping ----
    uint8_t* base_    = nullptr;   // first segment's base; non-null once reserved
    // Every mapped piece of the pool, ascending by `off`. Exactly one entry on
    // a 64-bit build (see PoolSeg).
    std::vector<PoolSeg> segs_;
    size_t   mapLen_  = 0;         // page-rounded mapping length

    // Reserve mapLen_ bytes of address space for the pool, as one piece where
    // possible and as several where a 32-bit process has no single run big
    // enough. Fills segs_ and base_.
    bool reserveSegments(size_t total);
    // The placement work behind it, reusable for pricing the load's own
    // allocations before the pool commits the address space they need.
    static bool reserveInto(std::vector<PoolSeg>& out, size_t total);
    uint8_t* poolAddr(size_t off) const { return poolAddrIn(segs_, off); }
    size_t   poolOffOf(const void* p) const { return poolOffIn(segs_, p); }
    // Call fn(addr, len) for each segment overlapping pool bytes
    // [firstByte, lastByte]. A range that spans a segment boundary becomes two
    // calls; madvise and friends cannot cross one.
    template <class F>
    void forEachSegRange(size_t firstByte, size_t lastByte, F&& fn) const {
        for (const PoolSeg& s : segs_) {
            size_t a = firstByte > s.off ? firstByte : s.off;
            size_t end = s.off + s.len - 1;
            size_t b = lastByte < end ? lastByte : end;
            if (a > b) continue;
            fn(s.va + (a - s.off), b - a + 1);
        }
    }
    size_t   poolBytes_ = 0;       // exact pool size = totalEvents_ * sizeof(PlayEvent)
    bool     diskFull_  = false;   // last open() aborted by the free-space guard
    bool     fileTooBig_ = false;  // last open() hit the volume's file-size ceiling
    bool     noAddrSpace_ = false; // last open() could not reserve the pool's VA range
    bool     needsChunked_ = false;// ...but the chunked sort would have fit
    size_t   totalEvents_ = 0;
    // Named temps on an SD-card pool dir, unlinked by close(). Empty on the
    // internal path, which unlinks at creation (see makeTemp).
    std::vector<std::string> tempPaths_;

    // ---- resident tables ----
    std::vector<uint32_t>    sisterPos_;     // per events[] position (see above)
    std::vector<TrackRange>  trackRange_;    // pool-index span per track
    std::vector<uint32_t>    trackSampleOff_;// per-track offset into trackSamples_
    std::vector<TrackSample> trackSamples_;  // every kTrackSampleStep events
    std::vector<int64_t>     posTimes_;      // events[] time every kPosSampleStep
    const std::vector<PlayEvent*>* events_ = nullptr;  // out.events (stable after open)

    // ---- loader ----
    std::thread          loaderThread_;
    std::atomic<bool>    loaderRunning_{false};
    std::atomic<uint64_t> loaderMajFlt_{0};   // see loaderMajFlt()
    const std::atomic<int64_t>* playheadUs_ = nullptr;
    int64_t              initialUs_ = 0;
    uint64_t             engineCpuMask_ = 0;
    int64_t              frontUs_ = 0, backUs_ = 0;   // window horizons
    int64_t              visibleUs_ = 0;    // buildVisible's band (setVisibleUs)
    bool                 frontPinned_ = false;  // APFA_WIN_FRONT_US was set
    // Published for the "win:" line — see windowFrontUs() and friends.
    std::atomic<int64_t>  winFrontUs_{0};
    std::atomic<uint64_t> winBytes_{0};
    std::atomic<uint64_t> memAvail_{0};
    // Loader state — guarded by mutex_ (loader tick, warmSeek; never the
    // engine hot path).
    mutable std::mutex   mutex_;
    std::vector<size_t>  trackLo_, trackHi_;   // warmed byte range per track
    size_t               frontPos_ = 0;        // events[] positions already scanned
    size_t               backPos_  = 0;
    size_t               adviseCursor_ = 0;    // rotating track start (see tick)
    // Pages of note-ons that are still sounding after the window has moved
    // past them (long notes): page address -> events[] position of the
    // note-off, after which the pin is dropped. Re-touched every tick so
    // neither our DONTNEED nor kernel reclaim cools them under the O(P) scan.
    std::unordered_map<uintptr_t, uint32_t> pinnedPages_;

    static constexpr size_t  kTrackSampleStep = 4096;    // events per track sample
    static constexpr size_t  kPosSampleStep   = 65536;   // events per global sample

    // Latency lead added to the visible band to form the front horizon: enough
    // for an advisory issued this tick to have landed before the playhead
    // reaches it, across a few 100 ms ticks of UFS/eMMC round trips. The
    // horizon is therefore ~2.15 s at the default note speed and ~5 s at the
    // maximum, against beta1's flat 30 s.
    static constexpr int64_t kLeadUs         = 2000000;   // 2 s
    // Floor for the adaptive shrink. Below the visible band buildVisible would
    // fault on every frame, so the budget is allowed to lose here — a stretched
    // frame is the intended failure mode (see the header note).
    static constexpr int64_t kMinFrontUs     = 500000;    // 0.5 s
    // Behind the playhead only the O(P) scan reads, and pinnedPages_ takes
    // over the moment the back edge passes a still-sounding note-on — so this
    // only has to cover notes shorter than one back-edge crossing.
    static constexpr int64_t kDefaultBackUs  = 3000000;   // 3 s
    // Share of MemAvailable the sliding window may price out at. The rest of
    // the headroom belongs to the resident tables, BASS, and the GL driver.
    static constexpr double  kWindowMemFraction = 0.25;
    // Ceiling on advisories issued per 100 ms tick across all tracks. Steady
    // state never approaches it (the front edge advances one tick of events);
    // it bounds the initial fill and post-seek refill so a density spike
    // cannot hand the kernel a burst it will only evict.
    static constexpr size_t  kMaxAdviseBytesPerTick = 32u << 20;   // 32 MB
    // Ceiling on scattered note-off pre-touches per tick (see the front-edge
    // scan). Each is a synchronous single-page read; 4096 of them is ~16 MB of
    // scattered I/O, already well past what a 100 ms tick can absorb.
    static constexpr size_t  kMaxSisterTouchPerTick = 4096;

    // ---- sliced pool state (32-bit only) ------------------------------------
    bool     sliced_   = false;
    // The pool: open for the whole session, and never mapped. ONE file on a
    // 64-bit build, always. On 32-bit it becomes a chain of sub-2 GB links once
    // the pool passes ~1.5 GB, which is where a 32-bit off_t would start
    // overflowing — see poolFileBytes() in streamer.cpp.
    std::vector<int> poolFds_;
    size_t   fullPoolBytes_ = 0;    // its size (poolBytes_ is the mapped slice's)
    // Per events[] position, resident for the session. Between them these
    // replace every dereference the engine used to make outside the playhead's
    // neighbourhood, which is what makes an unmapped pool workable at all.
    std::vector<uint32_t> poolIdx_;   // -> index in the full pool (parse order)
    std::vector<uint32_t> posUs_;     // -> absMicroSec (the parser caps it at 2^32-1)
    std::vector<uint32_t> pcData_;    // packed (code, param1, param2) per pcIdx entry
    int64_t  firstNoteUs_ = 0;
    // The two arenas slices alternate between, so the next slice can be built
    // and mapped while the current one is still playing and the transition is a
    // table swap rather than an mmap and a fault storm. Slice k always uses
    // arena_[k & 1].
    uint8_t* arena_[2] = { nullptr, nullptr };
    size_t   arenaBytes_ = 0;
    std::vector<SlicePlan> plan_;
    SliceMap cur_;                    // mapped and playing
    // False while an arena is being taken down and rebuilt (a seek that lands
    // on the other parity). The loader must not advise — let alone touch — an
    // arena that is not backed by a slice.
    bool     sliceLive_ = false;
    std::vector<PlayEvent*>* eventsMut_ = nullptr;   // out.events, to repoint
    std::string poolDirUsed_;         // where slice temps are created
    bool     poolKeepNamed_ = false;  // ...and whether they keep their names

    // Builder thread: materialises the next slice off the engine thread. If it
    // has not finished when the playhead arrives, advanceSlice() blocks — the
    // stall is ordinary storage lag entering the clock, which is what PFA does.
    std::thread             buildThread_;
    std::mutex              buildMutex_;
    std::condition_variable buildCv_;
    int      buildWant_  = -1;        // slice index requested (-1 = nothing)
    int      buildState_ = 0;         // 0 idle, 1 running, 2 ready, 3 failed
    bool     buildQuit_  = false;
    std::atomic<bool> buildAbort_{false};   // give up mid-build so close() is prompt
    SliceMap buildOut_;               // the finished slice, waiting to be installed

    void startBuilder();
    void stopBuilder();
    void builderMain();
    void requestBuild(int sliceIdx);
    // Wait for the in-flight build and take it. Returns false if there was
    // none, it failed, or it was for a different slice.
    bool takeBuild(int sliceIdx, SliceMap& out);
    // Materialise plan_[sliceIdx] into arena_[sliceIdx & 1]: work out the
    // carry sets and the read-ahead extension, lay the events out track by
    // track, bake the sister pointers against the arena base, write the file
    // and map it. Runs on the builder thread (and, for a seek, on the caller's).
    bool buildSlice(int sliceIdx, SliceMap& out);
    // Point events[] and the loader's tables at `s`, which must already be
    // mapped. Caller holds mutex_.
    void installSliceLocked(SliceMap& s, int64_t playheadUs);
    // Drop a slice's mapping (PROT_NONE back over its arena) and its temp file.
    void retireSlice(SliceMap& s);
    size_t arenaEvents() const { return arenaBytes_ / sizeof(PlayEvent); }

    // Address space per arena. Big enough that a slice is worth building (tens
    // of seconds of playback) and that it comfortably covers the loader's
    // read-ahead horizon — measured at 56 MB, peaking at 73.5 MB, on the 12M-note
    // load — with room for the carry sets on top. Two of these is the entire
    // address-space cost of a sliced load, whatever the song's length.
    static constexpr size_t kArenaTargetBytes = 192u << 20;
    static constexpr size_t kArenaMinBytes    = 64u << 20;
    // Share of the arena planSlices may fill. The rest absorbs the read-ahead
    // extension past endPos and the carry that comes with it.
    static constexpr size_t kSliceBudgetNum = 3, kSliceBudgetDen = 4;
    // Floor on a slice's body, so a note-wall whose carry alone fills the arena
    // still makes progress instead of emitting empty slices.
    static constexpr uint32_t kSliceMinEvents = 65536;
    // Materialiser read buffer, and the largest run of unwanted events it will
    // read over rather than starting a new pread. 4096 events is 224 KB — past
    // the point where one more seek is cheaper than the bytes.
    static constexpr size_t kSliceChunkEvents = 65536;
    static constexpr size_t kSliceGapEvents   = 4096;
};

}  // namespace apfa

#endif  // APFA_STREAMING
