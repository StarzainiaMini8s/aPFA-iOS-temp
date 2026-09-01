// native-lib.cpp — JNI surface for com.apfa.PlaybackActivity.
//
// Threading: nativeLoad runs on PlaybackActivity's background (loading) thread;
// nativeGetLoadProgress may be polled concurrently from the UI thread. Every
// other entry point is called only from the UI thread, sequentially. The
// g_engine pointer is therefore the only cross-thread shared state — atomic.
#include <jni.h>
#include <atomic>
#include <string>
#include <vector>

#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "engine.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "aPFA", __VA_ARGS__)

namespace {

std::atomic<apfa::Engine*> g_engine{nullptr};
ANativeWindow*             g_window = nullptr;   // UI-thread only
// Engine::loadError() of the last nativeLoad, stashed here because a failed
// load's engine is deleted before the UI thread can ask why it failed.
std::atomic<int>           g_lastLoadError{0};

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_apfa_PlaybackActivity_nativeLoad(JNIEnv* env, jobject, jstring midiPath,
                                          jstring sfPath, jint voiceCount,
                                          jfloat noteSpeed, jlong cpuMask,
                                          jboolean legacyRenderer,
                                          jboolean allowChunked,
                                          jstring poolDir) {
    apfa::Engine* old = g_engine.exchange(nullptr);
    if (old) { old->stop(); delete old; }

    auto* e = new apfa::Engine();
    g_engine.store(e);                       // visible so progress can be polled
    bool ok = e->load(jstr(env, midiPath), jstr(env, sfPath),
                       voiceCount, noteSpeed,
                       static_cast<uint64_t>(cpuMask),
                       legacyRenderer == JNI_TRUE,
                       allowChunked == JNI_TRUE,
                       jstr(env, poolDir));
    g_lastLoadError.store(e->loadError());
    if (!ok) {
        g_engine.store(nullptr);
        delete e;
        LOGI("nativeLoad failed");
    }
    return ok ? JNI_TRUE : JNI_FALSE;
}

// Why the last nativeLoad returned false — Engine::LoadError (1 = needs
// Chunked Disk Streaming, 2 = not enough free storage, 3 = pagefile past the
// volume's file-size limit, 0 = generic).
JNIEXPORT jint JNICALL
Java_com_apfa_PlaybackActivity_nativeGetLoadError(JNIEnv*, jobject) {
    return g_lastLoadError.load();
}

JNIEXPORT jfloat JNICALL
Java_com_apfa_PlaybackActivity_nativeGetLoadProgress(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->loadProgress().load() : 0.0f;
}

JNIEXPORT jlong JNICALL
Java_com_apfa_PlaybackActivity_nativeGetNoteCount(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->noteCount() : 0;
}

JNIEXPORT jlong JNICALL
Java_com_apfa_PlaybackActivity_nativeGetMemoryBytes(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->memoryBytes() : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_apfa_PlaybackActivity_nativeGetStreamedBytes(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->streamedBytes() : 0;
}

JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeStart(JNIEnv* env, jobject, jobject surface) {
    apfa::Engine* e = g_engine.load();
    if (!e || !surface) return;
    g_window = ANativeWindow_fromSurface(env, surface);
    e->start(g_window);
}

// Surface teardown only (e.g. app sent to recents / task view). Stops the render
// thread and releases the window, but KEEPS the engine and its parsed MIDI so the
// next nativeStart re-attaches to the new surface and resumes in place. Deleting
// the engine here is what used to unload the MIDI and leave a black screen.
JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeStop(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    if (e) e->stop();
    if (g_window) { ANativeWindow_release(g_window); g_window = nullptr; }
}

// Full teardown — the activity is actually finishing (onDestroy). Now it's safe
// to drop the engine and free the MIDI.
JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeRelease(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.exchange(nullptr);
    if (e) { e->stop(); delete e; }
    if (g_window) { ANativeWindow_release(g_window); g_window = nullptr; }
}

JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeSurfaceChanged(JNIEnv*, jobject,
                                                    jint w, jint h) {
    apfa::Engine* e = g_engine.load();
    if (e) e->surfaceChanged(w, h);
}

JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativePause(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    if (e) e->pause();
}

JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeResume(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    if (e) e->resume();
}

JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeSeek(JNIEnv*, jobject, jlong micros) {
    apfa::Engine* e = g_engine.load();
    if (e) e->seek(static_cast<int64_t>(micros));
}

JNIEXPORT jboolean JNICALL
Java_com_apfa_PlaybackActivity_nativeIsPlaying(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return (e && e->isPlaying()) ? JNI_TRUE : JNI_FALSE;
}

// 0 = none/still starting/ok; non-zero = Engine::StartError (1 synth, 2 renderer).
JNIEXPORT jint JNICALL
Java_com_apfa_PlaybackActivity_nativeGetStartError(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->startError() : 0;
}

JNIEXPORT jlong JNICALL
Java_com_apfa_PlaybackActivity_nativeGetTimeMicros(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->timeUs() : 0;
}

JNIEXPORT jlong JNICALL
Java_com_apfa_PlaybackActivity_nativeGetTotalMicros(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->totalUs() : 0;
}

// Seek-bar bounds = PFA's GetMinTime/GetMaxTime. The position slider spans
// [min, max], so the left edge lands in the -3s pre-roll, like stock PFA.
JNIEXPORT jlong JNICALL
Java_com_apfa_PlaybackActivity_nativeGetMinMicros(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->minTimeUs() : 0;
}

JNIEXPORT jlong JNICALL
Java_com_apfa_PlaybackActivity_nativeGetMaxMicros(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->maxTimeUs() : 0;
}

JNIEXPORT jfloat JNICALL
Java_com_apfa_PlaybackActivity_nativeGetFps(JNIEnv*, jobject) {
    apfa::Engine* e = g_engine.load();
    return e ? e->fps() : 0.0f;
}

JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeSetBgColor(JNIEnv*, jobject, jint bgrColor) {
    apfa::Engine* e = g_engine.load();
    if (e) e->setBgColor(static_cast<uint32_t>(bgrColor));
}

// pixels: Android ARGB_8888 ints (0xAARRGGBB), w*h of them. Converted to tightly
// packed RGBA bytes (row 0 = top) and handed to the engine for GL upload.
JNIEXPORT void JNICALL
Java_com_apfa_PlaybackActivity_nativeSetBgImage(JNIEnv* env, jobject,
                                                jintArray pixels, jint w, jint h) {
    apfa::Engine* e = g_engine.load();
    if (!e) return;
    if (!pixels || w <= 0 || h <= 0) { e->setBgImage(nullptr, 0, 0); return; }

    jsize n = env->GetArrayLength(pixels);
    if (n < static_cast<jsize>(w) * h) return;
    jint* src = env->GetIntArrayElements(pixels, nullptr);
    if (!src) return;

    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; i++) {
        uint32_t p = static_cast<uint32_t>(src[i]);   // 0xAARRGGBB
        rgba[i * 4 + 0] = (p >> 16) & 0xFF;           // R
        rgba[i * 4 + 1] = (p >>  8) & 0xFF;           // G
        rgba[i * 4 + 2] = (p >>  0) & 0xFF;           // B
        rgba[i * 4 + 3] = (p >> 24) & 0xFF;           // A
    }
    env->ReleaseIntArrayElements(pixels, src, JNI_ABORT);
    e->setBgImage(rgba.data(), w, h);
}

}  // extern "C"
