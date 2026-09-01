// streamer.cpp — see streamer.h.
//
// The parse walks the MIDI twice with ONE shared track walker (walkTracks), so
// the two passes cannot disagree about what gets emitted:
//
//   Pass A (skim)  — counts emissions per track, collects the tempo map and
//                    the note-bearing (track,channel) set. Nothing allocated
//                    per event.
//   Pass B (emit)  — re-parses, appending finished PlayEvents (µs times,
//                    sister pointers against the reserved base) to the pool
//                    file through a sequential write buffer. Note pairs whose
//                    note-on already left the buffer become fixups.
//   Pass C (patch) — applies the fixups to the pool file in sorted chunks.
//   Pass D (sort)  — builds the time-sorted events[] exactly like
//                    midi_parser.cpp's stable_sort (same keys, same tie-break
//                    = pool order), plus programChangeIdx, sisterPos, and the
//                    sampled time indexes the loader navigates by.
//
// Every semantic here (pairing stacks, end-of-track FIFO close-out, the
// non-note sister = &pool[0] artifact, the µs cap, the sort comparator) is a
// deliberate copy of midi_parser.cpp — the host parity harness diffs the two
// outputs field by field.
#ifdef APFA_STREAMING

#include "streamer.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>   // srand/getenv
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <queue>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

#include "platform.h"

namespace apfa {
namespace {

constexpr size_t kPageSize   = 4096;
constexpr size_t kBufEvents  = 131072;   // pass-B write buffer (~9 MB)
constexpr size_t kEventSize  = sizeof(PlayEvent);
constexpr size_t kRunEntries = 2097152;  // sort-run spill threshold (32 MB)
constexpr size_t kPairBufEntries = 1048576;  // pair spill threshold (8 MB)
constexpr size_t kMergeBudget = 64 << 20;    // total merge read-buffer RAM

// ---- inv[] windowing (pass D; see the sisterPos block) ----------------------
// The reserve keeps the window clear of everything else still live at pass D —
// the pool pages the merge just dirtied, the pair chunk, sisterPos_ itself —
// so the window does not simply move the kill a few seconds later. The floor
// stops a device under momentary pressure from picking a window so small the
// pass count (and with it the rescans of the forward map) explodes.
constexpr uint64_t kInvWindowReserve = 96u << 20;   // 96 MB
constexpr size_t   kInvWindowMin     = 8u << 20;    // 8 M entries = 32 MB
// Share of the post-reserve headroom the window may take. Half, because pass D
// is not alone in there: the pool pages the merge just dirtied are still going
// out, and sizing the window to every last free byte only moves the kill later
// instead of preventing it.
constexpr double   kInvWindowFraction = 0.5;
// Marks a pair field that already holds an events[] position rather than a
// pool index. Safe as a flag because both are < totalEvents, and a MIDI with
// 2^31 events is far past every other ceiling in this file.
constexpr uint32_t kInvResolvedBit   = 0x80000000u;

// ---- pool segmentation (see PoolSeg in streamer.h) --------------------------
// A segment boundary has to be BOTH a page boundary (mmap file offsets are
// page-granular) and a PlayEvent boundary (an event split across two mappings
// would be unreadable), so pieces are sized in multiples of the two's lowest
// common multiple: 28 KB for a 56-byte event, 36 KB for a 72-byte one.
constexpr size_t gcdConst(size_t a, size_t b) { return b == 0 ? a : gcdConst(b, a % b); }
constexpr size_t kSegGrain = (kEventSize / gcdConst(kEventSize, kPageSize)) * kPageSize;
// Don't bother with slivers, and keep the count low: poolAddr scans the list,
// and it is called once per event while the pointers are being baked in.
constexpr size_t kSegMin   = 8u << 20;
constexpr size_t kMaxSegs  = 32;

int64_t envUs(const char* name, int64_t fallback) {
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    long long x = strtoll(v, &end, 10);
    return (end && *end == '\0' && x > 0) ? static_cast<int64_t>(x) : fallback;
}

// ---- the pool is a CHAIN of files on 32-bit, ONE file on 64-bit ---------------
// **32-bit only**, the same stance the sliced pool takes: a 64-bit build never
// splits, no exceptions. Everything below is about a limit that only exists in
// a 32-bit process.
//
// `off_t` is 32 bits in a 32-bit Android process, so any positioned read or
// write past 2 GB overflows to a NEGATIVE offset and fails outright: a 2107 MB
// pool died in pass C with "fixup pread failed" on an LG X410 loading a 19M-note
// MIDI, ~97% of the way through the fixups.
//
// `_FILE_OFFSET_BITS=64` is not the way out at this minSdk — it remaps mmap to
// mmap64, which bionic only exports from API 21, and one unresolved symbol makes
// the old loader reject the whole library before any of it runs (engine.cpp
// documents the same trap for sched_setaffinity). Splitting the pool into files
// that each stay well inside 31 bits keeps every offset the kernel ever sees
// legal on every API level, with no new libc symbols and no syscall wrappers.
//
// The size is a multiple of kSegGrain, so a PlayEvent never straddles two files
// AND every boundary is page-aligned — which is what lets the non-sliced path go
// on mapping the pool over its reservation. None of this is on the hot path: the
// playback walk dereferences the mapped arena, and only the load passes and the
// slice builder ever address the pool by offset.
constexpr size_t kPoolFileDefault = ((static_cast<size_t>(1536) << 20) / kSegGrain)
                                    * kSegGrain;

// APFA_POOL_FILE_BYTES shrinks a link so a small test MIDI needs a whole chain
// (tests/slicepool_test.cpp). Host-only in practice, exactly like
// APFA_ARENA_BYTES: a normally-launched Android app never sees shell env vars.
// Rounded down to kSegGrain so the alignment invariants above still hold.
inline size_t poolFileBytes() {
    static const size_t v = [] {
        // 64-bit: off_t is 8 bytes, there is no 2 GB wall, and the pool stays
        // ONE file exactly as it always was. Said as a link larger than any
        // pool can be, so poolFileCount, poolRW and the mmap loop each collapse
        // to the single-file case with no branch and no behaviour change — and
        // so APFA_POOL_FILE_BYTES is inert here, like APFA_FORCE_SLICED is.
        if (sizeof(off_t) > 4) return (~static_cast<size_t>(0)) >> 1;
        const int64_t want = envUs("APFA_POOL_FILE_BYTES", 0);
        if (want <= 0) return kPoolFileDefault;
        // Clamp rather than fall back: a request below one grain means "as
        // small as legal", which is what a test wants. Silently returning the
        // 1.5 GB default there would make the chain quietly untested.
        const size_t r = (static_cast<size_t>(want) / kSegGrain) * kSegGrain;
        return r ? r : kSegGrain;
    }();
    return v;
}

inline size_t poolFileCount(uint64_t poolBytes) {
    const size_t link = poolFileBytes();
    return static_cast<size_t>((poolBytes + link - 1) / link);
}

// Positioned pool I/O across the chain. `off` is a byte offset into the whole
// pool; what reaches pread/pwrite is always < poolFileBytes(). Transfers that
// straddle a boundary are split, so no caller has to know the chain exists.
bool poolRW(const std::vector<int>& fds, void* buf, size_t bytes,
            uint64_t off, bool doWrite) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    const size_t link = poolFileBytes();
    while (bytes > 0) {
        const size_t idx = static_cast<size_t>(off / link);
        if (idx >= fds.size()) return false;
        const off_t  in  = static_cast<off_t>(off % link);
        const size_t n   = std::min(bytes, link - static_cast<size_t>(in));
        const ssize_t got = doWrite ? pwrite(fds[idx], p, n, in)
                                    : pread (fds[idx], p, n, in);
        if (got <= 0) return false;
        p     += got;
        bytes -= static_cast<size_t>(got);
        off   += static_cast<uint64_t>(got);
    }
    return true;
}
inline bool poolPread(const std::vector<int>& fds, void* b, size_t n, uint64_t o) {
    return poolRW(fds, b, n, o, false);
}
inline bool poolPwrite(const std::vector<int>& fds, const void* b, size_t n,
                       uint64_t o) {
    return poolRW(fds, const_cast<void*>(b), n, o, true);
}

// Bounds-checked big-endian byte cursor — verbatim from midi_parser.cpp.
struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    uint8_t u8() {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }
    uint32_t u16() { uint32_t a = u8(), b = u8(); return (a << 8) | b; }
    uint32_t u32() {
        uint32_t a = u8(), b = u8(), c = u8(), d = u8();
        return (a << 24) | (b << 16) | (c << 8) | d;
    }
    uint32_t varlen() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            uint8_t c = u8();
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) break;
        }
        return v;
    }
    void skip(uint32_t n) {
        if (p + n > end) { p = end; ok = false; } else { p += n; }
    }
};

struct TempoEvent { uint32_t tick; uint32_t usPerQuarter; };
struct TempoSeg   { uint32_t tick; uint64_t usAtTick; uint32_t usPerQuarter; };

// The one track walker both passes share. `sink` receives exactly the
// emissions midi_parser.cpp performs, in the same order:
//   uint32_t noteOn(track, tick, ch, key, vel)  -> token for the pairing stack
//   void     noteOff(track, tick, ch, key, onToken)
//   void     channelEvent(track, tick, status, p1, p2)
//   void     tempo(tick, usPerQuarter)
//   void     trackDone(track)
// Pairing (LIFO within the track, FIFO close-out at track end) lives here so
// it is decided once. Mirrors midi_parser.cpp's track loop line for line.
template <class Sink>
void walkTracks(const uint8_t* afterHeader, const uint8_t* fileEnd,
                uint32_t numTracks, Sink& sink,
                std::atomic<float>& progress, float p0, float p1,
                int& maxTrackOut) {
    static thread_local std::vector<uint32_t> pending[16][128];
    const uint8_t* cur = afterHeader;
    int maxTrack = 0;

    for (uint32_t t = 0; t < numTracks && cur + 8 <= fileEnd; t++) {
        if (!(cur[0] == 'M' && cur[1] == 'T' && cur[2] == 'r' && cur[3] == 'k')) break;
        uint32_t trkLen = (uint32_t(cur[4]) << 24) | (uint32_t(cur[5]) << 16) |
                          (uint32_t(cur[6]) << 8)  |  uint32_t(cur[7]);
        cur += 8;
        const uint8_t* trkEnd = cur + trkLen;
        if (trkEnd > fileEnd) trkEnd = fileEnd;
        Reader r{ cur, trkEnd };

        for (int c = 0; c < 16; c++)
            for (int k = 0; k < 128; k++) pending[c][k].clear();

        uint32_t absTick = 0;
        uint8_t  running = 0;

        while (r.ok && r.p < r.end) {
            absTick += r.varlen();
            uint8_t status = r.u8();
            if (!r.ok) break;
            if (status < 0x80) {            // running status: re-use last status
                r.p--;                      // the byte just read is data, not status
                status = running;
                if (status < 0x80) break;
            } else {
                running = status;
            }
            uint8_t hi = status & 0xF0;
            int     ch = status & 0x0F;

            if (hi == 0x90) {               // note on
                uint8_t key = r.u8() & 0x7F;
                uint8_t vel = r.u8() & 0x7F;
                if (vel > 0) {
                    pending[ch][key].push_back(
                        sink.noteOn(static_cast<int>(t), absTick, ch, key, vel));
                } else if (!pending[ch][key].empty()) {   // vel 0 = note off
                    sink.noteOff(static_cast<int>(t), absTick, ch, key,
                                 pending[ch][key].back());
                    pending[ch][key].pop_back();
                }
            } else if (hi == 0x80) {        // note off
                uint8_t key = r.u8() & 0x7F;
                r.u8();                     // release velocity (ignored)
                if (!pending[ch][key].empty()) {
                    sink.noteOff(static_cast<int>(t), absTick, ch, key,
                                 pending[ch][key].back());
                    pending[ch][key].pop_back();
                }
            } else if (hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
                uint8_t p1b = r.u8(), p2b = r.u8();
                sink.channelEvent(static_cast<int>(t), absTick, status, p1b, p2b);
            } else if (hi == 0xC0 || hi == 0xD0) {
                uint8_t p1b = r.u8();
                sink.channelEvent(static_cast<int>(t), absTick, status, p1b, 0);
            } else if (status == 0xFF) {    // meta event
                uint8_t  metaType = r.u8();
                uint32_t len      = r.varlen();
                if (metaType == 0x51 && len == 3) {
                    uint8_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
                    sink.tempo(absTick,
                               (uint32_t(b0) << 16) | (uint32_t(b1) << 8) | b2);
                } else {
                    r.skip(len);
                }
            } else if (status == 0xF0 || status == 0xF7) {  // sysex
                r.skip(r.varlen());
            } else {
                break;                      // malformed
            }
        }

        // notes still held at track end: terminate them there (FIFO — matches
        // midi_parser.cpp's range-for over the pending vector)
        for (int c = 0; c < 16; c++)
            for (int k = 0; k < 128; k++)
                for (uint32_t onTok : pending[c][k])
                    sink.noteOff(static_cast<int>(t), absTick, c, k, onTok);

        sink.trackDone(static_cast<int>(t));
        maxTrack = static_cast<int>(t);
        cur = trkEnd;
        progress.store(p0 + (p1 - p0) * float(t + 1) /
                            float(numTracks ? numTracks : 1));
    }
    maxTrackOut = maxTrack;
}

// Pass A: count emissions and gather metadata. Tokens are dummies — the
// pending stacks only need the right depths for the walker's pairing.
struct SkimSink {
    std::vector<TempoEvent> tempos;
    std::vector<size_t>     trackCounts;   // emissions per track index
    std::vector<uint8_t>    hasNotes16;    // (track*16 + ch) bitmap, grown on demand
    size_t noteCount = 0;

    void ensureTrack(int t) {
        if (trackCounts.size() <= static_cast<size_t>(t)) trackCounts.resize(t + 1, 0);
        if (hasNotes16.size() < static_cast<size_t>(t + 1) * 16)
            hasNotes16.resize(static_cast<size_t>(t + 1) * 16, 0);
    }
    uint32_t noteOn(int t, uint32_t, int ch, int, int) {
        ensureTrack(t);
        trackCounts[t]++;
        noteCount++;
        hasNotes16[static_cast<size_t>(t) * 16 + ch] = 1;
        return 0;
    }
    void noteOff(int t, uint32_t, int, int, uint32_t) { ensureTrack(t); trackCounts[t]++; }
    void channelEvent(int t, uint32_t, uint8_t, uint8_t, uint8_t) { ensureTrack(t); trackCounts[t]++; }
    void tempo(uint32_t tick, uint32_t uspq) { tempos.push_back({ tick, uspq }); }
    void trackDone(int t) { ensureTrack(t); }
};

// Fixup: a note-on that had already left the write buffer when its note-off
// was emitted; its sister pointer is patched into the file by pass C.
struct Fixup { uint32_t onIdx, offIdx; };
struct Pair  { uint32_t onIdx, offIdx; };
struct SortKey { uint64_t key; uint32_t idx; };

// (µs, track, chType) packed key; idx tie-break = parse order. ONE comparator
// shared by the chunked run sort and the un-chunked whole-table sort so the
// total event order can never diverge between the two modes.
inline bool sortKeyLess(const SortKey& a, const SortKey& b) {
    if (a.key != b.key) return a.key < b.key;
    return a.idx < b.idx;
}

// Free-space guard ("Not enough space on disk"): never fill the pool/spill
// volume past 98% — a load that would strand the phone at 0 B free aborts
// instead, and Streamer::diskFull() routes the right message to the UI.
// Checked once up front with the predicted total, then re-checked as the
// files grow (one fstatvfs per ~multi-MB write, cost is noise).
constexpr double kMinFreeDiskFraction = 0.02;

