// Large allocations bypass malloc on a 32-bit process.
//
// The problem this exists to solve is address space, not memory. bionic's
// allocator never hands a freed span's ADDRESS SPACE back to the kernel: it
// returns the pages (madvise) and keeps the range mapped for reuse. On a
// 64-bit process that is free; on a 32-bit one it is the whole budget. An
// LG X410 that had played RST 14.9 once sat idle afterwards holding
//
//     [anon:libc_malloc]   686.0 MB mapped   3.3 MB resident
//
// — the load's five 111 MB tables, freed but not given back. The next load in
// that process then had 686 MB less to place them in, and the pool reservation
// and the working-set pricing both failed against an address space that was
// mostly holes: "not enough address space" on the second attempt at a MIDI
// that had just played. Killing the app fixed it, which is what made it look
// intermittent.
//
// So route anything at or above kBigBytes straight to mmap, where free really
// does mean unmapped. The threshold is well above the app's ordinary
// allocations and below every table in a streaming load (four bytes an event —
// 111 MB at RST 14.9's 29.3 M), so this is a policy about the giant tables
// only and nothing else changes shape.
//
// 64-bit builds compile to nothing: 256 TB does not run out by fragmentation,
// and the malloc path is faster. iOS never compiles this file (see
// CMakeLists.txt — it is Android-only).

#include <cstddef>
#include <cstdlib>
#include <new>

#if !defined(__LP64__)

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

// Big enough that ordinary allocations keep the malloc fast path, small enough
// that every per-event table takes the mmap path. The tables are 4 B/event, so
// this catches them from ~2 M events up.
constexpr size_t kBigBytes = 8u << 20;

// How many mmap'd blocks may be live at once. A streaming load holds six
// (events[], sisterPos_, inv[], poolIdx_, posUs_ and the sort table) plus the
// transient double during a vector's growth; 64 is room to spare. Past that we
// fall back to malloc, which is correct, just not as tidy.
constexpr size_t kMaxBig = 64;

struct BigBlock { void* p; size_t len; };

// Both are constant-initialised (BSS / a static mutex initialiser), so they are
// live before any dynamic initialiser can allocate.
BigBlock        g_big[kMaxBig];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
size_t          g_page = 0;

void* bigAlloc(size_t n) {
    size_t page = g_page;
    if (page == 0) {
        long v = sysconf(_SC_PAGESIZE);
        page = g_page = (v > 0) ? static_cast<size_t>(v) : 4096;
    }
    const size_t len = (n + page - 1) & ~(page - 1);
    if (len < n) return nullptr;                     // rounding overflowed
    void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    pthread_mutex_lock(&g_lock);
    for (BigBlock& b : g_big) {
        if (b.p == nullptr) {
            b.p = p; b.len = len;
            pthread_mutex_unlock(&g_lock);
            return p;
        }
    }
    pthread_mutex_unlock(&g_lock);
    munmap(p, len);                                  // table full — use malloc
    return nullptr;
}

// True if `p` was ours, in which case it is now unmapped. Only ever reached for
// page-aligned pointers, so the scan is off the ordinary free path entirely.
bool bigFree(void* p) {
    void*  va  = nullptr;
    size_t len = 0;
    pthread_mutex_lock(&g_lock);
    for (BigBlock& b : g_big) {
        if (b.p == p) { va = b.p; len = b.len; b.p = nullptr; b.len = 0; break; }
    }
    pthread_mutex_unlock(&g_lock);
    if (!va) return false;
    munmap(va, len);
    return true;
}

inline void* allocate(size_t n) {
    if (n >= kBigBytes) {
        if (void* p = bigAlloc(n)) return p;
    }
    return std::malloc(n ? n : 1);
}

inline void deallocate(void* p) {
    if (!p) return;
    const size_t page = g_page;
    if (page && (reinterpret_cast<uintptr_t>(p) & (page - 1)) == 0 && bigFree(p))
        return;
    std::free(p);
}

}  // namespace

// Every form has to be here. A sized or array delete left to the default
// implementation would reach free() with an mmap'd pointer.
void* operator new(size_t n)                                    { void* p = allocate(n); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](size_t n)                                  { void* p = allocate(n); if (!p) throw std::bad_alloc(); return p; }
void* operator new(size_t n, const std::nothrow_t&) noexcept    { return allocate(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept  { return allocate(n); }

void operator delete(void* p) noexcept                          { deallocate(p); }
void operator delete[](void* p) noexcept                        { deallocate(p); }
void operator delete(void* p, size_t) noexcept                  { deallocate(p); }
void operator delete[](void* p, size_t) noexcept                { deallocate(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { deallocate(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { deallocate(p); }

#endif  // !__LP64__
