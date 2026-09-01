// midi_parser.cpp — see midi_parser.h.
//
// Each MIDI note message becomes a PlayEvent appended to eventPool in FILE
// order — the note-on event the instant the note-on is parsed, the note-off
// event the instant the note-off is parsed — exactly as PFA does (one heap
// object per message). This is load-bearing: it leaves note-on events in
// start-time order, the order the dispatch loop and the O(P) UpdateState scan
// walk them, so the polyphony hot path streams the pool instead of pointer-
// chasing it. Building the pool in note-COMPLETION order (pairing on the
// note-off) instead places every note-on by its END time, scattering every
// note-on access on the polyphony path — that was aPFA's worse-than-PFA
// polyphony lag.
//
// Times are stored as TICKS during parsing and converted to microseconds in
// place; the `sister` field holds the paired event's INDEX until a final pass
// turns it into a pointer. events[] is the time-sorted pointer table walked at
// playback (note.h — this layout IS the cache behaviour).
#include "midi_parser.h"

#include <algorithm>
#include <cstdlib>   // srand (PFA seeds the RNG once at startup)
#include <ctime>     // time   (srand seed source)
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "platform.h"

namespace apfa {
namespace {

// Bounds-checked big-endian byte cursor.
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

// During parsing `sister` holds the paired event's pool INDEX, not a pointer —
// a pointer would dangle when push_back reallocates the pool. A final pass
// (once the pool is frozen) converts every index to a real pointer.
inline PlayEvent* idxAsSister(uint32_t idx) {
    return reinterpret_cast<PlayEvent*>(static_cast<uintptr_t>(idx));
}

// Append a note-on PlayEvent at `tick`; return its pool index. Its sister is
// filled in when the matching note-off arrives. Mirrors PFA's MIDIChannelEvent
// field for field (note.h): eventType, eventCode, track, deltaTicks, absTicks,
// absMicroSec, channelEventType, inputQuality, channel, param1, param2,
// sister, simultaneous, label. absMicroSec holds a TICK until conversion.
uint32_t pushNoteOn(std::vector<PlayEvent>& pool, uint32_t tick,
                    int key, int vel, int track, int channel) {
    int      c   = channel & 0x0F;
    uint8_t  k   = static_cast<uint8_t>(key & 0x7F);
    uint32_t idx = static_cast<uint32_t>(pool.size());
    pool.push_back(PlayEvent{
        0, 0x90 | c, track, 0, static_cast<int32_t>(tick),
        static_cast<int64_t>(tick), kNoteOn, 0,
        static_cast<uint8_t>(c), k, static_cast<uint8_t>(vel & 0x7F),
        nullptr, 0, nullptr });
    return idx;
}

// Append a note-off PlayEvent at `tick` closing the note-on at `onIdx`, and
// cross-link the pair via `sister` (still as indices — see idxAsSister).
void pushNoteOff(std::vector<PlayEvent>& pool, uint32_t tick, uint32_t onIdx) {
    int     c     = pool[onIdx].channel;   // read before push_back may realloc
    int     track = pool[onIdx].track;
    uint8_t k     = pool[onIdx].param1;
    uint32_t offIdx = static_cast<uint32_t>(pool.size());
    pool.push_back(PlayEvent{
        0, 0x80 | c, track, 0, static_cast<int32_t>(tick),
        static_cast<int64_t>(tick), kNoteOff, 0,
        static_cast<uint8_t>(c), k, 0,
        idxAsSister(onIdx), 0, nullptr });
    pool[onIdx].sister = idxAsSister(offIdx);
}

// Append a singleton PlayEvent (CC, ProgramChange, etc.) at `tick`.
void pushChannelEvent(std::vector<PlayEvent>& pool, uint32_t tick,
                      uint8_t status, uint8_t p1, uint8_t p2, int track) {
    int c = status & 0x0F;
    int type = status >> 4;
    pool.push_back(PlayEvent{
        0, status, track, 0, static_cast<int32_t>(tick),
        static_cast<int64_t>(tick), type, 0,
        static_cast<uint8_t>(c), p1, p2,
        nullptr, 0, nullptr });
}

}  // namespace

MidiData parseMidi(const std::string& path, std::atomic<float>& progress,
                   uint64_t expectedEvents) {
    MidiData out;
    progress = 0.0f;

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) { LOGE("parseMidi: cannot open %s", path.c_str()); return out; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 14) {
        close(fd); LOGE("parseMidi: file too small"); return out;
    }
    size_t fileSize = static_cast<size_t>(st.st_size);
    void* map = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { LOGE("parseMidi: mmap failed"); return out; }
    const uint8_t* base    = static_cast<const uint8_t*>(map);
    const uint8_t* fileEnd = base + fileSize;

