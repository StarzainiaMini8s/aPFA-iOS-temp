// apfa_bridge.mm — see apfa_bridge.h. Structured deliberately to match
// native-lib.cpp function for function.
#include "apfa_bridge.h"

#include <atomic>
#include <string>

#include "engine.h"
#include "platform.h"   // LOGI / LOGE

namespace {

std::atomic<apfa::Engine*> g_engine{nullptr};
// Engine::loadError() of the last apfaLoad, stashed because a failed load's
// engine is deleted before the UI can ask why it failed.
std::atomic<int>           g_lastLoadError{0};

std::string cstr(const char* s) { return s ? std::string(s) : std::string(); }

}  // namespace

extern "C" {

bool apfaLoad(const char* midiPath, const char* sfPath,
              int voiceCount, float noteSpeed) {
    apfa::Engine* old = g_engine.exchange(nullptr);
    if (old) { old->stop(); delete old; }

    auto* e = new apfa::Engine();
    g_engine.store(e);                       // visible so progress can be polled
    // cpuMask 0 = "auto"; on iOS the engine takes the QoS path instead of any
    // affinity call, so the value is inert. legacyRenderer / allowChunked /
    // poolDir take their defaults — see engine.h, which defaults them precisely
    // so this call can stay this short.
    bool ok = e->load(cstr(midiPath), cstr(sfPath), voiceCount, noteSpeed, 0);
    g_lastLoadError.store(e->loadError());
    if (!ok) {
        g_engine.store(nullptr);
        delete e;
        LOGI("apfaLoad failed");
    }
    return ok;
}

int apfaGetLoadError(void) { return g_lastLoadError.load(); }

float apfaGetLoadProgress(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->loadProgress().load() : 0.0f;
}

int64_t apfaGetNoteCount(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->noteCount() : 0;
}

int64_t apfaGetMemoryBytes(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->memoryBytes() : 0;
}

void apfaStart(void* caeaglLayer) {
    apfa::Engine* e = g_engine.load();
    if (!e || !caeaglLayer) return;
    // Straight through: unlike ANativeWindow_fromSurface there is nothing to
    // convert or retain here — the CAEAGLLayer is owned by the playback view.
    e->start(caeaglLayer);
}

void apfaStop(void) {
    apfa::Engine* e = g_engine.load();
    if (e) e->stop();
}

void apfaRelease(void) {
    apfa::Engine* e = g_engine.exchange(nullptr);
    if (e) { e->stop(); delete e; }
}

void apfaSurfaceChanged(int w, int h) {
    apfa::Engine* e = g_engine.load();
    if (e) e->surfaceChanged(w, h);
}

void apfaPause(void) {
    apfa::Engine* e = g_engine.load();
    if (e) e->pause();
}

void apfaResume(void) {
    apfa::Engine* e = g_engine.load();
    if (e) e->resume();
}

void apfaSeek(int64_t micros) {
    apfa::Engine* e = g_engine.load();
    if (e) e->seek(micros);
}

bool apfaIsPlaying(void) {
    apfa::Engine* e = g_engine.load();
    return e && e->isPlaying();
}

int apfaGetStartError(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->startError() : 0;
}

int64_t apfaGetTimeMicros(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->timeUs() : 0;
}

int64_t apfaGetTotalMicros(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->totalUs() : 0;
}

int64_t apfaGetMinMicros(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->minTimeUs() : 0;
}

int64_t apfaGetMaxMicros(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->maxTimeUs() : 0;
}

float apfaGetFps(void) {
    apfa::Engine* e = g_engine.load();
    return e ? e->fps() : 0.0f;
}

void apfaSetBgColor(uint32_t bgrColor) {
    apfa::Engine* e = g_engine.load();
    if (e) e->setBgColor(bgrColor);
}

void apfaSetBgImage(const uint8_t* rgba, int w, int h) {
    apfa::Engine* e = g_engine.load();
    if (!e) return;
    // Already tightly packed RGBA here, where the JNI twin has to unpack
    // Android's ARGB_8888 ints first — CGBitmapContext gives us the engine's
    // layout directly (see APFASetupViewController's bgImageRGBA:).
    if (!rgba || w <= 0 || h <= 0) { e->setBgImage(nullptr, 0, 0); return; }
    e->setBgImage(rgba, w, h);
}

}  // extern "C"
