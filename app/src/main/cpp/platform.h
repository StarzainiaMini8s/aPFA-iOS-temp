// platform.h — cross-platform shims so the shared C++ engine builds on both
// Android (NDK) and iOS (Xcode), without touching the legit-run logic.
//
// Only platform *glue* lives here: logging and the engine/aux thread policy.
// On Android the behaviour is identical to the original inline definitions
// (same tag, same android log macros). On Apple the same call sites resolve to
// stdio logging and QoS-class thread hints (there is no per-core affinity on
// iOS — see APFA-DESIGN and the project notes on core pinning).
#pragma once

// ---- logging ---------------------------------------------------------------
#if defined(__ANDROID__)
  #include <android/log.h>
  #define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "aPFA", __VA_ARGS__)
  #define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "aPFA", __VA_ARGS__)
#else
  #include <cstdio>
  // All aPFA LOG call sites pass a string-literal format first, so the literal
  // concatenation below is well-formed. Shows up in the Xcode device console.
  #define LOGI(...) do { fprintf(stdout, "[aPFA] " __VA_ARGS__); fputc('\n', stdout); } while (0)
  #define LOGE(...) do { fprintf(stderr, "[aPFA] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#endif

// ---- thread policy ---------------------------------------------------------
// Android: pin/avoid via sched_setaffinity (handled inline in engine.cpp, which
// also needs the per-core frequency scan). Apple: bias onto the performance
// cluster via QoS — the strongest control iOS allows. No-op-equivalent on the
// A7 (iPhone 5S) which has a single homogeneous core type; meaningful on A10+.
#if defined(__APPLE__)
  #include <pthread/qos.h>

namespace apfa {
namespace platform {

// Call from inside the engine thread: request the performance cluster.
inline void setEngineThreadPolicy() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

// Call from inside an auxiliary (feeder / loader) thread: bias to efficiency
// cores so it does not contend with the engine for a performance core.
inline void setAuxThreadPolicy() {
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
}

}  // namespace platform
}  // namespace apfa
#endif  // __APPLE__
