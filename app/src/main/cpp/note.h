// note.h — the playback event representation: a 1:1 mirror of PFA's event class.
//
// PFA's slowdown is structural. Its channel events are MIDIChannelEvent objects
// (MIDI.h:210) — a polymorphic class deriving MIDIEvent — heap-allocated per
// track in parse order, but the dispatch loop and the O(P) note-off scan walk
// them in TIME order, so every step pointer-chases scattered memory and thrashes
// L2/L3 (PFA-ANALYSIS.md §4, §6, §7).
//
// PlayEvent below mirrors MIDIEvent + MIDIChannelEvent FIELD FOR FIELD. PFA
// genuinely carries every one of these per channel event (many barely used —
// that is PFA's real, wasteful layout, not padding we invented). Replicating it
// gives aPFA the same per-event size (~72 B vs PFA's 80 — the 8 B difference is
// PFA's vtable pointer, which a plain struct genuinely does not have) and so the
// same memory footprint and the same cache working-set. One note = two events.
//
//   eventPool : every PlayEvent, laid down in PARSE order (track by track).
//   events[]  : pointers into eventPool, sorted by TIME — the playback walk.
//
// Walking events[] in time order jumps all over eventPool: genuine, emergent
// cache pressure, exactly PFA's.
#pragma once

#include <cstdint>
#include <cstdlib>   // rand / srand (PFA Util::RandColor)
#include <utility>   // std::swap (PFA palette swap)

namespace apfa {

// PFA MIDIChannelEvent::ChannelEventType (MIDI.h:215).
// Full set: mirrors every channel event type PFA handles.
enum ChannelEventType {
    kNoteOff          = 0x8,
    kNoteOn           = 0x9,
    kNoteAftertouch   = 0xA,
    kController       = 0xB,
    kProgramChange    = 0xC,
    kChannelAftertouch= 0xD,
    kPitchBend        = 0xE
};

struct PlayEvent {
    // --- mirrors MIDIEvent (MIDI.h:180) ---
    int32_t  eventType;        // m_eEventType    — ChannelEvent for every note
    int32_t  eventCode;        // m_iEventCode    — raw MIDI status byte
    int32_t  track;            // m_iTrack
    int32_t  deltaTicks;       // m_iDT
    int32_t  absTicks;         // m_iAbsT
    int64_t  absMicroSec;      // m_llAbsMicroSec — absolute event time
    // --- mirrors MIDIChannelEvent (MIDI.h:210) ---
    int32_t  channelEventType; // m_eChannelEventType — kNoteOn / kNoteOff / etc.
    int32_t  inputQuality;     // m_eInputQuality
    uint8_t  channel;          // m_cChannel
    uint8_t  param1;           // m_cParam1 — key (0-127) or CC# or program#
    uint8_t  param2;           // m_cParam2 — velocity / CC value / pitch MSB
    PlayEvent* sister;         // m_pSister — note-on <-> note-off pairing (nullptr for non-note events)
    int32_t  simultaneous;     // m_iSimultaneous
    void*    label;            // m_sLabel