bool wouldExhaustDisk(int fd, uint64_t moreBytes) {
    struct statvfs vfs;
    if (fstatvfs(fd, &vfs) != 0) return false;   // can't tell — don't block the load
    uint64_t frsize = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
    uint64_t total  = static_cast<uint64_t>(vfs.f_blocks) * frsize;
    uint64_t avail  = static_cast<uint64_t>(vfs.f_bavail) * frsize;
    return avail < moreBytes + static_cast<uint64_t>(total * kMinFreeDiskFraction);
}

// Append `len` bytes, riding out short writes. False on error (ENOSPC etc.).
bool writeAll(int fd, const void* p, size_t len) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    while (len > 0) {
        ssize_t n = write(fd, b, len);
        if (n <= 0) return false;
        b += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Prefix every temp this file creates shares, so a crashed load's litter is
// recognisable to sweepStaleTemps (and to a curious user with a file manager).
constexpr char kTempPrefix[] = "apfa_";

// mkstemp in `dir`. keepPath == nullptr unlinks immediately — the fd is then
// the only handle, so the space is reclaimed automatically on close or on a
// crash. That is the internal-cache path. With keepPath the name is kept and
// returned instead, for volumes where an unlinked-but-mapped file is not a
// safe bet (see the SD-card note in streamer.h); the caller owns the unlink.
int makeTemp(const std::string& dir, const char* tag, std::string* keepPath) {
    std::string tmpl = dir + "/" + kTempPrefix + tag + "_XXXXXX";
    std::vector<char> nameBuf(tmpl.begin(), tmpl.end());
    nameBuf.push_back('\0');
    int fd = mkstemp(nameBuf.data());
    if (fd < 0) return fd;
    if (keepPath) *keepPath = nameBuf.data();
    else          unlink(nameBuf.data());
    return fd;
}

// Delete pool temps a previous run left behind. Only the named (SD-card) path
// can litter — a crash mid-load leaves multi-GB files that nothing else will
// ever reclaim. Safe to call on a directory that has none.
void sweepStaleTemps(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    size_t swept = 0;
    while (struct dirent* e = readdir(d)) {
        if (strncmp(e->d_name, kTempPrefix, sizeof(kTempPrefix) - 1) != 0) continue;
        if (unlink((dir + "/" + e->d_name).c_str()) == 0) swept++;
    }
    closedir(d);
    if (swept) LOGI("streamer: swept %zu stale pagefile temp(s) from %s",
                    swept, dir.c_str());
}

// Can this directory back the pool? The whole design rests on mapping the
// finished file PROT_READ|MAP_PRIVATE, and removable volumes reach us through
// FUSE, where that is not guaranteed (a direct_io file handle refuses mmap
// outright). Rather than discover it three passes and several GB in, create a
// one-page file, map it, and read it back. Costs one page of I/O.
bool poolDirUsable(const std::string& dir) {
    std::string path;
    int fd = makeTemp(dir, "probe", &path);
    if (fd < 0) {
        LOGE("streamer: cannot create files in %s (errno=%d)", dir.c_str(), errno);
        return false;
    }
    std::vector<uint8_t> page(kPageSize, 0xA5);
    bool ok = writeAll(fd, page.data(), page.size());
    if (!ok) LOGE("streamer: probe write failed in %s (errno=%d)", dir.c_str(), errno);
    if (ok) {
        void* m = mmap(nullptr, kPageSize, PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) {
            LOGE("streamer: %s cannot back a file mapping (errno=%d)",
                 dir.c_str(), errno);
            ok = false;
        } else {
            ok = (*static_cast<const volatile uint8_t*>(m) == 0xA5);
            munmap(m, kPageSize);
            if (!ok) LOGE("streamer: probe mapping read back wrong in %s", dir.c_str());
        }
    }
    ::close(fd);
    unlink(path.c_str());
    return ok;
}

// FAT32 stops at 4 GB per file — well under a U11-class pagefile, and the
// default format on plenty of SD cards. Where the kernel reports the real
// filesystem we refuse up front instead of dying at 4 GB with three passes
// spent; Android's FUSE layer reports its own magic and hides the answer, so
// there the EFBIG path in EmitSink is what catches it.
constexpr uint64_t kFat32MaxFileBytes = 0xFFFFFFFFull;   // 4 GB - 1

bool exceedsFileSizeLimit(int fd, uint64_t bytes) {
    struct statfs sfs;
    if (fstatfs(fd, &sfs) != 0) return false;
    return static_cast<unsigned long>(sfs.f_type) == 0x4d44ul &&   // MSDOS_SUPER_MAGIC
           bytes > kFat32MaxFileBytes;
}

// A CC / ProgramChange / PitchBend event's payload, kept beside its pool index
// during pass B. The sliced path needs these three bytes resident: on a seek,
// playSkippedEvents walks every controller event behind the playhead, and in a
// sliced load almost none of them are mapped.
struct PcRaw {
    uint32_t idx;
    uint8_t  code, p1, p2, _pad;
};

// Pass B: emit finished PlayEvents through a sequential write buffer.
struct EmitSink {
    // configured by open()
    const std::vector<int>* fds = nullptr;   // the pool chain (see poolFileBytes())
    uint64_t written = 0;                    // byte cursor across the whole chain
    const std::vector<PoolSeg>* poolSegs = nullptr;
    const std::vector<TempoSeg>* segs = nullptr;
    int      ticksPerQuarter = 480;
    size_t   trackSampleStep = 4096;

    // Sliced loads never map the pool, so there is no address to bake into
    // `sister`: store the partner's POOL INDEX + 1 (0 = none) in the same field
    // instead and let the slice materialiser turn it into an arena address.
    // Same struct, same 56 bytes, same passes.
    bool     encodeIdx = false;
    PlayEvent* sisterAt(size_t poolIdx) const {
        if (encodeIdx)
            return reinterpret_cast<PlayEvent*>(static_cast<uintptr_t>(poolIdx) + 1);
        return reinterpret_cast<PlayEvent*>(poolAddrIn(*poolSegs, poolIdx * kEventSize));
    }
    // sister = &pool[0]: midi_parser.cpp's final index->pointer pass turns the
    // parse-time nullptr (index 0) into a pointer at the pool base for every
    // non-note event. Reproduce the artifact exactly; sliced loads encode it as
    // 0 and the materialiser resolves it to the arena base.
    PlayEvent* sisterNone() const {
        if (encodeIdx) return nullptr;
        return reinterpret_cast<PlayEvent*>(poolAddrIn(*poolSegs, 0));
    }

    // outputs
    std::vector<Fixup>   fixups;         // cross-buffer sister patches (RAM; rare)
    std::vector<PcRaw>   pcRaw;          // sliced loads only (see PcRaw)
    std::vector<int64_t>  sampleUs;      // flattened per-track (µs, poolIdx) samples
    std::vector<uint32_t> sampleIdx;
    std::vector<uint32_t> sampleTrackOff; // per-track start into sampleUs/sampleIdx
    uint64_t totalUs = 0;
    uint32_t nextIdx = 0;
    bool     ioError  = false;
    bool     diskFull = false;           // ioError caused by the free-space guard
    bool     fileTooBig = false;         // ioError caused by EFBIG (FAT32's 4 GB)

    // Every write failure lands here so the two storage conditions the UI can
    // actually act on stay distinguishable from a generic I/O error.
    void noteWriteError() {
        ioError = true;
        if (errno == EFBIG) fileTooBig = true;
    }

    // Sort keys. chunked=false (the automatic path): runBuf holds EVERY key
    // until pass D — 16 B/event resident, the load transient that caps
    // un-chunked streaming at ~80 M notes on an 8 GB phone (it's what lmkd
    // killed at 78% on NoK 90M). chunked=true ("Chunked Disk Streaming"):
    // keys spill to disk in pre-sorted 32 MB runs and a k-way merge in pass D
    // consumes them — no transient, storage-bound only.
    bool     chunked = false;
    int      runsFd = -1;
    std::vector<SortKey>  runBuf;
    std::vector<uint64_t> runStarts;     // first entry index of each run
    uint64_t runEntries = 0;
    // Note pairs: same split (8 B/note resident, or spilled and streamed back
    // to build sisterPos).
    int      pairsFd = -1;
    std::vector<Pair> pairBuf;
    uint64_t pairCount = 0;

    // write buffer
    std::vector<PlayEvent> buf;
    uint32_t bufStart = 0;
    size_t   inTrackCount = 0;

    void spillRun() {
        if (runBuf.empty() || ioError) return;
        std::sort(runBuf.begin(), runBuf.end(), sortKeyLess);
        if (wouldExhaustDisk(runsFd, runBuf.size() * sizeof(SortKey))) {
            diskFull = ioError = true;
            return;
        }
        if (!writeAll(runsFd, runBuf.data(), runBuf.size() * sizeof(SortKey)))
            noteWriteError();
        runStarts.push_back(runEntries);
        runEntries += runBuf.size();
        runBuf.clear();
    }
    void spillPairs() {
        if (pairBuf.empty() || ioError) return;
        if (wouldExhaustDisk(pairsFd, pairBuf.size() * sizeof(Pair))) {
            diskFull = ioError = true;
            return;
        }
        if (!writeAll(pairsFd, pairBuf.data(), pairBuf.size() * sizeof(Pair)))
            noteWriteError();
        pairCount += pairBuf.size();
        pairBuf.clear();
    }

    uint64_t tickToUs(uint32_t tick) const {
        const std::vector<TempoSeg>& s = *segs;
        size_t lo = 0, hi2 = s.size();
        while (lo + 1 < hi2) {
            size_t mid = (lo + hi2) / 2;
            if (s[mid].tick <= tick) lo = mid; else hi2 = mid;
        }
        return s[lo].usAtTick +
               uint64_t(tick - s[lo].tick) * s[lo].usPerQuarter / ticksPerQuarter;
    }

    void flush() {
        if (buf.empty() || ioError) { bufStart = nextIdx; buf.clear(); return; }
        const size_t bytes = buf.size() * kEventSize;
        // Any fd in the chain answers statfs for the volume they all share.
        if (wouldExhaustDisk((*fds)[0], bytes)) {
            diskFull = ioError = true;
            bufStart = nextIdx; buf.clear();
            return;
        }
        // Positioned rather than sequential now that the pool spans several
        // files: `written` is the cursor across the chain, and poolPwrite splits
        // the transfer if it crosses a file boundary.
        if (poolPwrite(*fds, buf.data(), bytes, written)) written += bytes;
        else                                              noteWriteError();
        buf.clear();
        bufStart = nextIdx;
    }

    // Shared tail for every emission: time key, sample index, totalUs.
    uint32_t emit(const PlayEvent& e, int track, uint32_t tick, int chType) {
        uint32_t idx = nextIdx++;
        uint64_t us  = tickToUs(tick);
        if (us > 0xFFFFFFFFull) us = 0xFFFFFFFFull;   // parser's per-event cap
        if (us > totalUs) totalUs = us;
        PlayEvent out = e;
        out.absMicroSec = static_cast<int64_t>(us);
        buf.push_back(out);
        if (buf.size() >= kBufEvents) flush();
        runBuf.push_back({ (us << 19) |
                           (static_cast<uint64_t>(track) << 3) |
                           static_cast<uint64_t>(14 - chType), idx });
        if (chunked && runBuf.size() >= kRunEntries) spillRun();
        if (inTrackCount % trackSampleStep == 0) {
            sampleUs.push_back(static_cast<int64_t>(us));
            sampleIdx.push_back(idx);
        }
        inTrackCount++;
        return idx;
    }

    uint32_t noteOn(int t, uint32_t tick, int ch, int key, int vel) {
        int c = ch & 0x0F;
        PlayEvent e{ 0, 0x90 | c, t, 0, static_cast<int32_t>(tick),
                     0 /*µs set in emit*/, kNoteOn, 0,
                     static_cast<uint8_t>(c), static_cast<uint8_t>(key & 0x7F),
                     static_cast<uint8_t>(vel & 0x7F),
                     sisterNone() /*patched by noteOff*/,
                     0, nullptr };
        return emit(e, t, tick, kNoteOn);
    }

    void noteOff(int t, uint32_t tick, int ch, int key, uint32_t onIdx) {
        int c = ch & 0x0F;
        PlayEvent e{ 0, 0x80 | c, t, 0, static_cast<int32_t>(tick),
                     0, kNoteOff, 0,
                     static_cast<uint8_t>(c), static_cast<uint8_t>(key & 0x7F), 0,
                     sisterAt(onIdx),
                     0, nullptr };
        uint32_t offIdx = emit(e, t, tick, kNoteOff);
        pairBuf.push_back({ onIdx, offIdx });
        if (chunked && pairBuf.size() >= kPairBufEntries) spillPairs();
        PlayEvent* sisterPtr = sisterAt(offIdx);
        if (onIdx >= bufStart) {
            buf[onIdx - bufStart].sister = sisterPtr;
        } else {
            fixups.push_back({ onIdx, offIdx });
        }
    }

    void channelEvent(int t, uint32_t tick, uint8_t status, uint8_t p1, uint8_t p2) {
        int c    = status & 0x0F;
        int type = status >> 4;
        PlayEvent e{ 0, status, t, 0, static_cast<int32_t>(tick),
                     0, type, 0,
                     static_cast<uint8_t>(c), p1, p2, sisterNone(), 0, nullptr };
        uint32_t idx = emit(e, t, tick, type);
        // The three types Engine::playSkippedEvents replays on a seek. Only the
        // sliced path needs them resident (see PcRaw); encodeIdx is exactly that
        // path, so it doubles as the switch.
        if (encodeIdx && (type == kProgramChange || type == kController ||
                          type == kPitchBend))
            pcRaw.push_back({ idx, status, p1, p2, 0 });
    }

    void tempo(uint32_t, uint32_t) {}   // tempo map already built in pass A

    void trackDone(int) {
        sampleTrackOff.push_back(static_cast<uint32_t>(sampleUs.size()));
        inTrackCount = 0;
    }
};

#if defined(__ANDROID__)
bool setThreadAffinityMaskSelf(uint64_t mask) {
    if (mask == 0) return false;
    int ncpu = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    cpu_set_t set;
    CPU_ZERO(&set);
    bool any = false;
    for (int c = 0; c < ncpu && c < 64; c++) {
        if (((mask >> c) & 1ULL) != 0ULL) { CPU_SET(c, &set); any = true; }
    }
    if (!any) return false;
    // See engine.cpp: direct syscall so the ELF carries no sched_setaffinity
    // UND symbol, which Gingerbread's loader cannot resolve.
    syscall(__NR_sched_setaffinity, 0, sizeof(set), &set);
    return true;
}
#endif

// Fault a byte range in: one volatile read per page. The mapping is
// read-only, so this is purely a page-cache populate.
void touchRange(const uint8_t* first, const uint8_t* last) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(
        reinterpret_cast<uintptr_t>(first) & ~(kPageSize - 1));
    for (; p <= last; p += kPageSize) {
        (void)*const_cast<volatile uint8_t*>(p);
    }
}

uint64_t monoUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + ts.tv_nsec / 1000;
}

// MemAvailable from /proc/meminfo, in bytes — the kernel's own estimate of
// what can be handed out without swapping, which is exactly the quantity the
// window has to fit inside. 0 if unreadable (the caller then skips budgeting
// rather than guessing). stdio-free so this is safe at 10 Hz on the loader.
// One named "Field:   N kB" line out of a /proc/meminfo buffer, in bytes.
uint64_t meminfoField(const char* buf, const char* name) {
    const char* p = strstr(buf, name);
    if (!p) return 0;
    p += strlen(name);
    while (*p == ' ' || *p == '\t') p++;
    return static_cast<uint64_t>(strtoull(p, nullptr, 10)) * 1024ull;
}

