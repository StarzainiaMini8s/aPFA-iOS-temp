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
  #include <sys/resource.h>   // setiopolicy_np
  #include <sys/sysctl.h>     // hw.memsize
  #include <mach/mach.h>      // task_info / phys_footprint
  #include <dlfcn.h>          // os_proc_available_memory, looked up at runtime
  #include <cstddef>
  #include <cstdint>

namespace apfa {
namespace platform {

// Call from inside the engine thread: request the performance cluster.
inline void setEngineThreadPolicy() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

// Call from inside the streaming pool's loader / slice-builder thread.
//
// The Android policy for these threads is "every core BUT the engine's", i.e.
// stay out of the engine's way. QOS_CLASS_UTILITY looks like the iOS analog and
// is NOT: Darwin ties I/O priority to QoS, and UTILITY carries a throttled disk
// policy (IOPOL_UTILITY). The loader exists to keep read-ahead in front of the
// playhead, so throttling its reads defeats the whole point — this thread must
// be below the engine in CPU priority yet unthrottled on disk. USER_INITIATED
// plus an explicit IOPOL_IMPORTANT is that combination.
//
// This mattered only once the streaming pool was actually compiled for iOS;
// before that these call sites were dead code (streamer.cpp was excluded).
inline void setLoaderThreadPolicy() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_IMPORTANT);
}

// ---- memory budget ---------------------------------------------------------
// Total physical RAM. sysctl rather than sysconf(_SC_PHYS_PAGES) so the number
// does not depend on a Darwin sysconf extension.
inline uint64_t totalRamBytes() {
    uint64_t v = 0;
    size_t   len = sizeof(v);
    if (sysctlbyname("hw.memsize", &v, &len, nullptr, 0) != 0) return 0;
    return v;
}

// What this PROCESS may still allocate before jetsam kills it — not what the
// machine has free. iOS applies a per-process footprint limit well under
// physical RAM (and invisible from it), so every "will this fit" question on
// this platform has to be asked against this number.
//
// Clean, read-only, file-backed pages — which is all the streaming pool's
// mapping ever produces — do not count toward that footprint, which is exactly
// why the pool works here at all.
inline uint64_t availableMemoryBytes() {
    // os_proc_available_memory() is iOS 13+ and aPFA's floor is 12.0, so it is
    // resolved at runtime rather than linked. dlsym rather than a weak_import
    // declaration on purpose: a weak_import only takes effect if OUR
    // declaration is the first one the translation unit sees, and <os/proc.h>
    // can arrive ahead of it through any UIKit module import — at which point
    // the reference silently becomes strong and the app fails to launch on the
    // one OS version the whole dance exists for. Resolved once; the loader asks
    // for this at 10 Hz.
    using AvailFn = size_t (*)(void);
    static const AvailFn avail =
        reinterpret_cast<AvailFn>(dlsym(RTLD_DEFAULT, "os_proc_available_memory"));
    if (avail != nullptr) {
        size_t v = avail();
        if (v != 0) return static_cast<uint64_t>(v);
    }
    // iOS 12, or a process with no limit applied. Estimate: jetsam's foreground
    // limit sits near half of RAM across every generation with a published
    // figure (1 GB/645 MB, 2 GB/1.2 GB, 4 GB/2.1 GB, 8 GB/3.7 GB), so take 45%
    // and subtract what is already resident. Deliberately an UNDER-estimate:
    // too small only makes a load stream when it might have fitted, while too
    // large hands the in-RAM parse a budget it will be killed for using.
    const uint64_t limit = totalRamBytes() / 100 * 45;
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    uint64_t used = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        used = static_cast<uint64_t>(info.phys_footprint);
    return limit > used ? limit - used : 0;
}

}  // namespace platform
}  // namespace apfa
#endif  // __APPLE__
