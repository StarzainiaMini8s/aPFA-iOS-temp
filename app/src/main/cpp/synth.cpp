// synth.cpp — see synth.h.
#include "synth.h"

#include <time.h>
#include <unistd.h>
#include "platform.h"

namespace apfa {

// Never thin below this many voices: past here the audio stops being a
// quieter version of the piece and starts being a different one. The point is
// that it keeps playing, not that it stays faithful at any cost.
static const int kVoiceFloor = 16;

static uint64_t nowUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull + ts.tv_nsec / 1000;
}

// --- limiter ----------------------------------------------------------------
static float g_limEnv    = 0.0f;
static bool  g_limActive = false;

static void CALLBACK limiterDSP(HDSP, DWORD, void* buffer, DWORD length, void*) {
    if (!g_limActive) { g_limActive = true; LOGI("limiter DSP running"); }
    float* p = static_cast<float*>(buffer);
    const size_t n = length / sizeof(float);
    const float kCeiling = 0.60f;
    const float kAttack  = 0.25f;
    const float kRelease = 0.0001f;
    float env = g_limEnv;
    for (size_t i = 0; i < n; i++) {
        float s = p[i];
        float a = s < 0.0f ? -s : s;
        env += (a > env ? kAttack : kRelease) * (a - env);
        if (env > kCeiling) s *= kCeiling / env;
        if      (s >  1.0f) s =  1.0f;
        else if (s < -1.0f) s = -1.0f;
        p[i] = s;
    }
    g_limEnv = env;
}

// ---------------------------------------------------------------------------

bool Synth::init(int voiceLimit, int sampleRate) {
    sampleRate_ = sampleRate;
    if (!BASS_Init(-1, sampleRate_, 0, nullptr, nullptr)) {
        LOGE("BASS_Init failed: %d", BASS_ErrorGetCode());
        return false;
    }
    BASS_SetConfig(BASS_CONFIG_MIDI_VOICES, 100000);
    BASS_SetConfig(BASS_CONFIG_BUFFER, 100);
    BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 10);

    midiStream_ = BASS_MIDI_StreamCreate(16, BASS_SAMPLE_FLOAT, sampleRate_);
    if (!midiStream_) {
        LOGE("BASS_MIDI_StreamCreate failed: %d", BASS_ErrorGetCode());
        return false;
    }
    setVoiceLimit(voiceLimit);
    voiceCeiling_.store(voiceLimit < 1 ? 1 : voiceLimit);
    voiceCurrent_.store(voiceLimit < 1 ? 1 : voiceLimit);
    g_limEnv = 0.0f;
    g_limActive = false;
    if (!BASS_ChannelSetDSP(midiStream_, &limiterDSP, nullptr, 0))
        LOGE("limiter DSP attach failed: %d", BASS_ErrorGetCode());

    // OmniMIDI's own overload setting (BASSSynth.cpp StreamSettings). Kept
    // because it is what the reference synth does and it costs nothing; the
    // guard thread below is what actually holds the line on this build. See
    // the comment in synth.h.
    BASS_ChannelSetAttribute(midiStream_, BASS_ATTRIB_MIDI_CPU, 95.0f);

    startGuard();

    ready_ = true;
    LOGI("Synth ready: BASSMIDI 0x%08X, %d Hz, voices=%d, raw batch path, "
         "limiter on, overload guard on",
         BASS_MIDI_GetVersion(), sampleRate_, voiceLimit);
    return true;
}

// --- audio overload guard ----------------------------------------------------

void Synth::startGuard() {
    if (guardRun_.load()) return;
    guardRun_.store(true);
    guardArmed_.store(false);
    // Created here on purpose: Engine::threadMain applies the "avoid the engine
    // core" affinity mask before calling init(), so this thread inherits it and
    // stays off the engine's core, exactly like BASS's own render thread.
    guardThread_ = std::thread(&Synth::guardMain, this);
}

void Synth::stopGuard() {
    if (!guardRun_.exchange(false)) return;
    if (guardThread_.joinable()) guardThread_.join();
}