uint64_t readMemAvailable() {
    int fd = ::open("/proc/meminfo", O_RDONLY);
    if (fd < 0) return 0;
    char buf[2048];
    ssize_t got = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (got <= 0) return 0;
    buf[got] = '\0';
    // Value is in kB on every kernel that exports the field (2.6.27+ for the
    // format, 3.14+ for MemAvailable itself).
    if (uint64_t v = meminfoField(buf, "MemAvailable:")) return v;

    // Pre-3.14 kernels do not export it, and returning 0 here meant the caller
    // skipped budgeting ENTIRELY — the read-ahead window was never priced
    // against memory and never shrank. That is not a safe default on exactly
    // the devices that need it most: aPFA's floor is API 10, so a large slice
    // of the target hardware is pre-3.14. Measured on a Galaxy S5 Active
    // (Linux 3.4.0): "0 MB avail" on every tick of an RST 14.9 load, against
    // 5.58 M pages of kswapd reclaim.
    //
    // So estimate it the way the kernel's own commit does, minus the
    // watermarks we cannot see: free memory, plus HALF the reclaimable page
    // cache. Halving is the conservative half of the kernel's
    // `pagecache - min(pagecache/2, wmark_low)` — some of that cache is dirty
    // or mapped and will not come back on demand. SwapCached sits inside
    // Cached but is swap-backed, so it is not free for the taking.
    const uint64_t freeB   = meminfoField(buf, "MemFree:");
    const uint64_t buffers = meminfoField(buf, "Buffers:");
    const uint64_t cached  = meminfoField(buf, "Cached:");
    const uint64_t swapCac = meminfoField(buf, "SwapCached:");
    const uint64_t pagecache = cached > swapCac ? cached - swapCac : 0;
    return freeB + buffers / 2 + pagecache / 2;
}

}  // namespace

// ---- predict -------------------------------------------------------------------

uint64_t Streamer::mergeResidentBytes(uint64_t events) {
    return events * sizeof(uint32_t)                       // sisterPos_
         + static_cast<uint64_t>(kInvWindowMin) * sizeof(uint32_t)
         + kInvWindowReserve;                              // smallest inv window
}

uint64_t Streamer::predictEventCount(const std::string& midiPath) {
    int fd = ::open(midiPath.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 14) { ::close(fd); return 0; }
    size_t fileSize = static_cast<size_t>(st.st_size);
    void* map = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED) return 0;
    const uint8_t* mbase   = static_cast<const uint8_t*>(map);
    const uint8_t* fileEnd = mbase + fileSize;

    Reader hdr{ mbase, fileEnd };
    if (hdr.u8() != 'M' || hdr.u8() != 'T' || hdr.u8() != 'h' || hdr.u8() != 'd') {
        munmap(map, fileSize); return 0;
    }
    uint32_t hdrLen = hdr.u32();
    hdr.u16();
    uint32_t numTracks = hdr.u16();
    hdr.u16();
    if (hdrLen > 6) hdr.skip(hdrLen - 6);

    SkimSink skim;
    std::atomic<float> dummy{0};
    int maxTrack = 0;
    walkTracks(hdr.p, fileEnd, numTracks, skim, dummy, 0.0f, 0.0f, maxTrack);
    munmap(map, fileSize);

    uint64_t total = 0;
    for (size_t c : skim.trackCounts) total += c;
    return total;
}

// ---- open --------------------------------------------------------------------

// ---- slice planning (see SLICED-POOL-DESIGN.md) -----------------------------
//
// Walk the time-sorted pairing table once. `sounding` is the number of notes on
// at the current position: cut a slice there and that is exactly its carry-out
// AND the next slice's carry-in, because they are the same set of notes seen
// from either side of the boundary.
//
// The cost of the slice being built is therefore
//     carryIn + (pos - firstPos) + sounding
// and the cut happens as soon as taking one more event would exceed the budget.
std::vector<SlicePlan> planSlices(const std::vector<uint32_t>& sisterPos,
                                  uint32_t budgetEvents,
                                  uint32_t minEvents) {
    std::vector<SlicePlan> out;
    const size_t n = sisterPos.size();
    if (n == 0 || budgetEvents == 0) return out;
    if (minEvents == 0) minEvents = 1;

    SlicePlan cur;
    cur.firstPos = 0;
    cur.carryIn  = 0;
    uint32_t sounding = 0;

    for (size_t pos = 0; pos < n; pos++) {
        // Would admitting this event push the slice past the budget? The event
        // itself is +1 body, and it changes `sounding`, which is the carry-out
        // we would have to copy in as well.
        uint32_t s = sisterPos[pos];
        uint32_t soundingAfter = sounding;
        if (s == Streamer::kSisNoteOff) {
            if (soundingAfter > 0) soundingAfter--;
        } else if (s != Streamer::kSisNonNote) {
            soundingAfter++;
        }
        uint64_t costIfTaken = static_cast<uint64_t>(cur.carryIn)
                             + (pos + 1 - cur.firstPos)
                             + soundingAfter;
        uint32_t taken = static_cast<uint32_t>(pos - cur.firstPos);

        if (costIfTaken > budgetEvents && taken >= minEvents) {
            // Cut BEFORE this event: the slice ends at `pos`, and the notes
            // sounding across that boundary are its carry-out.
            cur.endPos   = static_cast<uint32_t>(pos);
            cur.carryOut = sounding;
            out.push_back(cur);
            cur = SlicePlan{};
            cur.firstPos = static_cast<uint32_t>(pos);
            cur.carryIn  = sounding;
        }
        sounding = soundingAfter;
    }
    cur.endPos   = static_cast<uint32_t>(n);
    cur.carryOut = 0;              // nothing sounds past the end of the song
    out.push_back(cur);
    return out;
}

// Reserve `total` bytes of address space for the pool. One piece if the
// address space allows it — which on a 64-bit build it always does, and that is
// the only shape a 64-bit build will accept. A 32-bit process that has the room
// but not in one run gets the pool split across several pieces instead of being
// refused; see PoolSeg in streamer.h for why that is safe.
bool Streamer::reserveSegments(size_t total) {
    for (const PoolSeg& sg : segs_) munmap(sg.va, sg.len);
    segs_.clear();
    base_ = nullptr;
    if (!reserveInto(segs_, total)) return false;
    base_ = segs_[0].va;
    LOGI("streamer: pool address space reserved in %zu piece%s (%zu MB total)",
         segs_.size(), segs_.size() == 1 ? "" : "s", total >> 20);
    for (size_t i = 0; i < segs_.size() && segs_.size() > 1; i++)
        LOGI("   seg %zu: %zu MB at %p (pool +%zu MB)",
             i, segs_[i].len >> 20, static_cast<void*>(segs_[i].va),
             segs_[i].off >> 20);
    return true;
}