    // ---- header ----
    Reader hdr{ base, fileEnd };
    if (hdr.u8() != 'M' || hdr.u8() != 'T' || hdr.u8() != 'h' || hdr.u8() != 'd') {
        munmap(map, fileSize); LOGE("parseMidi: not a MIDI file"); return out;
    }
    uint32_t hdrLen   = hdr.u32();
    hdr.u16();                                   // format (unused)
    uint32_t numTracks = hdr.u16();
    int16_t  division  = static_cast<int16_t>(hdr.u16());
    if (hdrLen > 6) hdr.skip(hdrLen - 6);
    int ticksPerQuarter = division > 0 ? division : 480;  // SMPTE -> fallback
    progress = 0.05f;

    // ---- tracks ----
    // Size the pool. `fileSize / 2` assumes two bytes per event; a dense black
    // MIDI runs closer to four, so that heuristic asks for about TWICE what the
    // file holds -- 757 MB for a 378 MB pool on Press G, which is a 32-bit
    // std::bad_alloc and a load refused as "too big for RAM" on a phone that
    // streams four times the events happily. When the caller has already
    // skimmed the file, use that count: predictEventCount is exact (measured
    // 3,388,104 predicted vs 3,388,104 parsed on System 32), so a 1/32 margin
    // is ample. Under-reserving is only slow, never wrong -- `pending[]` holds
    // pool INDICES, not pointers, so a reallocation cannot invalidate anything.
    out.eventPool.reserve(expectedEvents ? expectedEvents + expectedEvents / 32 + 4096
                                         : fileSize / 2 + 4096);
    std::vector<TempoEvent> tempos;
    // pool index of each unpaired note-on, per channel/key
    static thread_local std::vector<uint32_t> pending[16][128];