void Synth::guardMain() {
    // 20 ms cadence: fast enough to react well before a 100 ms playback buffer
    // can empty, slow enough to be free (50 wakeups/s, two cheap BASS calls).
    const int kPeriodUs = 20000;
    while (guardRun_.load(std::memory_order_relaxed)) {
        HSTREAM h = midiStream_;
        if (h && BASS_ChannelIsActive(h) == BASS_ACTIVE_PLAYING) {
            DWORD avail = BASS_ChannelGetData(h, nullptr, BASS_DATA_AVAILABLE);
            if (avail != static_cast<DWORD>(-1)) {
                int bufMs = static_cast<int>(
                    BASS_ChannelBytes2Seconds(h, avail) * 1000.0);
                // Only start steering once the buffer has filled once, so the
                // initial fill (and the refill after a resume or a seek) is not
                // mistaken for an overload.
                if (!guardArmed_.load(std::memory_order_relaxed)) {
                    if (bufMs >= 80) guardArmed_.store(true);
                } else {
                    float cpu = 0.0f;
                    BASS_ChannelGetAttribute(h, BASS_ATTRIB_CPU, &cpu);
                    int cur  = voiceCurrent_.load(std::memory_order_relaxed);
                    int ceil = voiceCeiling_.load(std::memory_order_relaxed);
                    int want = cur;
                    // Cut fast, restore slowly — the reverse pumps audibly.
                    // The CPU term leads (it crosses 100% before the buffer has
                    // drained); the buffer term is the backstop.
                    if      (bufMs < 20)                 want = cur >> 1;
                    else if (bufMs < 50 || cpu > 92.0f)  want = cur - (cur >> 2);
                    else if (bufMs > 80 && cpu < 75.0f)  want = cur + (cur >> 4) + 1;
                    if (want < kVoiceFloor) want = kVoiceFloor;
                    if (want > ceil)        want = ceil;
                    if (want != cur) {
                        voiceCurrent_.store(want, std::memory_order_relaxed);
                        BASS_ChannelSetAttribute(h, BASS_ATTRIB_MIDI_VOICES,
                                                 static_cast<float>(want));
                    }
                    int prev = voiceFloorSeen_.load(std::memory_order_relaxed);
                    while (want < prev &&
                           !voiceFloorSeen_.compare_exchange_weak(prev, want)) {}
                }
            }
        }
        usleep(kPeriodUs);
    }
}

void Synth::sampleGuard(int& voiceCeiling, int& voiceFloorSeen) {
    voiceCeiling   = voiceCurrent_.load(std::memory_order_relaxed);
    int seen       = voiceFloorSeen_.exchange(1 << 30, std::memory_order_relaxed);
    voiceFloorSeen = (seen == (1 << 30)) ? voiceCeiling : seen;
}

bool Synth::loadSoundfont(const std::string& path) {
    if (!midiStream_) return false;
    font_ = BASS_MIDI_FontInit(path.c_str(), 0);
    if (!font_) {
        const int err = BASS_ErrorGetCode();
        // 7000 = BASS_ERROR_MIDI_INCLUDE: an SFZ named a file it could not open.
        // Worth saying out loud — an SFZ is only ever as portable as the sample
        // folder beside it, and the generic code makes that look like a bad font.
        if (err == 7000)
            LOGE("BASS_MIDI_FontInit: SFZ references a file it cannot open (%s)",
                 path.c_str());
        else
            LOGE("BASS_MIDI_FontInit failed: %d", err);
        return false;
    }
    BASS_MIDI_FONT f;
    f.font = font_; f.preset = -1; f.bank = 0;
    if (!BASS_MIDI_StreamSetFonts(midiStream_, &f, 1)) {
        LOGE("BASS_MIDI_StreamSetFonts failed: %d", BASS_ErrorGetCode());
        return false;
    }
    BASS_MIDI_StreamLoadSamples(midiStream_);
    LOGI("Soundfont loaded: %s", path.c_str());
    return true;
}

void Synth::setVoiceLimit(int voices) {
    if (voices < 1) voices = 1;
    voiceCeiling_.store(voices);
    voiceCurrent_.store(voices);
    if (midiStream_)
        BASS_ChannelSetAttribute(midiStream_, BASS_ATTRIB_MIDI_VOICES,
                                 static_cast<float>(voices));
}

void Synth::start(uint64_t) {
    if (midiStream_) BASS_ChannelPlay(midiStream_, FALSE);
}