// Grab `total` bytes of address space into `out`, as one piece if possible and
// as several if a 32-bit process has the room but not in one run.
bool Streamer::reserveInto(std::vector<PoolSeg>& out, size_t total) {
    out.clear();
    if (total == 0) return false;

    auto reserve = [](size_t len) -> uint8_t* {
        void* p = mmap(nullptr, len, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
    };

    if (uint8_t* one = reserve(total)) {
        out.push_back({ one, 0, total });
        return true;
    }
    // 64-bit: if a single reservation failed, something is wrong that splitting
    // will not fix — 256 TB of address space does not run out by fragmentation.
    if (sizeof(void*) > 4) return false;

    size_t off = 0;
    while (off < total && out.size() < kMaxSegs) {
        const size_t remaining = total - off;
        uint8_t* got = nullptr;
        size_t   len = 0;
        // Ask for everything left, then keep halving. Every piece but the last
        // is rounded down to a grain boundary; the last is whatever remains
        // (already page-aligned, since total is and every earlier piece was).
        for (size_t want = remaining; want >= kSegGrain; want /= 2) {
            size_t ask = (want == remaining) ? remaining
                                             : (want / kSegGrain) * kSegGrain;
            if (ask == 0) break;
            if (ask < kSegMin && ask < remaining) break;   // sliver, and not the tail
            got = reserve(ask);
            if (got) { len = ask; break; }
        }
        if (!got) break;
        out.push_back({ got, off, len });
        off += len;
    }

    if (off < total) {
        for (const PoolSeg& sg : out) munmap(sg.va, sg.len);
        out.clear();
        return false;
    }
    return true;
}

bool Streamer::open(const std::string& midiPath, MidiData& out,
                    std::atomic<float>& progress, bool chunked,
                    const std::string& poolDir) {
    progress.store(0.0f);
    close();
    diskFull_ = fileTooBig_ = noAddrSpace_ = needsChunked_ = false;

    // ---- map the MIDI file, parse the header (verbatim parseMidi) ----
    int fd = ::open(midiPath.c_str(), O_RDONLY);
    if (fd < 0) { LOGE("streamer: cannot open %s", midiPath.c_str()); return false; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 14) {
        ::close(fd); LOGE("streamer: file too small"); return false;
    }
    size_t fileSize = static_cast<size_t>(st.st_size);
    void* midiMap = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (midiMap == MAP_FAILED) { LOGE("streamer: mmap failed"); return false; }
    const uint8_t* mbase   = static_cast<const uint8_t*>(midiMap);
    const uint8_t* fileEnd = mbase + fileSize;

    Reader hdr{ mbase, fileEnd };
    if (hdr.u8() != 'M' || hdr.u8() != 'T' || hdr.u8() != 'h' || hdr.u8() != 'd') {
        munmap(midiMap, fileSize); LOGE("streamer: not a MIDI file"); return false;
    }
    uint32_t hdrLen    = hdr.u32();
    hdr.u16();                                   // format (unused)
    uint32_t numTracks = hdr.u16();
    int16_t  division  = static_cast<int16_t>(hdr.u16());
    if (hdrLen > 6) hdr.skip(hdrLen - 6);
    int ticksPerQuarter = division > 0 ? division : 480;  // SMPTE -> fallback
    progress.store(0.02f);

    // ---- pass A: skim ----
    SkimSink skim;
    int maxTrack = 0;
    walkTracks(hdr.p, fileEnd, numTracks, skim, progress, 0.02f, 0.25f, maxTrack);

    size_t totalEvents = 0;
    for (size_t c : skim.trackCounts) totalEvents += c;
    if (totalEvents == 0 || totalEvents >= kSisNoteOff ||
        totalEvents > (SIZE_MAX / kEventSize) - kPageSize) {
        munmap(midiMap, fileSize);
        LOGE("streamer: unusable event count %zu", totalEvents);
        return false;
    }
    totalEvents_ = totalEvents;
    poolBytes_   = totalEvents * kEventSize;
    mapLen_      = (poolBytes_ + kPageSize - 1) & ~(kPageSize - 1);

    // ---- tempo map (verbatim parseMidi) ----
    std::sort(skim.tempos.begin(), skim.tempos.end(),
              [](const TempoEvent& a, const TempoEvent& b) { return a.tick < b.tick; });
    std::vector<TempoSeg> segs;
    segs.push_back({ 0u, 0ull, 500000u });
    for (const TempoEvent& te : skim.tempos) {
        TempoSeg& last = segs.back();
        if (te.tick <= last.tick) { last.usPerQuarter = te.usPerQuarter; continue; }
        uint64_t us = last.usAtTick +
            uint64_t(te.tick - last.tick) * last.usPerQuarter / ticksPerQuarter;
        segs.push_back({ te.tick, us, te.usPerQuarter });
    }

    // ---- decide how the pool will be reached, and reserve for it ----
    //
    // Two shapes, and which one it is has to be settled BEFORE pass B, because
    // pass B writes `sister` into the file and the two want different things
    // there (a pool address / a pool index):
    //
    //   1. THE WHOLE POOL, reserved up front in one piece or — on a 32-bit
    //      process with the room but not in one run — several, then mapped over
    //      the reservation MAP_FIXED. The shipped path, and the only one a
    //      64-bit build will accept.
    //   2. SLICED (32-bit only): the pool file is written and NEVER mapped. One
    //      slice at a time lives in a fixed pair of arenas, so the address space
    //      stops scaling with the song and free storage becomes the ceiling
    //      instead. See SLICED-POOL-DESIGN.md.
    //
    // Reserving the pool is not the end of the story either: the rest of the
    // load still has to allocate, and the pool can take so much of a 32-bit
    // address space that nothing is left to work in. That failed as an uncaught
    // std::bad_alloc two milliseconds after a *successful* 1564 MB reservation
    // on an LG X410 — the reservation had eaten 1564 MB of the 1751 MB free,
    // leaving only fragments. So price the remainder before committing, and
    // give it straight back.
    //
    // The working set is NOT one allocation. It is several std::vectors, and
    // every one of them needs a contiguous run of its OWN:
    //
    //   events[]    totalEvents * sizeof(PlayEvent*)  held all session
    //   sisterPos_  totalEvents * 4                   held all session
    //   inv[]       totalEvents * 4                   through pass D
    //   poolIdx_    totalEvents * 4    sliced only    held all session
    //   posUs_      totalEvents * 4    sliced only    held all session
    //   the sort    emit.runBuf, 16 B/event in ONE block when un-chunked;
    //               kMergeBudget spread over per-run buffers when chunked
    //
    // Pricing that as a single splittable reservation is what let a load pass
    // this check and then die anyway. On an LG Stylo 3 the pool reservation
    // left 424 MB free in gaps of 222/128/111/22/16/15/6/3 MB; a 399 MB
    // aggregate probe was satisfied across those pieces without complaint,
    // while the three 111.8 MB tables the load actually wanted could place only
    // two — the third found 111 MB and threw std::bad_alloc at 80%. The perverse
    // part: that phone had MORE free address space than an X410 that loaded the
    // same file, because on the X410 the aggregate probe failed and the load
    // fell through to slicing, which is what it should have done on both.
    //
    // So probe the real shape — every block on its own, all held at once, then
    // released. Only the chunked merge budget may arrive in pieces, because
    // that one genuinely is a set of per-run buffers.
    fullPoolBytes_ = poolBytes_;
    struct WorkBlock { size_t bytes; bool contiguous; };
    auto workBlocks = [&](bool slicedShape, bool chunkedShape) {
        const size_t tbl = totalEvents * sizeof(uint32_t);
        std::vector<WorkBlock> b;
        b.push_back({ totalEvents * sizeof(PlayEvent*), true });  // events[]
        b.push_back({ tbl, true });                               // sisterPos_
        b.push_back({ tbl, true });                               // inv[]
        if (slicedShape) {
            b.push_back({ tbl, true });                           // poolIdx_
            b.push_back({ tbl, true });                           // posUs_
        }
        if (chunkedShape) b.push_back({ kMergeBudget, false });   // per-run bufs
        else              b.push_back({ totalEvents * sizeof(SortKey), true });
        return b;
    };
    auto workBytes = [&](bool slicedShape, bool chunkedShape) {
        size_t n = 0;
        for (const WorkBlock& w : workBlocks(slicedShape, chunkedShape))
            n += w.bytes;
        return n;
    };
    auto priceWork = [&](bool slicedShape, bool chunkedShape) -> bool {
        const std::vector<WorkBlock> want = workBlocks(slicedShape, chunkedShape);
        std::vector<PoolSeg> held;
        size_t pieces = 0;
        bool ok = true;
        for (const WorkBlock& w : want) {
            if (w.bytes == 0) continue;
            std::vector<PoolSeg> got;
            // reserveInto takes one piece when it can and splits when it cannot,
            // so a block that comes back split is a block with no contiguous
            // home — exactly the allocation that would have thrown.
            if (!reserveInto(got, w.bytes) || (w.contiguous && got.size() != 1)) {
                for (const PoolSeg& sg : got) munmap(sg.va, sg.len);
                LOGI("streamer: working set: no %s home for a %zu MB block",
                     w.contiguous ? "contiguous" : "", w.bytes >> 20);
                ok = false;
                break;
            }
            pieces += got.size();
            held.insert(held.end(), got.begin(), got.end());
        }
        for (const PoolSeg& sg : held) munmap(sg.va, sg.len);
        if (ok)
            LOGI("streamer: working set of %zu MB priced as %zu blocks "
                 "in %zu piece%s", workBytes(slicedShape, chunkedShape) >> 20,
                 want.size(), pieces, pieces == 1 ? "" : "s");
        return ok;
    };
    auto dropPoolReservation = [&]() {
        for (const PoolSeg& sg : segs_) munmap(sg.va, sg.len);
        segs_.clear();
        base_ = nullptr;
    };

    // APFA_FORCE_SLICED takes the sliced path even when the whole pool would
    // map, so the 32-bit-only code can be exercised on a -m32 host build (see
    // tests/slicepool_test.cpp). It is deliberately inert in a 64-bit process:
    // a 64-bit build never slices, no exceptions.
    const bool forceSliced = sizeof(void*) == 4 && getenv("APFA_FORCE_SLICED") &&
                             *getenv("APFA_FORCE_SLICED") == '1';
    bool sliced = false;
    if (forceSliced) {
        LOGI("streamer: APFA_FORCE_SLICED — slicing a pool that would have mapped");
        sliced = true;
    } else if (reserveSegments(mapLen_)) {
        if (!priceWork(false, chunked)) {
            LOGI("streamer: the pool fits (%zu MB) but its %zu MB working set "
                 "does not — slicing instead", mapLen_ >> 20,
                 workBytes(false, chunked) >> 20);
            dropPoolReservation();
            sliced = true;
        }
    } else {
        LOGI("streamer: %zu MB of pool will not map in a %zu-bit address space "
             "— slicing instead", mapLen_ >> 20, sizeof(void*) * 8);
        sliced = true;
    }

    if (sliced && sizeof(void*) > 4) {
        // A 64-bit build never slices. 256 TB of address space does not run out
        // by fragmentation, so a failure here is something slicing cannot fix —
        // except by spilling the sort table, which is a mode the caller can
        // switch to, so say so before refusing.
        if (!chunked && priceWork(false, true)) {
            needsChunked_ = true;
            LOGI("streamer: the chunked sort's working set would fit where "
                 "the un-chunked one did not");
        }
        munmap(midiMap, fileSize);
        close();
        noAddrSpace_ = true;
        LOGE("streamer: cannot reserve %zu MB of address space (64-bit process)",
             mapLen_ >> 20);
        return false;
    }

    if (sliced) {
        // Two arenas, so the next slice is built AND mapped while the current
        // one is still playing and a transition costs a table swap instead of
        // an mmap and a fault storm. Take the largest pair the address space
        // will hand over in one contiguous piece each — sister pointers are
        // baked against an arena base, so a split arena is no arena — halving
        // down to the floor, below which an arena could not cover the loader's
        // read-ahead horizon and the carry sets on top of it.
        // APFA_ARENA_BYTES pins the arena so a test can force many slices out
        // of a small MIDI; ignored outside a 32-bit process, like slicing itself.
        //
        // The arenas are reserved BEFORE the working set is priced, and their
        // size is the one part of the sliced shape that is ours to choose. A
        // pair of 192 MB arenas that leaves the five 111 MB tables without a
        // contiguous home is not "no address space" — it is the wrong split of
        // it. So price after every reservation, and when the pricing fails,
        // give the arenas back and buy the tables their room instead. Largest
        // first, because a smaller arena means more slices and more boundary
        // transitions: it is traded away only when it has to be.
        const bool arenaPinned = getenv("APFA_ARENA_BYTES") != nullptr;
        size_t want = static_cast<size_t>(
            envUs("APFA_ARENA_BYTES", static_cast<int64_t>(
                std::min<size_t>(kArenaTargetBytes, mapLen_))));
        want = (want + kPageSize - 1) & ~(kPageSize - 1);
        const size_t firstWant = want;
        size_t lastWant = want;
        bool priced = false;
        while (want >= kPageSize) {
            lastWant = want;
            std::vector<PoolSeg> a, b;
            if (reserveInto(a, want) && a.size() == 1 &&
                reserveInto(b, want) && b.size() == 1) {
                arena_[0] = a[0].va;
                arena_[1] = b[0].va;
                arenaBytes_ = want;
                // Below the floor an arena could not cover the loader's
                // read-ahead horizon and the carry on top of it, so a pair that
                // small is no better than none — unless it holds the whole pool
                // anyway, or a test pinned the size.
                const bool bigEnough = arenaPinned || want >= kArenaMinBytes ||
                                       want >= mapLen_;
                if (bigEnough && priceWork(true, chunked)) { priced = true; break; }
                munmap(arena_[0], arenaBytes_);
                munmap(arena_[1], arenaBytes_);
                arena_[0] = arena_[1] = nullptr;
                arenaBytes_ = 0;
                if (!bigEnough) break;      // already at the floor
                LOGI("streamer: 2 x %zu MB arenas leave the %zu MB working set "
                     "nowhere to go — halving the arena and pricing again",
                     want >> 20, workBytes(true, chunked) >> 20);
            } else {
                for (const PoolSeg& sg : a) munmap(sg.va, sg.len);
                for (const PoolSeg& sg : b) munmap(sg.va, sg.len);
            }
            if (want <= kArenaMinBytes || arenaPinned) break;
            const size_t half = ((want / 2) + kPageSize - 1) & ~(kPageSize - 1);
            want = half < kArenaMinBytes ? kArenaMinBytes : half;
        }
        if (!priced) {
            // Same last word as the un-sliced path: the chunked on-disk sort
            // spills the sort table rather than holding it, so if THAT would
            // have fit the caller only has to switch modes.
            if (!chunked && priceWork(true, true)) {
                needsChunked_ = true;
                LOGI("streamer: the chunked sort's %zu MB working set would "
                     "fit where the un-chunked %zu MB did not",
                     workBytes(true, true) >> 20, workBytes(true, chunked) >> 20);
            }
            // Report before close(), which zeroes arenaBytes_ — reading it
            // afterwards is how this line used to claim "0 MB arenas" for a
            // load whose arenas had reserved perfectly well.
            LOGE("streamer: %zu-bit address space exhausted even sliced "
                 "(arenas tried from %zu MB down to %zu MB, %zu MB working set)",
                 sizeof(void*) * 8, firstWant >> 20, lastWant >> 20,
                 workBytes(true, chunked) >> 20);
            munmap(midiMap, fileSize);
            close();
            noAddrSpace_ = true;
            return false;
        }
        sliced_ = true;
        base_   = arena_[0];
        LOGI("streamer: sliced pool — 2 x %zu MB arena (%zu events each) against "
             "a %zu MB pool that is never mapped",
             arenaBytes_ >> 20, arenaEvents(), mapLen_ >> 20);
    }

    // ---- pick where the pagefile lives ----
    // Default: the MIDI's directory = the app cache dir, temps unlinked
    // immediately (zero litter even if we crash mid-parse). "Pagefile on SD
    // Card" points us at the card's app cache dir instead; that path keeps its
    // temps named (see streamer.h) and only takes effect if the volume passes
    // the mapping probe — a card that can't back the mapping falls back to
    // internal storage rather than failing the load.
    std::string dir = midiPath.substr(0, midiPath.find_last_of('/'));
    if (dir.empty()) dir = ".";
    bool keepNamed = false;
    if (!poolDir.empty() && poolDir != dir) {
        sweepStaleTemps(poolDir);          // whatever a crashed load left there
        if (poolDirUsable(poolDir)) {
            dir = poolDir;
            keepNamed = true;
            LOGI("streamer: pagefile on SD card (%s)", dir.c_str());
        } else {
            LOGE("streamer: SD card pagefile dir unusable — using internal storage");
        }
    }
    // Named temps are tracked for close() to unlink; unlinked ones need no
    // bookkeeping at all. `outPath` lets a caller reclaim one early — see
    // releaseTemp.
    auto makePoolTemp = [&](const char* tag, std::string* outPath = nullptr) -> int {
        if (!keepNamed) return makeTemp(dir, tag, nullptr);
        std::string path;
        int fd = makeTemp(dir, tag, &path);
        if (fd >= 0) {
            tempPaths_.push_back(path);
            if (outPath) *outPath = path;
        }
        return fd;
    };
    // Give a named temp's space back the moment its fd closes, instead of at
    // close(). The unlinked path gets this from the kernel for free; without it
    // the spills — 16 B/event of keys plus 8 B/note of pairs, GBs on the MIDIs
    // that need the card in the first place — would sit on the card for the
    // whole of playback with nothing holding them open.
    poolDirUsed_   = dir;          // the slice builder creates its temps here
    poolKeepNamed_ = keepNamed;
    auto releaseTemp = [&](std::string& path) {
        if (path.empty()) return;
        unlink(path.c_str());
        tempPaths_.erase(std::remove(tempPaths_.begin(), tempPaths_.end(), path),
                         tempPaths_.end());
        path.clear();
    };

    // One file per poolFileBytes() of pool, so no offset ever leaves 31 bits.
    std::vector<int> poolFds;
    const size_t nPoolFiles = poolFileCount(poolBytes_);
    for (size_t i = 0; i < nPoolFiles; i++) {
        int fd = makePoolTemp("pool");
        if (fd < 0) {
            LOGE("streamer: temp pool file %zu/%zu failed in %s (errno=%d)",
                 i + 1, nPoolFiles, dir.c_str(), errno);
            for (int done : poolFds) ::close(done);
            munmap(midiMap, fileSize); close();
            return false;
        }
        poolFds.push_back(fd);
    }
    if (nPoolFiles > 1)
        LOGI("streamer: pool split across %zu files of at most %zu MB "
             "(32-bit off_t caps a single file at 2 GB)",
             nPoolFiles, poolFileBytes() >> 20);

    // Pre-flight free-space check: pass A gave exact event/note counts, so the
    // total on-disk footprint (pool + spills when chunked) is known before a
    // single byte is written. Refuse now rather than strand the phone at 0 B
    // free three passes in.
    uint64_t predictedDiskBytes = poolBytes_;
    if (chunked)
        predictedDiskBytes += totalEvents * sizeof(SortKey) +
                              static_cast<uint64_t>(skim.noteCount) * sizeof(Pair);
    // A sliced load keeps two materialised slices alongside the pool, each at
    // most one arena's worth. That is the whole extra storage cost of slicing,
    // however long the song is.
    if (sliced) predictedDiskBytes += 2ull * arenaBytes_;
    if (wouldExhaustDisk(poolFds[0], predictedDiskBytes)) {
        LOGE("streamer: %.1f MB pagefile would exhaust free storage — refusing",
             predictedDiskBytes / 1048576.0);
        diskFull_ = true;
        munmap(midiMap, fileSize);
        for (int f : poolFds) ::close(f);
        close();
        return false;
    }
    // Same idea for the per-file ceiling: a FAT32 card takes 4 GB and no more.
    // What has to fit is one LINK of the chain, not the whole pool — which is a
    // second thing the split buys, since poolFileBytes() is comfortably under
    // 4 GB, so a pool of any length now clears FAT32 as well.
    if (exceedsFileSizeLimit(poolFds[0], std::min<uint64_t>(poolBytes_,
                                                           poolFileBytes()))) {
        LOGE("streamer: %.1f MB pool file exceeds the volume's 4 GB file limit "
             "— refusing",
             std::min<uint64_t>(poolBytes_, poolFileBytes()) / 1048576.0);
        fileTooBig_ = true;
        munmap(midiMap, fileSize);
        for (int f : poolFds) ::close(f);
        close();
        return false;
    }

    // ---- pass B: emit ----
    std::string runsPath, pairsPath;   // named only on the SD path (see above)
    EmitSink emit;
    emit.fds  = &poolFds;
    emit.poolSegs = &segs_;
    emit.segs = &segs;
    emit.ticksPerQuarter = ticksPerQuarter;
    emit.trackSampleStep = kTrackSampleStep;
    emit.chunked = chunked;
    emit.encodeIdx = sliced;
    emit.buf.reserve(kBufEvents);
    emit.sampleTrackOff.push_back(0);
    if (chunked) {
        emit.runBuf.reserve(kRunEntries);
        emit.pairBuf.reserve(kPairBufEntries);
        emit.runsFd  = makePoolTemp("runs",  &runsPath);
        emit.pairsFd = makePoolTemp("pairs", &pairsPath);
        if (emit.runsFd < 0 || emit.pairsFd < 0) {
            LOGE("streamer: spill temp files failed in %s (errno=%d)", dir.c_str(), errno);
            if (emit.runsFd >= 0) ::close(emit.runsFd);
            if (emit.pairsFd >= 0) ::close(emit.pairsFd);
            munmap(midiMap, fileSize);
            for (int f : poolFds) ::close(f);
            close();
            return false;
        }
    } else {
        // Un-chunked: hold everything for pass D in RAM up front. This IS the
        // deliberate ceiling (16 B/event + 8 B/note transient); past it the
        // engine routes to chunked mode or refuses.
        emit.runBuf.reserve(totalEvents);
        emit.pairBuf.reserve(skim.noteCount);
    }

    int maxTrackB = 0;
    walkTracks(hdr.p, fileEnd, numTracks, emit, progress, 0.25f, 0.60f, maxTrackB);
    emit.flush();
    if (chunked) {
        emit.spillRun();
        emit.spillPairs();
    } else {
        // One whole-table sort in place of the run spills — same comparator,
        // so the merged and un-merged orders are byte-identical.
        std::sort(emit.runBuf.begin(), emit.runBuf.end(), sortKeyLess);
        emit.runEntries = emit.runBuf.size();
        emit.pairCount  = emit.pairBuf.size();
    }
    munmap(midiMap, fileSize);

    // From here on, every failure path must release the spill fds too
    // (chunked mode only creates them; close(-1) is guarded).
    auto failCleanup = [&]() {
        if (emit.runsFd >= 0)  ::close(emit.runsFd);
        if (emit.pairsFd >= 0) ::close(emit.pairsFd);
        for (int f : poolFds) ::close(f);
        close();
    };

    if (emit.ioError || emit.nextIdx != totalEvents || maxTrackB != maxTrack ||
        emit.runEntries != totalEvents) {
        diskFull_   = emit.diskFull;
        fileTooBig_ = emit.fileTooBig;
        LOGE("streamer: emit pass failed (io=%d, full=%d, big=%d, %u/%zu events, "
             "%llu keys)",
             emit.ioError ? 1 : 0, emit.diskFull ? 1 : 0, emit.fileTooBig ? 1 : 0,
             emit.nextIdx, totalEvents,
             static_cast<unsigned long long>(emit.runEntries));
        failCleanup();
        return false;
    }

    // ---- pass C: patch cross-buffer sister pointers ----
    progress.store(0.62f);
    {
        std::sort(emit.fixups.begin(), emit.fixups.end(),
                  [](const Fixup& a, const Fixup& b) { return a.onIdx < b.onIdx; });
        std::vector<PlayEvent> chunk(kBufEvents);
        size_t i = 0;
        while (i < emit.fixups.size()) {
            uint32_t c0 = emit.fixups[i].onIdx - (emit.fixups[i].onIdx % kBufEvents);
            size_t   n  = std::min(kBufEvents, totalEvents - c0);
            uint64_t off = static_cast<uint64_t>(c0) * kEventSize;
            if (!poolPread(poolFds, chunk.data(), n * kEventSize, off)) {
                LOGE("streamer: fixup pread failed at +%llu MB (errno=%d)",
                     static_cast<unsigned long long>(off >> 20), errno);
                failCleanup(); return false;
            }
            while (i < emit.fixups.size() && emit.fixups[i].onIdx < c0 + n) {
                const Fixup& f = emit.fixups[i];
                chunk[f.onIdx - c0].sister = emit.sisterAt(f.offIdx);
                i++;
            }
            if (!poolPwrite(poolFds, chunk.data(), n * kEventSize, off)) {
                LOGE("streamer: fixup pwrite failed at +%llu MB (errno=%d)",
                     static_cast<unsigned long long>(off >> 20), errno);
                failCleanup(); return false;
            }
        }
        emit.fixups.clear(); emit.fixups.shrink_to_fit();
    }

    // ---- pass D: sorted keys -> events[], pcIdx, samples ----
    // Same total order as midi_parser.cpp's stable_sort: (µs, track,
    // channelEventType DESC), ties broken by pool index = parse order.
    // us<<19 | track<<3 | (14-chType) packs all three keys; idx is the tie.
    // Un-chunked: one linear walk over the whole sorted key table (resident
    // since pass B). Chunked: k-way merge of the spilled runs — peak RAM is
    // the permanent tables plus ~64 MB of merge buffers, which is what lets
    // 90M-note files through where the resident table gets lmkd reaped
    // ("low on swap and thrashing").
    progress.store(0.62f);
    out.events.resize(totalEvents);
    out.programChangeIdx.clear();
    posTimes_.clear();
    if (sliced) {
        // events[] stays full length and keeps global positions — the engine's
        // cursors, active_ and noteState_ never learn that slicing exists — but
        // its pointers are filled in a slice at a time, by installSliceLocked.
        poolIdx_.assign(totalEvents, 0);
        posUs_.assign(totalEvents, 0);
    }
    auto placeEvent = [&](size_t pos, const SortKey& sk) {
        if (sliced) {
            poolIdx_[pos] = sk.idx;
            posUs_[pos]   = static_cast<uint32_t>(sk.key >> 19);
        } else {
            out.events[pos] = reinterpret_cast<PlayEvent*>(
                poolAddr(static_cast<size_t>(sk.idx) * kEventSize));
        }
        int chType = 14 - static_cast<int>(sk.key & 7);
        if (chType == kProgramChange || chType == kController || chType == kPitchBend)
            out.programChangeIdx.push_back(pos);
        if (pos % kPosSampleStep == 0)
            posTimes_.push_back(static_cast<int64_t>(sk.key >> 19));
        if ((pos & 0xFFFFF) == 0)
            progress.store(0.62f + 0.18f * float(pos) / float(totalEvents));
    };
    if (!chunked) {
        for (size_t pos = 0; pos < totalEvents; pos++)
            placeEvent(pos, emit.runBuf[pos]);
        emit.runBuf.clear();
        emit.runBuf.shrink_to_fit();   // release the 16 B/event key table now
    } else {
        struct RunCursor {
            uint64_t next, end;            // entry indices in the runs file
            std::vector<SortKey> buf;
            size_t bufPos = 0;
        };
        size_t nRuns = emit.runStarts.size();
        size_t perRunEntries = std::max<size_t>(
            4096, kMergeBudget / (nRuns ? nRuns : 1) / sizeof(SortKey));

        std::vector<RunCursor> runs(nRuns);
        for (size_t r = 0; r < nRuns; r++) {
            runs[r].next = emit.runStarts[r];
            runs[r].end  = (r + 1 < nRuns) ? emit.runStarts[r + 1] : emit.runEntries;
        }
        auto refill = [&](RunCursor& rc) -> bool {
            if (rc.next >= rc.end) return false;
            size_t n = static_cast<size_t>(
                std::min<uint64_t>(perRunEntries, rc.end - rc.next));
            rc.buf.resize(n);
            ssize_t got = pread(emit.runsFd, rc.buf.data(), n * sizeof(SortKey),
                                static_cast<int64_t>(rc.next) * sizeof(SortKey));
            if (got != static_cast<ssize_t>(n * sizeof(SortKey))) return false;
            rc.next += n;
            rc.bufPos = 0;
            return true;
        };

        using HeapItem = std::pair<SortKey, uint32_t>;   // (key, run index)
        auto heapGreater = [](const HeapItem& a, const HeapItem& b) {
            if (a.first.key != b.first.key) return a.first.key > b.first.key;
            return a.first.idx > b.first.idx;
        };
        std::priority_queue<HeapItem, std::vector<HeapItem>, decltype(heapGreater)>
            heap(heapGreater);
        for (size_t r = 0; r < nRuns; r++)
            if (refill(runs[r]))
                heap.push({ runs[r].buf[runs[r].bufPos++], static_cast<uint32_t>(r) });

        size_t pos = 0;
        while (!heap.empty()) {
            HeapItem top = heap.top();
            heap.pop();
            placeEvent(pos, top.first);
            pos++;

            RunCursor& rc = runs[top.second];
            if (rc.bufPos >= rc.buf.size() && !refill(rc)) continue;
            heap.push({ rc.buf[rc.bufPos++], top.second });
        }
        if (pos != totalEvents) {
            LOGE("streamer: merge produced %zu/%zu events", pos, totalEvents);
            failCleanup();
            return false;
        }
    }
    if (emit.runsFd >= 0) {
        ::close(emit.runsFd); emit.runsFd = -1;
        releaseTemp(runsPath);            // keys are consumed — give the space back
    }
    progress.store(0.80f);

    // ---- sisterPos: inverse map (transient) + pairs (resident or streamed) ----
    // inv[] maps pool index -> events[] position. Holding all of it costs
    // 4 B/event on top of sisterPos_'s own 4, and those two together are the
    // part of the load that chunking does NOT move to disk: the merge writes
    // events[] straight through by position so swap takes it, but these two are
    // hit at random here and have to be resident. That is what lmkd kills a big
    // chunked load for at exactly this point — Redmi Note 9 / U11, 153.1M
    // events, 1168 MB wanted against a 1077 MB budget, dead at 80%.
    //
    // So when the whole map will not fit, it is built in WINDOWS of pool-index
    // space. Each pass holds invWin entries, resolves whichever pair fields
    // land inside the window, and rewrites the pairs in place with
    // kInvResolvedBit set on a field once it holds a position rather than an
    // index, so a later window leaves it alone. The price is one rescan of the
    // forward map and one rewrite of the pairs per pass.
    //
    // A single window IS the historical path, taken verbatim — every device
    // that loads a MIDI today still loads it with the same code and the same
    // I/O. The windowing only ever engages where the load would not happen.
    {
        sisterPos_.assign(totalEvents, kSisNonNote);

        // Sized against what is free now that sisterPos_ is standing, not
        // against a prediction: pass D is the one place where guessing high
        // costs the entire load.
        size_t invWin = totalEvents;
        if (totalEvents < kInvResolvedBit) {
            // APFA_INV_WINDOW pins the window in entries so a host test can
            // force many passes out of a small file. Inert on a phone, like
            // APFA_ARENA_BYTES — see tests/README.md.
            if (int64_t pinned = envUs("APFA_INV_WINDOW", 0)) {
                invWin = static_cast<size_t>(pinned);
            } else if (uint64_t avail = readMemAvailable()) {
                uint64_t room = avail > kInvWindowReserve
                    ? static_cast<uint64_t>((avail - kInvWindowReserve) *
                                            kInvWindowFraction) / sizeof(uint32_t)
                    : 0;
                if (room < totalEvents)
                    invWin = static_cast<size_t>(
                        std::max<uint64_t>(room, kInvWindowMin));
            }
        }
        if (invWin > totalEvents || invWin == 0) invWin = totalEvents;
        const size_t passes = (totalEvents + invWin - 1) / invWin;

        // Streams the pairs file through fn in kPairBufEntries chunks, writing
        // each chunk back only when fn changed it. Chunked only — the
        // un-chunked path still has every pair in RAM.
        std::vector<Pair> chunk;
        auto streamPairs = [&](bool writeBack, auto&& fn) -> bool {
            if (chunk.empty()) chunk.resize(kPairBufEntries);
            uint64_t done = 0;
            while (done < emit.pairCount) {
                size_t n = static_cast<size_t>(
                    std::min<uint64_t>(chunk.size(), emit.pairCount - done));
                const size_t  bytes = n * sizeof(Pair);
                const int64_t off   = static_cast<int64_t>(done) * sizeof(Pair);
                if (pread(emit.pairsFd, chunk.data(), bytes, off)
                        != static_cast<ssize_t>(bytes)) {
                    LOGE("streamer: pairs pread failed");
                    return false;
                }
                for (size_t i = 0; i < n; i++) fn(chunk[i]);
                if (writeBack &&
                    pwrite(emit.pairsFd, chunk.data(), bytes, off)
                        != static_cast<ssize_t>(bytes)) {
                    LOGE("streamer: pairs pwrite failed");
                    return false;
                }
                done += n;
            }
            return true;
        };
        auto bail = [&]() {
            ::close(emit.pairsFd);
            for (int f : poolFds) ::close(f);
            close();
        };
        // pos of every pool index in [lo, hi). The forward map is a bijection,
        // so every entry of the window is written before it is read.
        auto fillWindow = [&](std::vector<uint32_t>& inv, size_t lo, size_t hi) {
            for (size_t pos = 0; pos < totalEvents; pos++) {
                size_t pi = sliced ? poolIdx_[pos]
                                   : poolOffOf(out.events[pos]) / kEventSize;
                if (pi >= lo && pi < hi) inv[pi - lo] = static_cast<uint32_t>(pos);
            }
        };

        std::vector<uint32_t> inv(invWin);
        if (passes == 1) {
            fillWindow(inv, 0, totalEvents);
            auto place = [&](const Pair& pr) {
                sisterPos_[inv[pr.onIdx]]  = inv[pr.offIdx];
                sisterPos_[inv[pr.offIdx]] = kSisNoteOff;
            };
            if (!chunked) {
                for (const Pair& pr : emit.pairBuf) place(pr);
                emit.pairBuf.clear();
                emit.pairBuf.shrink_to_fit();
            } else if (!streamPairs(false, place)) {
                bail();
                return false;
            }
        } else {
            LOGI("streamer: inv[] windowed into %zu passes of %.1f MB "
                 "(whole map is %.1f MB) — trading load time for the RAM it "
                 "would have taken",
                 passes, invWin * 4.0 / 1048576.0, totalEvents * 4.0 / 1048576.0);
            for (size_t lo = 0; lo < totalEvents; lo += invWin) {
                const size_t hi = std::min(lo + invWin, totalEvents);
                fillWindow(inv, lo, hi);
                auto resolve = [&](uint32_t& f) {
                    if (f & kInvResolvedBit) return;
                    if (f >= lo && f < hi) f = inv[f - lo] | kInvResolvedBit;
                };
                auto resolvePair = [&](Pair& pr) {
                    resolve(pr.onIdx);
                    resolve(pr.offIdx);
                };
                if (!chunked) {
                    for (Pair& pr : emit.pairBuf) resolvePair(pr);
                } else if (!streamPairs(true, resolvePair)) {
                    bail();
                    return false;
                }
                progress.store(0.80f + 0.10f * static_cast<float>(hi) /
                                              static_cast<float>(totalEvents));
            }
            // Every field is a position now; strip the flag and place them.
            auto place = [&](const Pair& pr) {
                uint32_t on  = pr.onIdx  & ~kInvResolvedBit;
                uint32_t off = pr.offIdx & ~kInvResolvedBit;
                sisterPos_[on]  = off;
                sisterPos_[off] = kSisNoteOff;
            };
            if (!chunked) {
                for (const Pair& pr : emit.pairBuf) place(pr);
                emit.pairBuf.clear();
                emit.pairBuf.shrink_to_fit();
            } else if (!streamPairs(false, place)) {
                bail();
                return false;
            }
        }
    }
    if (emit.pairsFd >= 0) {
        ::close(emit.pairsFd); emit.pairsFd = -1;
        releaseTemp(pairsPath);           // sisterPos is built — same
    }
    progress.store(0.92f);

    if (sliced) {
        // The three bytes playSkippedEvents replays for each controller /
        // program-change / pitch-bend event, put in events[] order. pcRaw is in
        // pool order, so one binary search per entry lines the two up.
        std::sort(emit.pcRaw.begin(), emit.pcRaw.end(),
                  [](const PcRaw& a, const PcRaw& b) { return a.idx < b.idx; });
        pcData_.assign(out.programChangeIdx.size(), 0);
        for (size_t i = 0; i < out.programChangeIdx.size(); i++) {
            uint32_t pi = poolIdx_[out.programChangeIdx[i]];
            const PcRaw* f = std::lower_bound(
                emit.pcRaw.data(), emit.pcRaw.data() + emit.pcRaw.size(), pi,
                [](const PcRaw& a, uint32_t v) { return a.idx < v; });
            if (f != emit.pcRaw.data() + emit.pcRaw.size() && f->idx == pi)
                pcData_[i] = static_cast<uint32_t>(f->code) |
                             (static_cast<uint32_t>(f->p1) << 8) |
                             (static_cast<uint32_t>(f->p2) << 16);
        }
        std::vector<PcRaw>().swap(emit.pcRaw);
        // Engine::load's pre-roll anchor: the first note-on, which it cannot
        // find by walking events[] when nothing is mapped yet.
        firstNoteUs_ = 0;
        for (size_t pos = 0; pos < totalEvents; pos++)
            if (sisterPos_[pos] < kSisNoteOff) {
                firstNoteUs_ = static_cast<int64_t>(posUs_[pos]);
                break;
            }
    } else {
        // per-track pool ranges + sampled (µs -> pool idx) index. A sliced load
        // navigates by the CURRENT SLICE's tables instead (buildSlice fills
        // them against slot indices), so these would only be overwritten.
        trackRange_.assign(skim.trackCounts.size(), TrackRange{});
        {
            size_t first = 0;
            for (size_t t = 0; t < skim.trackCounts.size(); t++) {
                trackRange_[t] = { first, skim.trackCounts[t] };
                first += skim.trackCounts[t];
            }
        }
        trackSamples_.resize(emit.sampleUs.size());
        for (size_t i = 0; i < emit.sampleUs.size(); i++)
            trackSamples_[i] = { emit.sampleUs[i], emit.sampleIdx[i] };
        trackSampleOff_ = std::move(emit.sampleTrackOff);
        // walkTracks may have stopped early (malformed file): pad offsets so every
        // trackRange_ index has a sample span (possibly empty).
        while (trackSampleOff_.size() < trackRange_.size() + 1)
            trackSampleOff_.push_back(static_cast<uint32_t>(trackSamples_.size()));
    }

    // ---- map the pool file read-only over the reservation ----
    // Sliced loads skip this entirely: the pool is never mapped, and the fd
    // stays open for the whole session because it is what the slice
    // materialiser reads from (and, on the internal path, the only thing
    // holding the unlinked file's blocks).
    if (!sliced) {
        bool mapOk = true;
        for (const PoolSeg& sg : segs_) {
            // poolFileBytes() is a multiple of kSegGrain, so every file boundary
            // is page-aligned and lands between two events — a segment that
            // crosses one just needs a second MAP_FIXED at the right VA.
            size_t done = 0;
            const size_t link = poolFileBytes();
            while (done < sg.len) {
                const uint64_t poff = static_cast<uint64_t>(sg.off) + done;
                const size_t   idx  = static_cast<size_t>(poff / link);
                const off_t    in   = static_cast<off_t>(poff % link);
                const size_t   n    = std::min(sg.len - done,
                                              link - static_cast<size_t>(in));
                void* mapped = mmap(sg.va + done, n, PROT_READ,
                                    MAP_FIXED | MAP_PRIVATE, poolFds[idx], in);
                if (mapped == MAP_FAILED || mapped != sg.va + done) {
                    LOGE("streamer: pool mmap failed at +%zu MB (errno=%d)",
                         (sg.off + done) >> 20, errno);
                    mapOk = false;
                    break;
                }
                done += n;
            }
            if (!mapOk) break;
        }
        for (int f : poolFds) ::close(f);
        if (!mapOk) {
            // A failed MAP_FIXED may have destroyed the reservation under it, so
            // the segment list can no longer be trusted to describe live mappings.
            segs_.clear();
            base_ = nullptr;
            close();
            return false;
        }
    } else {
        poolFds_ = poolFds;
    }
    events_    = &out.events;
    eventsMut_ = &out.events;

    // ---- MidiData metadata (identical to parseMidi's tail) ----
    out.totalUs = static_cast<uint32_t>(std::min<uint64_t>(emit.totalUs, 0xFFFFFFFFull));
    out.trackCount = maxTrack + 1;
    out.trackColors.assign(static_cast<size_t>(out.trackCount) * 16, 0xFFFFFFFFu);
    {
        uint32_t palette[16];
        defaultPalettePFA(palette);
        srand(static_cast<unsigned>(time(nullptr)));   // PianoFromAbove.cpp:43
        int iPos = 0;
        for (int trk = 0; trk < out.trackCount; trk++)
            for (int chn = 0; chn < 16; chn++) {
                size_t idx = static_cast<size_t>(trk) * 16 + chn;
                if (idx >= skim.hasNotes16.size() || !skim.hasNotes16[idx]) continue;
                out.trackColors[idx] = (iPos < 16) ? palette[iPos] : randColorPFA();
                iPos++;
            }
    }
    out.minNote = 0;     // parser's "All keys" override
    out.maxNote = 127;
    out.actualNoteCount = skim.noteCount;
    out.valid = true;
    // out.eventPool stays EMPTY — the pool lives in the mapping. events[]
    // points into it; MidiData stays move-only and never copies.

    // Front horizon: visible band + lead unless pinned by the env override
    // (which also switches off the adaptive shrink, so a pinned run measures
    // exactly the horizon it was given). setVisibleUs may have already run.
    {
        int64_t pinned = envUs("APFA_WIN_FRONT_US", 0);
        frontPinned_ = (pinned > 0);
        frontUs_ = frontPinned_ ? pinned : visibleUs_ + kLeadUs;
    }
    backUs_  = envUs("APFA_WIN_BACK_US",  kDefaultBackUs);

    if (sliced) {
        progress.store(0.95f);
        // Cut the walk into slices that fit an arena, keeping a quarter of it
        // back for the read-ahead extension past each boundary and the carry
        // that comes with it, then materialise the first one so playback can
        // start the moment load() returns.
        uint32_t budget = static_cast<uint32_t>(std::min<size_t>(
            arenaEvents() * kSliceBudgetNum / kSliceBudgetDen, kSisNoteOff - 1));
        plan_ = planSlices(sisterPos_, budget, kSliceMinEvents);
        size_t worst = 0;
        for (const SlicePlan& spn : plan_)
            if (spn.poolEvents() > worst) worst = spn.poolEvents();
        if (plan_.empty() || worst > arenaEvents()) {
            LOGE("streamer: a slice needs %zu events and the arena holds only %zu "
                 "— the polyphony at one boundary will not fit", worst, arenaEvents());
            noAddrSpace_ = true;
            close();
            return false;
        }
        LOGI("streamer: %zu slice%s planned, worst %zu of %zu arena events",
             plan_.size(), plan_.size() == 1 ? "" : "s", worst, arenaEvents());
        startBuilder();
        if (!seekSlice(0, firstNoteUs_ - 3000000)) {
            LOGE("streamer: the first slice could not be materialised");
            close();
            return false;
        }
    }

    progress.store(1.0f);
    LOGI("streamer: %zu notes (%zu events), %d tracks, %.1f s | pool %.1f MB on disk, "
         "window %+.2fs/%-.2fs%s, tables %.1f MB resident, %s sort%s",
         out.noteCount(), totalEvents, out.trackCount, out.totalUs / 1e6,
         fullPoolBytes_ / 1048576.0, frontUs_ / 1e6, backUs_ / 1e6,
         frontPinned_ ? " (pinned)" : "",
         (out.events.size() * sizeof(PlayEvent*) + sisterPos_.size() * 4 +
          poolIdx_.size() * 4 + posUs_.size() * 4) / 1048576.0,
         chunked ? "chunked" : "in-RAM",
         sliced ? ", sliced" : "");
    return true;
}

void Streamer::close() {
    stopLoader();
    stopBuilder();
    // The arenas are unmapped below as arenas, not as pool segments — segs_
    // only ever borrows the current one.
    if (sliced_) { retireSlice(cur_); segs_.clear(); }
    for (const PoolSeg& sg : segs_) munmap(sg.va, sg.len);
    segs_.clear();
    for (int i = 0; i < 2; i++) {
        if (arena_[i]) munmap(arena_[i], arenaBytes_);
        arena_[i] = nullptr;
    }
    arenaBytes_ = 0;
    for (int f : poolFds_) if (f >= 0) ::close(f);
    poolFds_.clear();
    poolFds_.shrink_to_fit();
    sliced_ = false;
    sliceLive_ = false;
    fullPoolBytes_ = 0;
    firstNoteUs_ = 0;
    eventsMut_ = nullptr;
    poolDirUsed_.clear();
    poolKeepNamed_ = false;
    plan_.clear();     plan_.shrink_to_fit();
    poolIdx_.clear();  poolIdx_.shrink_to_fit();
    posUs_.clear();    posUs_.shrink_to_fit();
    pcData_.clear();   pcData_.shrink_to_fit();
    base_ = nullptr;
    // Named temps (SD-card pool dir) are ours to remove — after the mapping is
    // gone, so nothing is reading them. The internal path unlinks at creation
    // and leaves this empty.
    for (const std::string& p : tempPaths_) unlink(p.c_str());
    tempPaths_.clear();
    tempPaths_.shrink_to_fit();
    mapLen_ = poolBytes_ = totalEvents_ = 0;
    sisterPos_.clear();      sisterPos_.shrink_to_fit();
    trackRange_.clear();     trackRange_.shrink_to_fit();
    trackSamples_.clear();   trackSamples_.shrink_to_fit();
    trackSampleOff_.clear(); trackSampleOff_.shrink_to_fit();
    posTimes_.clear();       posTimes_.shrink_to_fit();
    pinnedPages_.clear();
    events_ = nullptr;
    trackLo_.clear();        trackLo_.shrink_to_fit();
    trackHi_.clear();        trackHi_.shrink_to_fit();
    adviseCursor_ = 0;
    winFrontUs_.store(0, std::memory_order_relaxed);
    winBytes_.store(0, std::memory_order_relaxed);
    memAvail_.store(0, std::memory_order_relaxed);
}

// ---- sliced pool (32-bit only; see SLICED-POOL-DESIGN.md) ---------------------
//
// The pool file is written exactly as before and then never mapped. Instead the
// song is cut into slices (planSlices), each slice is materialised into its own
// compact pool laid out track by track — the full pool's own order, restricted
// to the slice — and mapped read-only over one of two fixed arenas. Because
// every slice of a given parity maps at the SAME address, the materialiser can
// bake real `sister` pointers into the file, and because events[] keeps global
// positions and is repointed a slice at a time, dispatch() and buildVisible()
// never learn that any of this is happening. That is the whole point: the
// address space stops scaling with the song, and the hot path pays nothing.

size_t Streamer::posForTime(int64_t us) const {
    // events[] is time-sorted, so this is applySeek's own binary search run
    // against the resident time table instead of a pool that is not mapped.
    size_t lo = 0, hi = totalEvents_;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (static_cast<int64_t>(posUs_[mid]) <= us) lo = mid + 1; else hi = mid;
    }
    return lo;
}

