// midi_parser.h — Standard MIDI File -> PFA-faithful scattered event store.
#pragma once

#include "note.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace apfa {

// A view over the time-sorted pointer table. The ELEMENTS are unchanged —
// 8-byte PlayEvent* in exactly PFA's order, exactly as PFA's
// vector<MIDIEvent*> holds them (MIDI.h:174) — and so are the addresses the
// engine dereferences through them. Only WHERE THE ARRAY ITSELF LIVES varies:
// the in-RAM parse keeps it in eventsOwned below, while a streaming load points
// it at a file mapping so 8 B/event of anonymous memory (3.4 GB on a 448M-event
// MIDI) becomes clean, evictable page cache instead of swap's problem.
//
// This is residency, not layout. Nothing here may change the element type or
// the order — that table is PFA's, and shrinking it would make aPFA faster than
// the thing it exists to measure (note.h).
struct EventSpan {
    PlayEvent** p = nullptr;
    size_t      n = 0;

    size_t size()  const { return n; }
    bool   empty() const { return n == 0; }
    PlayEvent**       data()        { return p; }
    PlayEvent* const* data()  const { return p; }
    PlayEvent**       begin()       { return p; }
    PlayEvent**       end()         { return p + n; }
    PlayEvent* const* begin() const { return p; }
    PlayEvent* const* end()   const { return p + n; }
    PlayEvent*&       operator[](size_t i)       { return p[i]; }
    PlayEvent* const& operator[](size_t i) const { return p[i]; }
};

struct MidiData {
    // eventPool holds every PlayEvent in PARSE order (track by track) — this is
    // the scatter source. events[] points into it, sorted by time; walking it in
    // time order pointer-chases all over the pool (note.h, PFA-ANALYSIS.md §7).
    std::vector<PlayEvent>  eventPool;
    // The in-RAM parse owns its pointer table here; a streaming load leaves this
    // empty and points `events` at a file mapping it owns instead.
    std::vector<PlayEvent*> eventsOwned;
    EventSpan               events;       // pointers into eventPool, sorted by timeUs
    std::vector<uint32_t>   trackColors;  // indexed by track*16 + channel
    // Indices into events[] for ProgramChange and Controller events — mirrors
    // PFA's m_vProgramChange. Used to restore instrument/CC state on seek.
    std::vector<size_t>     programChangeIdx;
    uint32_t totalUs = 0;                 // end time of the last note
    int      trackCount = 0;
    int      minNote = 21;                // A0 default (PFA's MIDI::A0)
    int      maxNote = 108;              // C8 default (PFA's MIDI::C8)
    size_t   actualNoteCount = 0;         // real note count (note-on events only)
    bool     valid = false;

    size_t noteCount() const { return actualNoteCount; }
    size_t memoryBytes() const {   // size(), not capacity() — real committed RAM
        return eventPool.size() * sizeof(PlayEvent) +
               // eventsOwned, not events: a streamed table is a file mapping,
               // not process RAM, and is accounted with the pool instead.
               eventsOwned.size() * sizeof(PlayEvent*) +
               trackColors.size() * sizeof(uint32_t) +
               programChangeIdx.size() * sizeof(size_t);
    }

    // events[] points into eventPool, so MidiData is move-only: a move keeps the
    // pool's heap buffer in place (pointers stay valid); a copy would dangle them.
    MidiData() = default;
    MidiData(MidiData&&) = default;
    MidiData& operator=(MidiData&&) = default;
    MidiData(const MidiData&) = delete;
    MidiData& operator=(const MidiData&) = delete;
};

// Parse a .mid file at `path`. `progress` (0..1) is updated as it runs.
// `expectedEvents`, when non-zero, is the exact event count from a prior skim
// (Streamer::predictEventCount) and is used to size the pool. Zero keeps the
// old file-size heuristic, which is what a build without the streamer (iOS)
// gets. The parameter is defaulted so every existing 2-argument call site --
// the iOS engine included -- still compiles unchanged.
MidiData parseMidi(const std::string& path, std::atomic<float>& progress,
                   uint64_t expectedEvents = 0);

}  // namespace apfa