    const uint8_t* cur = hdr.p;
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
                if (vel > 0) {              // emit the note-on event now
                    pending[ch][key].push_back(
                        pushNoteOn(out.eventPool, absTick, key, vel, t, ch));
                    out.actualNoteCount++;
                } else if (!pending[ch][key].empty()) {   // vel 0 = note off
                    pushNoteOff(out.eventPool, absTick, pending[ch][key].back());
                    pending[ch][key].pop_back();
                }
            } else if (hi == 0x80) {        // note off
                uint8_t key = r.u8() & 0x7F;
                r.u8();                     // release velocity (ignored)
                if (!pending[ch][key].empty()) {
                    pushNoteOff(out.eventPool, absTick, pending[ch][key].back());
                    pending[ch][key].pop_back();
                }
            } else if (hi == 0xA0 || hi == 0xB0 || hi == 0xE0) {
                uint8_t p1 = r.u8(), p2 = r.u8(); // 2 data bytes
                pushChannelEvent(out.eventPool, absTick, status, p1, p2, t);
            } else if (hi == 0xC0 || hi == 0xD0) {
                uint8_t p1 = r.u8();              // 1 data byte
                pushChannelEvent(out.eventPool, absTick, status, p1, 0, t);
            } else if (status == 0xFF) {    // meta event
                uint8_t  metaType = r.u8();
                uint32_t len      = r.varlen();
                if (metaType == 0x51 && len == 3) {
                    uint8_t b0 = r.u8(), b1 = r.u8(), b2 = r.u8();
                    tempos.push_back({ absTick,
                        (uint32_t(b0) << 16) | (uint32_t(b1) << 8) | b2 });
                } else {
                    r.skip(len);
                }
            } else if (status == 0xF0 || status == 0xF7) {  // sysex
                r.skip(r.varlen());
            } else {
                break;                      // malformed
            }
        }

        // notes still held at track end: terminate them there
        for (int c = 0; c < 16; c++)
            for (int k = 0; k < 128; k++)
                for (uint32_t onIdx : pending[c][k])
                    pushNoteOff(out.eventPool, absTick, onIdx);

        maxTrack = static_cast<int>(t);
        cur = trkEnd;
        progress = 0.05f + 0.53f * float(t + 1) / float(numTracks ? numTracks : 1);
    }

    // ---- tempo map ----
    std::sort(tempos.begin(), tempos.end(),
              [](const TempoEvent& a, const TempoEvent& b) { return a.tick < b.tick; });
    std::vector<TempoSeg> segs;
    segs.push_back({ 0u, 0ull, 500000u });
    for (const TempoEvent& te : tempos) {
        TempoSeg& last = segs.back();
        if (te.tick <= last.tick) { last.usPerQuarter = te.usPerQuarter; continue; }
        uint64_t us = last.usAtTick +
            uint64_t(te.tick - last.tick) * last.usPerQuarter / ticksPerQuarter;
        segs.push_back({ te.tick, us, te.usPerQuarter });
    }
    auto tickToUs = [&](uint32_t tick) -> uint64_t {
        size_t lo = 0, hi2 = segs.size();
        while (lo + 1 < hi2) {
            size_t mid = (lo + hi2) / 2;
            if (segs[mid].tick <= tick) lo = mid; else hi2 = mid;
        }
        const TempoSeg& s = segs[lo];
        return s.usAtTick + uint64_t(tick - s.tick) * s.usPerQuarter / ticksPerQuarter;
    };

    // ---- link note-on <-> note-off sisters (the pool is now final) ----
    // `sister` currently holds the paired event's INDEX; convert each to a real
    // pointer now that no further push_back can move the pool — the same
    // in-place trick absMicroSec uses for its tick value.
    progress = 0.60f;
    for (size_t k = 0; k < out.eventPool.size(); k++) {
        uintptr_t sib = reinterpret_cast<uintptr_t>(out.eventPool[k].sister);
        out.eventPool[k].sister = &out.eventPool[sib];
    }

    // ---- ticks -> microseconds (in place over the pool) ----
    progress = 0.62f;
    uint64_t totalUs = 0;
    size_t   poolN   = out.eventPool.size();
    for (size_t i = 0; i < poolN; i++) {
        PlayEvent& e = out.eventPool[i];
        uint64_t us = tickToUs(static_cast<uint32_t>(e.absMicroSec));  // held a tick
        if (us > 0xFFFFFFFFull) us = 0xFFFFFFFFull;
        e.absMicroSec = static_cast<int64_t>(us);
        if (us > totalUs) totalUs = us;
        if ((i & 0xFFFFF) == 0)
            progress = 0.62f + 0.12f * float(i) / float(poolN ? poolN : 1);
    }

    // ---- time-sorted pointer table (the playback walk order) ----
    progress = 0.74f;
    out.events.resize(poolN);
    for (size_t i = 0; i < poolN; i++) out.events[i] = &out.eventPool[i];
    progress = 0.80f;
    // PFA Z-order: events are merged track-by-track with a min-heap that breaks ties
    // by lowest track index first. This means track 0 events come first in the sorted
    // list and are drawn first (underneath); the highest-numbered track is drawn last
    // (on top). Within the same track and timestamp, preserve original parse order
    // (pointer comparison works because all events are contiguous in eventPool).
    std::stable_sort(out.events.begin(), out.events.end(),
              [](const PlayEvent* a, const PlayEvent* b) {
                  if (a->absMicroSec != b->absMicroSec)
                      return a->absMicroSec < b->absMicroSec;
                  if (a->track != b->track)
                      return a->track < b->track;  // lower track = drawn first = underneath
                  return a->channelEventType > b->channelEventType;  // non-notes first, note-on, note-off last
              });

    // ---- program change and controller index ----
    progress = 0.90f;
    for (size_t i = 0; i < out.events.size(); i++) {
        if (out.events[i]->isProgramChange() || out.events[i]->isController() || out.events[i]->isPitchBend()) {
            out.programChangeIdx.push_back(i);
        }
    }

    // ---- track colours (PFA-faithful, GameState.cpp SetChannelSettings) ----
    // PFA walks note-bearing (track,channel) pairs in track-major order: the first
    // 16 take the fixed default palette, the rest get a fresh random colour. srand
    // is seeded from the clock so colours past channel 16 differ every load.
    progress = 0.95f;
    out.trackCount = maxTrack + 1;
    out.trackColors.assign(static_cast<size_t>(out.trackCount) * 16, 0xFFFFFFFFu);

    // Which (track,channel) pairs actually carry notes — only these get a slot.
    std::vector<uint8_t> hasNotes(out.trackColors.size(), 0);
    for (const PlayEvent& e : out.eventPool)
        if (e.isNoteOn())
            hasNotes[static_cast<size_t>(e.track) * 16 + e.channel] = 1;

    uint32_t palette[16];
    defaultPalettePFA(palette);
    srand(static_cast<unsigned>(time(nullptr)));   // PianoFromAbove.cpp:43

    int iPos = 0;
    for (int trk = 0; trk < out.trackCount; trk++)
        for (int chn = 0; chn < 16; chn++) {
            size_t idx = static_cast<size_t>(trk) * 16 + chn;
            if (!hasNotes[idx]) continue;
            out.trackColors[idx] = (iPos < 16) ? palette[iPos] : randColorPFA();
            iPos++;
        }

    // ---- note range (PFA's iMinNote/iMaxNote, expanded to at least A0-C8) ----
    {
        int mn = 127, mx = 0;
        for (const PlayEvent& e : out.eventPool)
            if (e.isNoteOn()) {
                if (e.param1 < mn) mn = e.param1;
                if (e.param1 > mx) mx = e.param1;
            }
        // PFA "All keys" mode: expand to at least A0(21)..C8(108)
        out.minNote = 0;
        out.maxNote = 127;
    }
    out.totalUs = static_cast<uint32_t>(std::min<uint64_t>(totalUs, 0xFFFFFFFFull));
    out.valid   = !out.eventPool.empty();
    progress    = 1.0f;

    munmap(map, fileSize);
    LOGI("parseMidi: %zu notes (%zu events), %d tracks, %.1f s, %.1f MB",
         out.noteCount(), out.eventPool.size(), out.trackCount,
         out.totalUs / 1e6, out.memoryBytes() / 1048576.0);
    return out;
}

}  // namespace apfa