void Streamer::startBuilder() {
    if (buildThread_.joinable()) return;
    buildQuit_ = false;
    buildAbort_.store(false);
    buildThread_ = std::thread(&Streamer::builderMain, this);
}

void Streamer::stopBuilder() {
    if (buildThread_.joinable()) {
        buildAbort_.store(true);
        {
            std::lock_guard<std::mutex> lk(buildMutex_);
            buildQuit_ = true;
        }
        buildCv_.notify_all();
        buildThread_.join();
    }
    SliceMap stale;
    {
        std::lock_guard<std::mutex> lk(buildMutex_);
        stale = std::move(buildOut_);
        buildOut_   = SliceMap{};
        buildState_ = 0;
        buildWant_  = -1;
        buildQuit_  = false;
    }
    buildAbort_.store(false);
    retireSlice(stale);
}

void Streamer::builderMain() {
#if defined(__ANDROID__)
    // Same placement policy as the loader: everywhere BUT the engine core, so
    // materialising the next slice never contends with dispatch/render.
    int ncpu = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    uint64_t ncpuMask = (ncpu >= 64) ? ~0ULL : ((1ULL << ncpu) - 1ULL);
    uint64_t avoid = (~engineCpuMask_) & ncpuMask;
    if (engineCpuMask_ != 0) setThreadAffinityMaskSelf(avoid);
#elif defined(__APPLE__)
    apfa::platform::setAuxThreadPolicy();
#endif
    for (;;) {
        int want;
        {
            std::unique_lock<std::mutex> lk(buildMutex_);
            buildCv_.wait(lk, [this] {
                return buildQuit_ || (buildState_ == 1 && buildWant_ >= 0);
            });
            if (buildQuit_) return;
            want = buildWant_;
        }
        SliceMap m;
        bool ok = buildSlice(want, m);
        {
            std::lock_guard<std::mutex> lk(buildMutex_);
            buildOut_   = std::move(m);
            buildState_ = ok ? 2 : 3;
        }
        buildCv_.notify_all();
    }
}