// Pause releases the notes and leaves the stream RENDERING, exactly as PFA
// does: GameState.cpp:1128 ("If we just paused, kill the music") calls
// MIDIOutDevice::AllNotesOff(), and OmniMIDI — a live output device — keeps
// rendering right through it, so the release tails decay naturally in real
// time and the piece ends up genuinely silent while paused.
//
// This used to call BASS_ChannelPause(), which freezes the whole stream
// mid-sample. That froze the decay instead of letting it finish, and the
// frozen tail (plus whatever was still sitting in the playback buffer) was
// then played back on resume — audible as the previous note's release
// arriving after you had already seeked somewhere else.
void Synth::pause() {
    releaseAllNotes();
}

void Synth::resume() {
    // Nothing to restart: the stream was never stopped. Re-arm the overload
    // guard because a resume is a natural place for the buffer to be refilling.
    guardArmed_.store(false);
}

// PFA's MIDIOutDevice::AllNotesOff (MIDI.cpp:894-898) verbatim: All-notes-off
// (CC 123) then Sustain-off (CC 64) across all 16 channels. CC 123 releases the
// notes rather than cutting them, which is what makes the tails ring out.
void Synth::releaseAllNotes() {
    if (!midiStream_) return;
    for (int c = 0; c < 16; c++) {
        sendRaw(static_cast<uint8_t>(0xB0 | c), 123, 0);
        sendRaw(static_cast<uint8_t>(0xB0 | c), 64, 0);
    }
    flush();
}

void Synth::allNotesOff() {
    if (!midiStream_) return;
    releaseAllNotes();
    // Seek-only extra: reset Pitch Bend to center (8192) so seeking backward
    // past a pitch-bend doesn't leave the channel permanently bent. Deliberately
    // NOT done on pause — resuming has to keep the bend that was in force.
    for (int c = 0; c < 16; c++)
        sendRaw(static_cast<uint8_t>(0xE0 | c), 0, 64); // LSB=0, MSB=64 -> 8192
    flush();
}

// --- immediate raw call path ------------------------------------------------
// Fire BASS_MIDI_StreamEvents immediately per note using raw MIDI bytes,
// same timing as before but no struct overhead — BASSMIDI decodes raw bytes
// directly, same as OmniMIDI's SendDirectData path.

void Synth::sendRaw(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!midiStream_) return;
    uint8_t buf[3] = { status, d1, d2 };
    
    // Program Change (0xC0) and Channel Aftertouch (0xD0) are 2-byte messages.
    // BASS_MIDI_EVENTS_RAW reads sequentially; sending a 3rd byte (d2=0) would
    // be parsed as running status data (e.g. Program Change 0) and instantly undo
    // the instrument change!
    int len = ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 2 : 3;

    uint64_t t0 = nowUs();
    BASS_MIDI_StreamEvents(midiStream_, BASS_MIDI_EVENTS_RAW | BASS_MIDI_EVENTS_ASYNC, buf, len);
    evMicros_.fetch_add(nowUs() - t0, std::memory_order_relaxed);
    evCalls_.fetch_add(1, std::memory_order_relaxed);
}

void Synth::noteOn(int channel, int key, int velocity) {
    sendRaw(static_cast<uint8_t>(0x90 | (channel & 0x0F)),
            static_cast<uint8_t>(key & 0x7F),
            static_cast<uint8_t>(velocity & 0x7F));
}

void Synth::noteOff(int channel, int key) {
    sendRaw(static_cast<uint8_t>(0x80 | (channel & 0x0F)),
            static_cast<uint8_t>(key & 0x7F),
            0);
}

void Synth::flush() {
    // Nothing to flush — events fire immediately. Kept for call-site compat.
}

void Synth::sampleEventCost(uint64_t& calls, uint64_t& micros, uint64_t& bpMicros) {
    calls    = evCalls_.exchange(0,  std::memory_order_relaxed);
    micros   = evMicros_.exchange(0, std::memory_order_relaxed);
    bpMicros = 0;
}

void Synth::shutdown() {
    ready_ = false;
    stopGuard();   // must go first: guardMain dereferences midiStream_
    if (midiStream_) { BASS_StreamFree(midiStream_); midiStream_ = 0; }
    if (font_)       { BASS_MIDI_FontFree(font_);    font_ = 0; }
    BASS_Free();
}

}  // namespace apfa