    bool isNoteOn()       const { return channelEventType == kNoteOn; }
    bool isNoteOff()      const { return channelEventType == kNoteOff; }
    bool isController()   const { return channelEventType == kController; }
    bool isProgramChange()const { return channelEventType == kProgramChange; }
    bool isPitchBend()    const { return channelEventType == kPitchBend; }
    // True for any event that is NOT a note-on or note-off (CC, ProgramChange, PitchBend, etc.)
    bool isNonNote()      const { return channelEventType != kNoteOn && channelEventType != kNoteOff; }
};

// HSV (h,s,v in 0..1) -> packed 0xAABBGGRR (memory order R,G,B,A for a GL
// 4x UNSIGNED_BYTE normalized vertex attribute).
inline uint32_t packHSV(float h, float s, float v) {
    float r = 0, g = 0, b = 0;
    float i = h * 6.0f;
    int   seg = static_cast<int>(i) % 6;
    float f = i - static_cast<int>(i);
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (seg) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    uint32_t R = static_cast<uint32_t>(r * 255.0f + 0.5f);
    uint32_t G = static_cast<uint32_t>(g * 255.0f + 0.5f);
    uint32_t B = static_cast<uint32_t>(b * 255.0f + 0.5f);
    return R | (G << 8) | (B << 16) | (0xFFu << 24);
}

// --- PFA-faithful track colours (Misc.cpp Util::HSVtoRGB / RandColor, ---------
// --- GameState.cpp ColorChannel, Config.cpp VisualSettings::LoadDefaultValues) -
//
// PFA assigns colours by walking note-bearing channels in order: the first 16 get
// the fixed default palette (the HSV spread built in LoadDefaultValues), and every
// channel after that gets a fresh random colour. srand() is seeded once from the
// clock (PianoFromAbove.cpp:43), so each load is "inherently random" past channel
// 16 — see [[apfa-synth-fidelity-reference]] for the fidelity ethos.
//
// PFA stores colours as Windows COLORREF 0x00BBGGRR; aPFA's GL vertex attribute
// wants memory-order R,G,B,A. The byte layout is identical (R=bit0, G=bit8,
// B=bit16) — we just force A=0xFF so the note geometry is opaque.

// PFA Util::HSVtoRGB (Misc.cpp:220). H in [0,360), S/V in [0,100].
inline void hsvToRgbPFA(int H, int S, int V, int& R, int& G, int& B) {
    double dH = H / 60.0, dS = S / 100.0, dV = V / 100.0;
    double C = dV * dS;
    double m = dV - C;
    double dR1 = 0.0, dG1 = 0.0, dB1 = 0.0;
    if      (dH >= 0 && dH < 1.0) { dR1 = C;                 dG1 = C * dH;          dB1 = 0; }
    else if (dH >= 1 && dH < 2.0) { dR1 = C * (2.0 - dH);    dG1 = C;               dB1 = 0; }
    else if (dH >= 2 && dH < 3.0) { dR1 = 0;                 dG1 = C;               dB1 = C * (dH - 2.0); }
    else if (dH >= 3 && dH < 4.0) { dR1 = 0;                 dG1 = C * (4.0 - dH);  dB1 = C; }
    else if (dH >= 4 && dH < 5.0) { dR1 = C * (dH - 4.0);    dG1 = 0;               dB1 = C; }
    else if (dH >= 5 && dH < 6.0) { dR1 = C;                 dG1 = 0;               dB1 = C * (6.0 - dH); }
    R = static_cast<int>((dR1 + m) * 255.0 + 0.5);
    G = static_cast<int>((dG1 + m) * 255.0 + 0.5);
    B = static_cast<int>((dB1 + m) * 255.0 + 0.5);
}

// Pack R,G,B (0..255) into the GL vertex colour layout (0xFFBBGGRR), opaque.
inline uint32_t packRGB(int R, int G, int B) {
    return (static_cast<uint32_t>(R) & 0xFF)
         | ((static_cast<uint32_t>(G) & 0xFF) << 8)
         | ((static_cast<uint32_t>(B) & 0xFF) << 16)
         | (0xFFu << 24);
}

// PFA Util::RandColor (Misc.cpp:193). Caller must have seeded srand().
inline uint32_t randColorPFA() {
    int R, G, B;
    hsvToRgbPFA(rand() % 360, rand() % 40 + 60, rand() % 20 + 80, R, G, B);
    return packRGB(R, G, B);
}

// PFA's 16-entry default palette (Config.cpp VisualSettings::LoadDefaultValues).
// Fills a 16-slot array the caller owns; first 16 note-bearing channels use these.
inline void defaultPalettePFA(uint32_t palette[16]) {
    const int iColors = 16, S = 80, V = 100;
    int R, G, B;
    for (int i = 10, count = 0; count < iColors; i = (i + 7) % iColors, count++) {
        hsvToRgbPFA(360 * i / iColors, S, V, R, G, B);
        palette[count] = packRGB(R, G, B);
    }
    std::swap(palette[2], palette[4]);   // PFA swaps slots 2 and 4
}

}  // namespace apfa