void Streamer::requestBuild(int sliceIdx) {
    if (sliceIdx < 0 || static_cast<size_t>(sliceIdx) >= plan_.size()) return;
    if (!buildThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(buildMutex_);
        if (buildState_ != 0) return;      // busy, or holding an untaken result
        buildWant_  = sliceIdx;
        buildState_ = 1;
    }
    buildCv_.notify_all();
}

bool Streamer::takeBuild(int sliceIdx, SliceMap& out) {
    SliceMap stale;
    bool got = false;
    {
        std::unique_lock<std::mutex> lk(buildMutex_);
        buildCv_.wait(lk, [this] { return buildState_ != 1; });
        if (buildState_ == 2) {
            if (buildOut_.idx == sliceIdx) { out = std::move(buildOut_); got = true; }
            else                            stale = std::move(buildOut_);
        }
        buildOut_   = SliceMap{};
        buildState_ = 0;
        buildWant_  = -1;
    }
    // Whatever the builder had is not what was asked for — and it is holding an
    // arena, so it has to go before anything else can use one.
    retireSlice(stale);
    return got;
}

void Streamer::retireSlice(SliceMap& s) {
    if (s.idx >= 0 && arena_[s.arena] != nullptr) {
        // Put the PROT_NONE reservation back over the arena. This drops the
        // slice file's mapping (so its blocks are reclaimed the moment the temp
        // is unlinked) and leaves nothing readable behind for a stray pointer.
        mmap(arena_[s.arena], arenaBytes_, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    }
    if (s.fd >= 0) { ::close(s.fd); s.fd = -1; }
    if (!s.path.empty()) { unlink(s.path.c_str()); s.path.clear(); }
    s = SliceMap{};
}

// Materialise plan_[sliceIdx] into arena_[sliceIdx & 1].
bool Streamer::buildSlice(int sliceIdx, SliceMap& out) {
    out = SliceMap{};
    if (sliceIdx < 0 || static_cast<size_t>(sliceIdx) >= plan_.size()) return false;
    const SlicePlan& sp   = plan_[static_cast<size_t>(sliceIdx)];
    const size_t firstPos = sp.firstPos;
    const size_t endPos   = sp.endPos;
    const size_t cap      = arenaEvents();
    const uint64_t t0     = monoUs();

    // ---- carry-in: every note-on still sounding at firstPos ----
    // One forward pass over the pairing table. The number of notes sounding at
    // a position IS the carry at that position (SLICED-POOL-DESIGN.md), so the
    // same walk yields both the set at firstPos and the running count at endPos.
    std::vector<uint32_t> carryIn;
    carryIn.reserve(sp.carryIn);
    size_t sounding = 0;
    for (size_t pos = 0; pos < endPos; pos++) {
        uint32_t s = sisterPos_[pos];
        if (s == kSisNonNote) continue;
        if (s == kSisNoteOff) { if (sounding) sounding--; continue; }
        sounding++;
        if (pos < firstPos && static_cast<size_t>(s) >= firstPos)
            carryIn.push_back(static_cast<uint32_t>(pos));
    }

    // ---- read-ahead extension ----
    // buildVisible() reads AHEAD of the playhead, so a slice that stopped at
    // its transition point would have the window cursor running off the end of
    // the mapping. Carry the body on for one front horizon past endPos — the
    // same horizon the loader advises over, so invariant 5 holds too: the
    // loader can only MADV_WILLNEED what is mapped. Stop early if the arena
    // fills first.
    const int64_t lookaheadUs = frontUs_ > 0 ? frontUs_ : kLeadUs;
    const int64_t limitUs = (endPos > 0 ? static_cast<int64_t>(posUs_[endPos - 1]) : 0)
                          + lookaheadUs;
    size_t matEnd = endPos;
    while (matEnd < totalEvents_) {
        if (static_cast<int64_t>(posUs_[matEnd]) > limitUs) break;
        uint32_t s = sisterPos_[matEnd];
        size_t after = sounding;
        if (s == kSisNoteOff) { if (after) after--; }
        else if (s != kSisNonNote) after++;
        if (carryIn.size() + (matEnd + 1 - firstPos) + after > cap) break;
        sounding = after;
        matEnd++;
    }

    // ---- carry-out: the note-off of everything still sounding at matEnd ----
    // Without these a body note-on would have no `sister` to point at, and
    // buildVisible reads exactly that pointer for the note's duration.
    std::vector<uint32_t> carryOut;
    carryOut.reserve(sounding);
    for (size_t pos = 0; pos < matEnd; pos++) {
        uint32_t s = sisterPos_[pos];
        if (s < kSisNoteOff && static_cast<size_t>(s) >= matEnd)
            carryOut.push_back(s);
    }
    std::sort(carryOut.begin(), carryOut.end());

    const size_t total = carryIn.size() + (matEnd - firstPos) + carryOut.size();
    if (total == 0 || total > cap) {
        LOGE("streamer: slice %d wants %zu events, arena holds %zu",
             sliceIdx, total, cap);
        return false;
    }

    // ---- lay it out track by track ----
    // The full pool is in PARSE order, which is track-major, so sorting the
    // slice's events by pool index reproduces exactly that layout restricted to
    // the slice: the same ~76 interleaved streams, the same scatter under the
    // time-ordered walk, only a shorter address span — and span alone costs
    // nothing. It also makes slot order and pool-file read order the same
    // order, so the materialiser reads and writes sequentially.
    struct Need   { uint32_t poolIdx, pos; };
    struct PosSlot{ uint32_t pos, slot; };
    std::vector<Need> need;
    need.reserve(total);
    for (uint32_t p : carryIn)  need.push_back({ poolIdx_[p], p });
    for (size_t p = firstPos; p < matEnd; p++)
        need.push_back({ poolIdx_[p], static_cast<uint32_t>(p) });
    for (uint32_t p : carryOut) need.push_back({ poolIdx_[p], p });
    std::sort(need.begin(), need.end(),
              [](const Need& a, const Need& b) { return a.poolIdx < b.poolIdx; });

    out.bodySlot.assign(matEnd - firstPos, 0);
    std::vector<PosSlot> carry;
    carry.reserve(carryIn.size() + carryOut.size());
    for (size_t i = 0; i < need.size(); i++) {
        uint32_t p = need[i].pos;
        if (p >= firstPos && p < matEnd) out.bodySlot[p - firstPos] = static_cast<uint32_t>(i);
        else carry.push_back({ p, static_cast<uint32_t>(i) });
    }
    std::sort(carry.begin(), carry.end(),
              [](const PosSlot& a, const PosSlot& b) { return a.pos < b.pos; });
    out.carryPos.reserve(carry.size());
    out.carrySlot.reserve(carry.size());
    for (const PosSlot& c : carry) {
        out.carryPos.push_back(c.pos);
        out.carrySlot.push_back(c.slot);
    }

    // ---- write the slice pool ----
    out.idx      = sliceIdx;
    out.arena    = sliceIdx & 1;
    out.firstPos = firstPos;
    out.endPos   = endPos;
    out.matEnd   = matEnd;
    out.slots    = total;
    out.bytes    = total * kEventSize;
    uint8_t* const arenaBase = arena_[out.arena];

    int fd = makeTemp(poolDirUsed_, "slice", poolKeepNamed_ ? &out.path : nullptr);
    if (fd < 0) {
        LOGE("streamer: slice temp failed in %s (errno=%d)", poolDirUsed_.c_str(), errno);
        return false;
    }
    if (wouldExhaustDisk(fd, out.bytes)) {
        LOGE("streamer: slice %d (%.1f MB) would exhaust free storage",
             sliceIdx, out.bytes / 1048576.0);
        diskFull_ = true;
        ::close(fd);
        if (!out.path.empty()) unlink(out.path.c_str());
        out = SliceMap{};
        return false;
    }

    std::vector<PlayEvent> rbuf(kSliceChunkEvents);
    std::vector<PlayEvent> wbuf;
    wbuf.reserve(kBufEvents);
    size_t curTrack = static_cast<size_t>(-1);
    size_t inTrack  = 0;
    bool ok = true;
    size_t i = 0;
    while (i < need.size() && ok) {
        if (buildAbort_.load(std::memory_order_relaxed)) { ok = false; break; }
        // Read from the first wanted event up to the last one that is still
        // worth reaching in the same pread: a run of unwanted events longer
        // than kSliceGapEvents costs more in bytes than one more seek does.
        size_t j = i;
        while (j + 1 < need.size() &&
               need[j + 1].poolIdx - need[j].poolIdx <= kSliceGapEvents &&
               need[j + 1].poolIdx - need[i].poolIdx < kSliceChunkEvents)
            j++;
        const uint32_t base = need[i].poolIdx;
        const size_t   span = need[j].poolIdx - base + 1;
        if (!poolPread(poolFds_, rbuf.data(), span * kEventSize,
                       static_cast<uint64_t>(base) * kEventSize)) {
            LOGE("streamer: slice %d pool read failed at +%u (errno=%d)",
                 sliceIdx, base, errno);
            ok = false;
            break;
        }
        for (size_t k = i; k <= j; k++) {
            PlayEvent e = rbuf[need[k].poolIdx - base];
            // Pass B stored the partner's POOL INDEX + 1 in `sister` (0 for a
            // non-note event, whose sister is PFA's &pool[0] artifact). Resolve
            // it against the arena: every partner is in this slice by
            // construction — a note-on's off is in the body or the carry-out, a
            // note-off's on is in the body or the carry-in.
            uintptr_t enc = reinterpret_cast<uintptr_t>(e.sister);
            size_t slot = 0;
            if (enc != 0) {
                uint32_t partner = static_cast<uint32_t>(enc - 1);
                const Need* f = std::lower_bound(
                    need.data(), need.data() + need.size(), partner,
                    [](const Need& a, uint32_t v) { return a.poolIdx < v; });
                if (f != need.data() + need.size() && f->poolIdx == partner)
                    slot = static_cast<size_t>(f - need.data());
            }
            e.sister = reinterpret_cast<PlayEvent*>(arenaBase + slot * kEventSize);

            // The loader's per-track index, rebuilt against slot numbers. Pool
            // order is track-major, so tracks arrive in ascending order and
            // each one's slots are contiguous.
            size_t trk = e.track < 0 ? 0 : static_cast<size_t>(e.track);
            while (out.trackRange.size() <= trk) {
                out.trackRange.push_back(TrackRange{ k, 0 });
                out.trackSampleOff.push_back(static_cast<uint32_t>(out.trackSamples.size()));
            }
            if (trk != curTrack) { curTrack = trk; inTrack = 0; }
            if (out.trackRange[trk].count == 0) out.trackRange[trk].first = k;
            out.trackRange[trk].count++;
            if (inTrack % kTrackSampleStep == 0)
                out.trackSamples.push_back(
                    TrackSample{ e.absMicroSec, static_cast<uint32_t>(k) });
            inTrack++;

            wbuf.push_back(e);
            if (wbuf.size() >= kBufEvents) {
                if (!writeAll(fd, wbuf.data(), wbuf.size() * kEventSize)) {
                    if (errno == EFBIG) fileTooBig_ = true;
                    LOGE("streamer: slice %d write failed (errno=%d)", sliceIdx, errno);
                    ok = false;
                    break;
                }
                wbuf.clear();
            }
        }
        i = j + 1;
    }
    if (ok && !wbuf.empty() &&
        !writeAll(fd, wbuf.data(), wbuf.size() * kEventSize)) {
        if (errno == EFBIG) fileTooBig_ = true;
        LOGE("streamer: slice %d write failed (errno=%d)", sliceIdx, errno);
        ok = false;
    }
    out.trackSampleOff.push_back(static_cast<uint32_t>(out.trackSamples.size()));

    // ---- map it over its arena ----
    if (ok) {
        size_t mapBytes = (out.bytes + kPageSize - 1) & ~(kPageSize - 1);
        if (mapBytes > arenaBytes_) { ok = false; }
        else {
            void* m = mmap(arenaBase, mapBytes, PROT_READ, MAP_FIXED | MAP_PRIVATE,
                           fd, 0);
            if (m == MAP_FAILED || m != arenaBase) {
                LOGE("streamer: slice %d mmap failed (errno=%d)", sliceIdx, errno);
                ok = false;
            }
        }
    }
    // The mapping holds the inode, so the fd has done its job either way.
    ::close(fd);
    if (!ok) {
        if (!out.path.empty()) unlink(out.path.c_str());
        out = SliceMap{};
        return false;
    }
    // Invariant 5: the read-ahead extension must cover a full front horizon, or
    // buildVisible's window cursor stops short of the visible band and the
    // loader loses its lead. It only ever falls short when the arena filled
    // first, which the plan's quarter of headroom is there to prevent.
    if (matEnd < totalEvents_ &&
        static_cast<int64_t>(posUs_[matEnd]) <= limitUs)
        LOGE("streamer: slice %d's read-ahead was cut short by the arena "
             "(%zu of the %lld us horizon) — notes may pop in late",
             sliceIdx, matEnd - endPos, static_cast<long long>(lookaheadUs));
    LOGI("streamer: slice %d/%zu built — pos %zu..%zu (+%zu read-ahead), "
         "%zu events (%zu carry) in %.1f MB, %llu ms",
         sliceIdx, plan_.size(), firstPos, endPos, matEnd - endPos, total,
         carry.size(), out.bytes / 1048576.0,
         static_cast<unsigned long long>((monoUs() - t0) / 1000));
    return true;
}

void Streamer::installSliceLocked(SliceMap& s, int64_t playheadUs) {
    std::vector<PlayEvent*>& ev = *eventsMut_;
    uint8_t* const base = arena_[s.arena];
    // Repoint events[] at the slice. Global positions never change, so the
    // engine's cursors, active_ and noteState_ stay exactly what they were —
    // the notes sounding across a boundary ARE the next slice's carry-in, so
    // the entries active_ holds are among the ones written here.
    for (size_t i = 0; i < s.bodySlot.size(); i++)
        ev[s.firstPos + i] = reinterpret_cast<PlayEvent*>(
            base + static_cast<size_t>(s.bodySlot[i]) * kEventSize);
    for (size_t i = 0; i < s.carryPos.size(); i++)
        ev[s.carryPos[i]] = reinterpret_cast<PlayEvent*>(
            base + static_cast<size_t>(s.carrySlot[i]) * kEventSize);

    // The loader's byte arithmetic is unchanged — it just addresses the arena
    // now, with the slice's own per-track index behind it.
    segs_.assign(1, PoolSeg{ base, 0, arenaBytes_ });
    base_           = base;
    poolBytes_      = s.bytes;
    mapLen_         = arenaBytes_;
    trackRange_     = s.trackRange;
    trackSampleOff_ = s.trackSampleOff;
    trackSamples_   = s.trackSamples;
    pinnedPages_.clear();
    trackLo_.clear();
    trackHi_.clear();
    sliceLive_ = true;
    resetWindowLocked(playheadUs);

    // Carry-in note-ons are behind the window from the moment the slice opens,
    // and the O(P) scan reads them until their note-off dispatches. Pin them
    // here, because the back-edge scan that would normally do it never passes
    // over their positions in this slice.
    for (size_t i = 0; i < s.carryPos.size(); i++) {
        if (s.carryPos[i] >= s.firstPos) break;      // carry-out, sorted after
        uint32_t off = sisterPos_[s.carryPos[i]];
        if (off >= kSisNoteOff) continue;
        uintptr_t a = reinterpret_cast<uintptr_t>(
            base + static_cast<size_t>(s.carrySlot[i]) * kEventSize);
        uintptr_t p1 = a & ~(kPageSize - 1);
        uintptr_t p2 = (a + kEventSize - 1) & ~(kPageSize - 1);
        uint32_t& r1 = pinnedPages_[p1];
        if (off > r1) r1 = off;
        if (p2 != p1) {
            uint32_t& r2 = pinnedPages_[p2];
            if (off > r2) r2 = off;
        }
    }
}

bool Streamer::advanceSlice(int64_t playheadUs) {
    if (!sliced_) return false;
    const int want = cur_.idx + 1;
    if (want <= 0 || static_cast<size_t>(want) >= plan_.size()) return false;

    SliceMap next;
    if (!takeBuild(want, next)) {
        // The builder has not got there. Do it here and take the stall: that is
        // ordinary storage lag entering the clock, exactly as a major fault is,
        // and it is what the design accepted in exchange for a free hot path.
        if (!buildSlice(want, next)) {
            LOGE("streamer: slice %d unavailable — playback stops here", want);
            return false;
        }
    }
    SliceMap old;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        old  = std::move(cur_);
        cur_ = std::move(next);
        installSliceLocked(cur_, playheadUs);
    }
    retireSlice(old);
    requestBuild(cur_.idx + 1);
    return true;
}

