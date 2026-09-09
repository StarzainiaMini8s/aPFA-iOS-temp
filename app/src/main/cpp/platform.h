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
#elif defined(__APPLE__)
  // stdout/stderr are a dead end on a SIDELOADED iOS build: reading them needs
  // a Mac with Xcode or Console.app attached over USB, and the people this port
  // is built for do not have one. A load that refuses therefore has to leave
  // its reason somewhere the phone itself can show — so every line also goes to
  // a plain text file in the app's Documents folder, which UIFileSharingEnabled
  // publishes as "On My iPhone -> aPFA -> apfa-log.txt", and the last error line
  // is kept in memory for the failure alert to quote (see logLastError below).
  // Definitions are in the __APPLE__ block at the bottom of this header.
  #define LOGI(...) ::apfa::platform::logPrint('I', __VA_ARGS__)
  #define LOGE(...) ::apfa::platform::logPrint('E', __VA_ARGS__)
#else
  #include <cstdio>
  // All aPFA LOG call sites pass a string-literal format first, so the literal
  // concatenation below is well-formed.
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
  #include <cstdarg>
  #include <cstddef>
  #include <cstdint>
  #include <cstdio>
  #include <cstring>
  #include <ctime>
  #include <mutex>
  #include <os/log.h>       // device syslog; reachable from Windows over USB
  #include <string>

namespace apfa {
namespace platform {

// ---- logging (see the LOGI/LOGE macros at the top of this header) -----------
//
// Everything here is header-only `inline` with function-local statics on
// purpose: the alternative is a new .cpp, which would have to be added to
// ios/project.yml, ios/Makefile AND the CI workflow's source list in lockstep,
// and this is not worth three places that can drift.

// Size cap for ONE run's file (the previous run is kept whole, see logSetPath).
// On overflow we stop appending rather than wrapping, because the interesting
// part of any aPFA log is the load at the top — the 2 Hz "perf:" lines that
// follow are what makes it big, and losing the load to keep those would be
// exactly backwards.
constexpr long   kLogMaxBytes    = 1L << 20;   // 1 MB
constexpr size_t kLogLastErrMax  = 512;

inline std::mutex& logMutex()   { static std::mutex m; return m; }
inline std::string& logPath()   { static std::string p; return p; }
inline char* logLastErrBuf()    { static char b[kLogLastErrMax] = {0}; return b; }
inline bool& logOverflowed()    { static bool o = false; return o; }

inline FILE*& logStream() { static FILE* f = nullptr; return f; }

// Called once at launch from the app delegate, with a path inside Documents.
//
// One log per run, but the PREVIOUS run is renamed aside rather than
// overwritten, and that is the whole point of the rename. The load worth
// reading is usually the one that ended in a jetsam kill: no alert, no error
// code, the process simply gone. Every line is flushed as it is written so that
// log survives the kill — and then the completely natural next move is to open
// the app again and try, which with a plain "w" would truncate the one file
// that had the answer in it. Whoever reads this wants -prev far more often than
// they want the run in which they went looking.
inline void logSetPath(const char* path) {
    std::lock_guard<std::mutex> lk(logMutex());
    if (logStream() != nullptr) { fclose(logStream()); logStream() = nullptr; }
    logOverflowed() = false;
    logPath() = path ? path : "";
    if (logPath().empty()) return;

    const size_t dot = logPath().find_last_of('.');
    const std::string prev =
        (dot == std::string::npos ? logPath() : logPath().substr(0, dot)) +
        "-prev.txt";
    rename(logPath().c_str(), prev.c_str());   // silently no-ops on the first run

    logStream() = fopen(logPath().c_str(), "w");
    if (logStream() == nullptr) return;
    // Date the file: the reader is matching it against "the time I pressed
    // Play", possibly days later and over a chat app.
    char when[64] = "";
    time_t    t = time(nullptr);
    struct tm tmv;
    if (localtime_r(&t, &tmv) != nullptr)
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(logStream(), "==== aPFA launched %s ====\n", when);
    fflush(logStream());
}

// The most recent LOGE line, "" if there has not been one. The failure alert
// quotes this: on a platform with no reachable console, a load that refuses for
// a reason the UI has no code for must still be able to say what happened.
inline const char* logLastError() { return logLastErrBuf(); }

// Call at the top of a load. Without it the failure alert can quote an error
// from a PREVIOUS attempt — the file stays, but "the last thing that went
// wrong" has to mean "in this load" for the alert to be worth showing.
inline void logClearLastError() {
    std::lock_guard<std::mutex> lk(logMutex());
    logLastErrBuf()[0] = '\0';
}

__attribute__((format(printf, 2, 3)))
inline void logPrint(char level, const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    // stdout/stderr reach a debugger and NOTHING else: an app launched normally
    // on iOS — which is every sideloaded launch — has them pointed at /dev/null,
    // so this line is for an Xcode session and no other reader.
    fprintf(level == 'E' ? stderr : stdout, "[aPFA] %s\n", line);

    // The device syslog is the reader that matters, because it is the one a
    // WINDOWS machine can reach: 3uTools' real-time log, or idevicesyslog from
    // libimobiledevice, both pull this over USB with no Mac anywhere. os_log is
    // iOS 10 and aPFA's floor is 12.0, so this links strongly and needs none of
    // the dlsym care that os_proc_available_memory does below. %{public}s is
    // load-bearing: without the annotation the unified log redacts every
    // dynamic string to <private> and the reader gets empty lines.
    if (level == 'E')
        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_ERROR, "[aPFA] %{public}s", line);
    else
        os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "[aPFA] %{public}s", line);

    std::lock_guard<std::mutex> lk(logMutex());
    if (level == 'E') {
        std::strncpy(logLastErrBuf(), line, kLogLastErrMax - 1);
        logLastErrBuf()[kLogLastErrMax - 1] = '\0';
    }
    FILE* f = logStream();
    if (f == nullptr || logOverflowed()) return;

    char stamp[16] = "--:--:--";
    time_t     t  = time(nullptr);
    struct tm  tmv;
    if (localtime_r(&t, &tmv) != nullptr)
        strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);
    fprintf(f, "%s %c %s\n", stamp, level, line);
    fflush(f);

    if (ftell(f) > kLogMaxBytes) {
        fprintf(f, "%s I --- log full (%ld bytes); nothing further is recorded, "
                   "the load above is what matters ---\n", stamp, kLogMaxBytes);
        fflush(f);
        logOverflowed() = true;
    }
}

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