bool Streamer::seekSlice(size_t pos, int64_t playheadUs) {
    if (!sliced_) return true;
    if (plan_.empty()) return false;
    if (pos >= totalEvents_) pos = totalEvents_ - 1;

    // Bodies tile the walk, so the owning slice is a plain upper-bound search
    // over the boundaries.
    size_t lo = 0, hi = plan_.size();
    while (lo + 1 < hi) {
        size_t mid = (lo + hi) / 2;
        if (plan_[mid].firstPos <= pos) lo = mid; else hi = mid;
    }
    const int want = static_cast<int>(lo);

    if (cur_.idx == want && sliceLive_) {
        std::lock_guard<std::mutex> lk(mutex_);
        resetWindowLocked(playheadUs);
        return true;
    }

    SliceMap next;
    if (!takeBuild(want, next)) {
        // The target needs the arena the playing slice is in (slices alternate,
        // so that is every seek of the same parity). Stand the loader off, drop
        // the slice, and build here — a seek is a deliberate action and the
        // design accepts a rebuild, i.e. a pause, for one.
        if (cur_.idx >= 0 && (cur_.idx & 1) == (want & 1)) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                sliceLive_ = false;
            }
            retireSlice(cur_);
        }
        if (!buildSlice(want, next)) {
            LOGE("streamer: slice %d could not be built for the seek", want);
            return false;
        }
    }
    SliceMap old;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        old  = std::move(cur_);
        cur_ = std::move(next);
        installSliceLocked(cur_, playheadUs);
    }
    retireSlice(old);
    requestBuild(want + 1);
    return true;
}

// ---- window navigation --------------------------------------------------------

void Streamer::trackWindow(int track, int64_t fromUs, int64_t toUs,
                           size_t& outFirst, size_t& outLast) const {
    const TrackRange& tr = trackRange_[track];
    if (tr.count == 0) { outFirst = 1; outLast = 0; return; }   // empty
    const uint32_t s0 = trackSampleOff_[track];
    const uint32_t s1 = trackSampleOff_[track + 1];

    // floor sample for fromUs
    size_t first = tr.first;
    {
        uint32_t lo = s0, hi = s1;
        while (lo < hi) {                    // first sample with us > fromUs
            uint32_t mid = (lo + hi) / 2;
            if (trackSamples_[mid].us > fromUs) hi = mid; else lo = mid + 1;
        }
        if (lo > s0) first = trackSamples_[lo - 1].poolIdx;
    }
    // ceiling sample for toUs
    size_t last = tr.first + tr.count - 1;
    {
        uint32_t lo = s0, hi = s1;
        while (lo < hi) {                    // first sample with us > toUs
            uint32_t mid = (lo + hi) / 2;
            if (trackSamples_[mid].us > toUs) hi = mid; else lo = mid + 1;
        }
        if (lo < s1) last = trackSamples_[lo].poolIdx;   // one sample past = safe ceil
    }
    outFirst = first;
    outLast  = last;
}

size_t Streamer::coarsePosOf(int64_t us) const {
    if (posTimes_.empty()) return 0;
    size_t lo = 0, hi = posTimes_.size();
    while (lo < hi) {                        // first sample with time > us
        size_t mid = (lo + hi) / 2;
        if (posTimes_[mid] > us) hi = mid; else lo = mid + 1;
    }
    return (lo > 0 ? lo - 1 : 0) * kPosSampleStep;
}

void Streamer::touchPoolRange(size_t firstByte, size_t lastByte, bool willneed) {
    if (firstByte > lastByte || lastByte >= poolBytes_) {
        if (lastByte >= poolBytes_) lastByte = poolBytes_ ? poolBytes_ - 1 : 0;
        if (firstByte > lastByte) return;
    }
    forEachSegRange(firstByte & ~(kPageSize - 1), lastByte,
                    [&](uint8_t* a, size_t len) {
        if (willneed) madvise(a, len, MADV_WILLNEED);
        touchRange(a, a + len - 1);
    });
}

void Streamer::advisePoolRange(size_t firstByte, size_t lastByte) {
    if (firstByte > lastByte || lastByte >= poolBytes_) {
        if (lastByte >= poolBytes_) lastByte = poolBytes_ ? poolBytes_ - 1 : 0;
        if (firstByte > lastByte) return;
    }
    // Advise and return. No touchRange: waiting on the read here would put the
    // loader thread in front of a synchronous disk queue AND serialise the very
    // readahead the advisory just scheduled, which is what made beta1's cost
    // track storage speed instead of engine demand.
    forEachSegRange(firstByte & ~(kPageSize - 1), lastByte,
                    [](uint8_t* a, size_t len) {
        madvise(a, len, MADV_WILLNEED);
    });
}

int64_t Streamer::budgetedFrontUs(int64_t t) {
    uint64_t avail = readMemAvailable();
    memAvail_.store(avail, std::memory_order_relaxed);

    int64_t front = frontUs_;
    // A sliced load's window can only ever cover the mapped slice, so price it
    // against that — otherwise the horizon is charged for bytes it cannot warm.
    const size_t lowPos  = sliced_ ? cur_.firstPos : 0;
    const size_t highPos = sliced_ ? cur_.matEnd   : totalEvents_;
    size_t backPos = std::max(coarsePosOf(t - backUs_), lowPos);
    auto priceOf = [&](int64_t f) -> uint64_t {
        size_t hi = std::min(coarsePosOf(t + f) + kPosSampleStep, highPos);
        return (hi > backPos ? static_cast<uint64_t>(hi - backPos) : 0ull) * kEventSize;
    };

    if (!frontPinned_ && avail > 0) {
        const uint64_t budget = static_cast<uint64_t>(avail * kWindowMemFraction);
        // The horizon-to-bytes curve is monotone but not linear (density
        // varies), so scale by the overshoot and re-price. A handful of
        // iterations converges; each is one binary search over posTimes_.
        for (int i = 0; i < 8; i++) {
            uint64_t price = priceOf(front);
            if (price <= budget || front <= kMinFrontUs) break;
            int64_t scaled = static_cast<int64_t>(
                static_cast<double>(front) * static_cast<double>(budget) /
                static_cast<double>(price));
            // Never trust the scale to shrink on its own — density ahead of the
            // playhead can be lower than the window's average.
            if (scaled >= front) scaled = front * 3 / 4;
            front = std::max(scaled, kMinFrontUs);
        }
    }
    winFrontUs_.store(front, std::memory_order_relaxed);
    winBytes_.store(priceOf(front), std::memory_order_relaxed);
    return front;
}

void Streamer::setVisibleUs(int64_t visibleUs) {
    if (visibleUs < 0) visibleUs = 0;
    visibleUs_ = visibleUs;
    // Keep frontUs_ consistent whether this lands before or after open().
    if (!frontPinned_) frontUs_ = visibleUs_ + kLeadUs;
}

void Streamer::resetWindowLocked(int64_t aroundUs) {
    trackLo_.assign(trackRange_.size(), 0);
    trackHi_.assign(trackRange_.size(), 0);
    for (size_t t = 0; t < trackRange_.size(); t++) {
        size_t f, l;
        trackWindow(static_cast<int>(t), aroundUs - backUs_, aroundUs - backUs_, f, l);
        size_t startByte = (f <= l ? f : trackRange_[t].first) * kEventSize;
        trackLo_[t] = trackHi_[t] = startByte;
    }
    frontPos_ = coarsePosOf(aroundUs);
    backPos_  = coarsePosOf(aroundUs - backUs_);
    if (sliced_) {
        // Nothing before the slice's first position is mapped, and the carry-in
        // note-ons the back-edge scan would have found there are pinned by
        // installSliceLocked instead.
        if (frontPos_ < cur_.firstPos) frontPos_ = cur_.firstPos;
        if (backPos_  < cur_.firstPos) backPos_  = cur_.firstPos;
    }
}

// ---- loader thread --------------------------------------------------------------

void Streamer::startLoader(const std::atomic<int64_t>* playheadUs,
                           int64_t initialUs, uint64_t engineCpuMask) {
    if (loaderRunning_.load() || !isOpen()) return;
    playheadUs_    = playheadUs;
    initialUs_     = initialUs;
    engineCpuMask_ = engineCpuMask;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        resetWindowLocked(initialUs);
    }
    loaderRunning_ = true;
    loaderThread_  = std::thread(&Streamer::loaderMain, this);
}

void Streamer::stopLoader() {
    loaderRunning_ = false;
    if (loaderThread_.joinable()) loaderThread_.join();
    playheadUs_ = nullptr;
}

void Streamer::loaderMain() {
#if defined(__ANDROID__)
    // Same placement policy as the BASS render threads: everywhere BUT the
    // engine core, so read-ahead I/O never contends with dispatch/render.
    int ncpu = static_cast<int>(sysconf(_SC_NPROCESSORS_CONF));
    uint64_t ncpuMask = (ncpu >= 64) ? ~0ULL : ((1ULL << ncpu) - 1ULL);
    uint64_t avoid = (~engineCpuMask_) & ncpuMask;
    if (engineCpuMask_ != 0) setThreadAffinityMaskSelf(avoid);
#elif defined(__APPLE__)
    apfa::platform::setAuxThreadPolicy();
#endif
    bool seenPlayhead = false;
    while (loaderRunning_.load()) {
        int64_t t = playheadUs_ ? playheadUs_->load() : 0;
        if (t != 0) seenPlayhead = true;
        if (!seenPlayhead) t = initialUs_;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            loaderTickLocked(t);
        }
        // Read-ahead faults, published for Engine::frame's "fault:" line.
        // Sampled outside the lock: the engine reads this without the mutex.
        {
            struct rusage ru;
            if (getrusage(RUSAGE_THREAD, &ru) == 0)
                loaderMajFlt_.store(static_cast<uint64_t>(ru.ru_majflt),
                                    std::memory_order_relaxed);
        }
        // 100 ms cadence. The horizon is only ~2 s now, so a tick advances the
        // front edge by ~5% of the window — the advisories stay well ahead of
        // the playhead without ever handing the kernel a burst.
        usleep(100000);
    }
}

void Streamer::loaderTickLocked(int64_t t) {
    // Between dropping one slice and installing the next there is nothing to
    // advise and nothing safe to touch — the arena is a PROT_NONE reservation.
    if (sliced_ && !sliceLive_) return;
    const std::vector<PlayEvent*>& ev = *events_;
    // Only positions inside the mapped slice have live events[] pointers, so
    // that — not the whole song — is what every scan below is bounded by.
    const size_t n = sliced_ ? cur_.matEnd : totalEvents_;
    size_t curPos = coarsePosOf(t);
    // coarsePosOf floors to a sample boundary, so just after a transition it can
    // land before the slice starts; the "still sounding" tests below rely on it
    // being at or after firstPos to mean "this note-on is a carry-in".
    if (sliced_ && curPos < cur_.firstPos) curPos = cur_.firstPos;
    if (curPos > n) curPos = n;

    // Horizon for this tick: visible band + lead, shrunk if the window would
    // not fit in MemAvailable. Recomputed every tick because the price of a
    // fixed horizon swings with event density (§6 of PERF-FINDINGS: 720 MB at
    // U11's average, 3.17 GB at its collapse).
    const int64_t frontUs = budgetedFrontUs(t);

    // Front-edge scan: pre-touch the note-offs of long notes entering the
    // window, so buildVisible's duration read (e->sister->absMicroSec) never
    // faults on the engine thread. These stay SYNCHRONOUS — one scattered page
    // each, and blocking here is how the loader paces itself against storage
    // instead of racing ahead of it.
    //
    // A shorter horizon puts more note-offs past the front edge, so the count
    // is capped: past the cap the remaining sisters are left to fault on the
    // engine thread, which is what PFA does with all of them. Better to spend
    // a bounded slice of the tick here than to let one dense stretch of long
    // notes starve the bulk advisories below.
    size_t newFront = std::min(coarsePosOf(t + frontUs) + kPosSampleStep, n);
    size_t sisterLeft = kMaxSisterTouchPerTick;
    for (size_t pos = frontPos_; pos < newFront; pos++) {
        uint32_t s = sisterPos_[pos];
        if (s < kSisNoteOff && static_cast<size_t>(s) > newFront) {
            if (sisterLeft == 0) continue;
            sisterLeft--;
            const uint8_t* off = reinterpret_cast<const uint8_t*>(ev[s]);
            touchRange(off, off + kEventSize - 1);
        }
    }
    if (newFront > frontPos_) frontPos_ = newFront;

    // Back-edge scan: note-ons leaving the window that are STILL sounding
    // (their off is ahead of the playhead) get pinned — the O(P) note-off
    // scan and buildVisible keep reading them until the off dispatches.
    size_t newBack = std::min(coarsePosOf(t - backUs_), n);
    for (size_t pos = backPos_; pos < newBack; pos++) {
        uint32_t s = sisterPos_[pos];
        if (s < kSisNoteOff && static_cast<size_t>(s) > curPos) {
            uintptr_t page = reinterpret_cast<uintptr_t>(ev[pos]) & ~(kPageSize - 1);
            uintptr_t page2 = (reinterpret_cast<uintptr_t>(ev[pos]) + kEventSize - 1)
                              & ~(kPageSize - 1);
            auto& rel = pinnedPages_[page];
            if (s > rel) rel = s;
            if (page2 != page) {
                auto& rel2 = pinnedPages_[page2];
                if (s > rel2) rel2 = s;
            }
        }
    }
    if (newBack > backPos_) backPos_ = newBack;

    // Per-track sliding window: advise new bytes ahead, drop bytes behind.
    // Tracks are visited from a rotating start so that when the per-tick
    // ceiling binds (initial fill, post-seek refill) it is not always the same
    // low-numbered tracks that consume it — every track gets warmed.
    const size_t nTracks = trackRange_.size();
    size_t adviseLeft = kMaxAdviseBytesPerTick;
    if (nTracks != 0) adviseCursor_ %= nTracks;
    for (size_t i = 0; i < nTracks; i++) {
        size_t trk = (adviseCursor_ + i) % nTracks;
        if (trackRange_[trk].count == 0) continue;
        size_t f, l;
        trackWindow(static_cast<int>(trk), t - backUs_, t + frontUs, f, l);
        if (f > l) continue;
        size_t loByte = f * kEventSize;
        size_t hiByte = l * kEventSize + kEventSize - 1;
        if (hiByte >= poolBytes_) hiByte = poolBytes_ - 1;

        if (hiByte + 1 > trackHi_[trk]) {          // advise the newly-entered range
            size_t from = std::max(trackHi_[trk], loByte);
            if (from <= hiByte) {
                // Trim to the per-tick ceiling; trackHi_ only advances over
                // what was actually advised, so the remainder is picked up on
                // the next tick rather than silently skipped.
                size_t want = hiByte - from + 1;
                size_t take = std::min(want, adviseLeft);
                if (take > 0) {
                    advisePoolRange(from, from + take - 1);
                    adviseLeft -= take;
                    trackHi_[trk] = from + take;
                }
            } else {
                trackHi_[trk] = hiByte + 1;        // range already behind us
            }
        }
        if (loByte > trackLo_[trk] + (kPageSize << 8)) {   // retire ≥1 MB behind
            size_t lo = (trackLo_[trk] + kPageSize - 1) & ~(kPageSize - 1);
            size_t hi = loByte & ~(kPageSize - 1);
            if (hi > lo)
                forEachSegRange(lo, hi - 1, [](uint8_t* a, size_t len) {
                    madvise(a, len, MADV_DONTNEED);
                });
            trackLo_[trk] = loByte;
        }
    }
    adviseCursor_ += 1;

    // Pins: drop the completed ones, keep the rest warm (a re-touch after our
    // own DONTNEED costs one 4K read; there are few pins).
    for (auto it = pinnedPages_.begin(); it != pinnedPages_.end();) {
        if (static_cast<size_t>(it->second) <= curPos) {
            it = pinnedPages_.erase(it);
        } else {
            (void)*const_cast<volatile uint8_t*>(
                reinterpret_cast<const uint8_t*>(it->first));
            ++it;
        }
    }
}

// ---- seek -----------------------------------------------------------------------

void Streamer::warmSeek(int64_t targetUs, int64_t visibleEndUs,
                        const std::vector<int>& activePositions) {
    if (!isOpen()) return;
    if (sliced_ && !sliceLive_) return;
    std::lock_guard<std::mutex> lk(mutex_);
    const std::vector<PlayEvent*>& ev = *events_;

    resetWindowLocked(targetUs);
    frontPos_ = coarsePosOf(targetUs);   // let the next tick re-scan the window

    // Kick off readahead for the visible band on every track, then fault it in.
    int64_t warmFrom = targetUs - 2000000;
    int64_t warmTo   = visibleEndUs + 2000000;
    std::vector<std::pair<size_t, size_t>> ranges(trackRange_.size(), { 1, 0 });
    for (size_t trk = 0; trk < trackRange_.size(); trk++) {
        if (trackRange_[trk].count == 0) continue;
        size_t f, l;
        trackWindow(static_cast<int>(trk), warmFrom, warmTo, f, l);
        if (f > l) continue;
        size_t loByte = f * kEventSize;
        size_t hiByte = std::min(l * kEventSize + kEventSize - 1, poolBytes_ - 1);
        forEachSegRange(loByte & ~(kPageSize - 1), hiByte,
                        [](uint8_t* a, size_t len) {
            madvise(a, len, MADV_WILLNEED);
        });
        ranges[trk] = { loByte, hiByte };
    }
    for (size_t trk = 0; trk < ranges.size(); trk++) {
        if (ranges[trk].first > ranges[trk].second) continue;
        touchPoolRange(ranges[trk].first, ranges[trk].second, /*willneed=*/false);
        trackHi_[trk] = std::max(trackHi_[trk], ranges[trk].second + 1);
    }

    // Long-note offs inside the visible band (buildVisible reads their times
    // on the very next frame).
    size_t p0 = coarsePosOf(targetUs);
    size_t p1 = std::min(coarsePosOf(visibleEndUs) + kPosSampleStep, totalEvents_);
    if (sliced_) {
        if (p0 < cur_.firstPos) p0 = cur_.firstPos;
        if (p1 > cur_.matEnd)   p1 = cur_.matEnd;
        if (p0 > p1)            p0 = p1;
    }
    for (size_t pos = p0; pos < p1; pos++) {
        uint32_t s = sisterPos_[pos];
        if (s < kSisNoteOff && static_cast<size_t>(s) > p1) {
            const uint8_t* off = reinterpret_cast<const uint8_t*>(ev[s]);
            touchRange(off, off + kEventSize - 1);
        }
    }

    // Still-sounding note-ons resurrected by the seek: touch them (applySeek
    // reads param1 right after this) and their offs, and pin their pages.
    for (int posInt : activePositions) {
        size_t pos = static_cast<size_t>(posInt);
        const uint8_t* on = reinterpret_cast<const uint8_t*>(ev[pos]);
        touchRange(on, on + kEventSize - 1);
        uint32_t s = sisterPos_[pos];
        if (s < kSisNoteOff) {
            const uint8_t* off = reinterpret_cast<const uint8_t*>(ev[s]);
            touchRange(off, off + kEventSize - 1);
            uintptr_t page = reinterpret_cast<uintptr_t>(on) & ~(kPageSize - 1);
            uintptr_t page2 = (reinterpret_cast<uintptr_t>(on) + kEventSize - 1)
                              & ~(kPageSize - 1);
            auto& rel = pinnedPages_[page];
            if (s > rel) rel = s;
            if (page2 != page) {
                auto& rel2 = pinnedPages_[page2];
                if (s > rel2) rel2 = s;
            }
        }
    }
}

// ---- memory accounting ------------------------------------------------------------

size_t Streamer::memoryBytes() const {
    if (!isOpen()) return 0;
    // Under the lock throughout: a slice transition swaps the per-track tables
    // and the slot maps, and this is called from the UI thread.
    std::lock_guard<std::mutex> lk(mutex_);
    size_t tables = sisterPos_.capacity() * sizeof(uint32_t) +
                    trackRange_.capacity() * sizeof(TrackRange) +
                    trackSamples_.capacity() * sizeof(TrackSample) +
                    trackSampleOff_.capacity() * sizeof(uint32_t) +
                    posTimes_.capacity() * sizeof(int64_t) +
                    // sliced only: the tables that answer for positions no
                    // mapping covers, plus the slice's own slot maps.
                    poolIdx_.capacity() * sizeof(uint32_t) +
                    posUs_.capacity() * sizeof(uint32_t) +
                    pcData_.capacity() * sizeof(uint32_t) +
                    plan_.capacity() * sizeof(SlicePlan) +
                    cur_.bodySlot.capacity() * sizeof(uint32_t) +
                    (cur_.carryPos.capacity() + cur_.carrySlot.capacity()) *
                        sizeof(uint32_t);

    size_t window = 0;
    for (size_t t = 0; t < trackLo_.size(); t++)
        if (trackHi_[t] > trackLo_[t]) window += trackHi_[t] - trackLo_[t];
    window += pinnedPages_.size() * kPageSize;
    return tables + window;
}

}  // namespace apfa

#endif  // APFA_STREAMING
