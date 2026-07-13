// pthread_lock_shim.cpp — LD_PRELOAD / DYLD_INSERT_LIBRARIES interposer that
// transparently replaces pthread mutexes in unmodified binaries with any
// SoftwareMutex from this repo's benchmark suite (selected via
// MUTEX_SHIM_LOCK=<name>, same names as get_mutex()).
//
// Design (LiTL-style, see "Multicore Locks: The Case Is Not Closed Yet",
// ATC'16):
//
//  * No hash map on the fast path. When a mutex is claimed we overwrite the
//    first 16 bytes of the pthread_mutex_t with {64-bit magic, ShimMutex*}.
//    pthread_mutex_t is >= 40 bytes on glibc and 64 on macOS, so this always
//    fits. Once claimed, the real pthread_mutex_* functions are never called
//    on that address again, so clobbering the bytes is safe. Fast path is a
//    single atomic load + compare.
//
//  * Dense thread ids. SoftwareMutex::lock(tid) needs tids in
//    [0, MUTEX_SHIM_MAX_THREADS). Ids are handed out on a thread's first
//    shim acquisition and recycled at thread exit through a pthread_key
//    destructor, so thread-pool churn does not exhaust the pool. If more
//    threads are *concurrently* live than the pool holds, we abort loudly
//    (a too-large tid would corrupt per-thread lock arrays).
//
//  * Recursion emulation. The wrapper tracks owner + depth, so recursive and
//    errorcheck mutexes (including glibc's static recursive initializer)
//    work without peeking at libc internals, and accidental re-entry can't
//    self-deadlock a non-reentrant SoftwareMutex.
//
//  * Condition variables. A waiter parked in pthread_cond_wait would
//    otherwise sleep while still holding the custom lock => deadlock. We
//    hook cond_wait/timedwait: take a per-wrapper *shadow* real mutex,
//    release the custom lock, wait on the real condvar with the shadow, then
//    re-acquire the custom lock. signal/broadcast serialize on the shadow of
//    the condvar's last-associated mutex, which closes the lost-wakeup
//    window LiTL leaves open (disable with MUTEX_SHIM_COND_STRICT=0).
//
//  * Reentrancy guard. Everything the shim itself triggers (lock
//    constructors calling malloc, the `system`/`cpp_std` locks calling
//    pthread internally, ...) sees a per-thread guard and falls through to
//    the real implementation. Hook order is always:
//    claimed? -> shim; guarded/not-ready? -> real; else filter + claim.
//
//  * Scope filtering (MUTEX_SHIM_SCOPE):
//      all    - claim every claimable mutex (default; maximal coverage)
//      app    - claim only when the first-sight caller's return address lies
//               in the main executable's text (extend with
//               MUTEX_SHIM_TARGET_LIBS=substr,substr)
//      region - claim only mutexes whose *address* lies in a tracked mmap
//               region (MAP_SHARED by default, see MUTEX_SHIM_REGION_FLAGS)
//               or an mbind()-ed range on Linux (NUMA/CXL allocations).
//
// Environment:
//   MUTEX_SHIM_LOCK=<name>        lock to inject (required; unset = inert)
//   MUTEX_SHIM_MAX_THREADS=N      dense-tid pool / SoftwareMutex::init(N)
//   MUTEX_SHIM_SCOPE=all|app|region
//   MUTEX_SHIM_TARGET_LIBS=a,b    extra text ranges counted as "app"
//   MUTEX_SHIM_REGION_FLAGS=shared|anon|all
//   MUTEX_SHIM_STATS=1            dump counters at exit
//   MUTEX_SHIM_STATS_FILE=path    stats destination (default stderr)
//   MUTEX_SHIM_LOG=1              log each claim/ignore to stderr
//   MUTEX_SHIM_COND_STRICT=0      LiTL-level condvar handling (see above)
//   MUTEX_SHIM_STRICT=1           abort instead of degrading on shim errors
//
// Known limitations (documented in README): pthread_rwlock/barrier/spin pass
// through untouched; pthread_mutex_timedlock degrades to a blocking lock;
// trylock on a momentarily-free contended mutex may block briefly (the
// SoftwareMutex interface has no native trylock); cross-thread unlock of a
// claimed mutex returns EPERM; process-shared/robust/priority-protocol
// mutexes are never claimed; statically linked targets can't be intercepted.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <pthread.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#endif

#if defined(__linux__) && __has_include(<numaif.h>)
#include <numaif.h>
#define SHIM_HAVE_NUMAIF 1
#endif

#include "bench_utils.hpp" // get_mutex() + SoftwareMutex

// ───────────────────────── real-function dispatch ───────────────────────────
// macOS: dyld does not apply __interpose tuples to the interposing image, so
// direct calls from this file already reach libSystem.
// Linux: this .so *defines* the pthread symbols, so internal calls must go
// through dlsym(RTLD_NEXT) pointers.

#ifdef __APPLE__
#define CALL_REAL(name, ...) ::name(__VA_ARGS__)
#else
#define DECL_REAL(ret, name, ...)                                             \
    typedef ret (*fn_##name)(__VA_ARGS__);                                     \
    static fn_##name real_##name = nullptr;

DECL_REAL(int, pthread_mutex_init, pthread_mutex_t*, const pthread_mutexattr_t*)
DECL_REAL(int, pthread_mutex_destroy, pthread_mutex_t*)
DECL_REAL(int, pthread_mutex_lock, pthread_mutex_t*)
DECL_REAL(int, pthread_mutex_trylock, pthread_mutex_t*)
DECL_REAL(int, pthread_mutex_unlock, pthread_mutex_t*)
DECL_REAL(int, pthread_mutex_timedlock, pthread_mutex_t*, const struct timespec*)
DECL_REAL(int, pthread_cond_wait, pthread_cond_t*, pthread_mutex_t*)
DECL_REAL(int, pthread_cond_timedwait, pthread_cond_t*, pthread_mutex_t*, const struct timespec*)
DECL_REAL(int, pthread_cond_signal, pthread_cond_t*)
DECL_REAL(int, pthread_cond_broadcast, pthread_cond_t*)
DECL_REAL(int, pthread_cond_destroy, pthread_cond_t*)
DECL_REAL(void*, mmap, void*, size_t, int, int, int, off_t)
DECL_REAL(int, munmap, void*, size_t)

static std::atomic<bool> g_reals_ready{false};

static void ensure_reals() {
    if (g_reals_ready.load(std::memory_order_acquire)) return;
    // Idempotent; concurrent resolution writes identical values.
    real_pthread_mutex_init      = (fn_pthread_mutex_init)dlsym(RTLD_NEXT, "pthread_mutex_init");
    real_pthread_mutex_destroy   = (fn_pthread_mutex_destroy)dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    real_pthread_mutex_lock      = (fn_pthread_mutex_lock)dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_pthread_mutex_trylock   = (fn_pthread_mutex_trylock)dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_pthread_mutex_unlock    = (fn_pthread_mutex_unlock)dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_pthread_mutex_timedlock = (fn_pthread_mutex_timedlock)dlsym(RTLD_NEXT, "pthread_mutex_timedlock");
    real_pthread_cond_wait       = (fn_pthread_cond_wait)dlsym(RTLD_NEXT, "pthread_cond_wait");
    real_pthread_cond_timedwait  = (fn_pthread_cond_timedwait)dlsym(RTLD_NEXT, "pthread_cond_timedwait");
    real_pthread_cond_signal     = (fn_pthread_cond_signal)dlsym(RTLD_NEXT, "pthread_cond_signal");
    real_pthread_cond_broadcast  = (fn_pthread_cond_broadcast)dlsym(RTLD_NEXT, "pthread_cond_broadcast");
    real_pthread_cond_destroy    = (fn_pthread_cond_destroy)dlsym(RTLD_NEXT, "pthread_cond_destroy");
    real_mmap                    = (fn_mmap)dlsym(RTLD_NEXT, "mmap");
    real_munmap                  = (fn_munmap)dlsym(RTLD_NEXT, "munmap");
    g_reals_ready.store(true, std::memory_order_release);
}

#define CALL_REAL(name, ...) (ensure_reals(), real_##name(__VA_ARGS__))
#endif // !__APPLE__

namespace {

// ───────────────────────────── small utilities ──────────────────────────────

__attribute__((format(printf, 1, 2)))
void shim_msg(const char* fmt, ...) {
    // stderr via raw write(): the shim must not take libc FILE locks, which
    // may themselves be interposed pthread mutexes.
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n > sizeof buf) n = (int)sizeof buf;
    ssize_t r = write(2, buf, (size_t)n);
    (void)r;
}

inline void cpu_relax() {
#if defined(__aarch64__)
    __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#endif
}

struct ShimSpinLock {
    std::atomic<uint32_t> v{0};
    void lock() {
        while (v.exchange(1, std::memory_order_acquire) != 0) {
            while (v.load(std::memory_order_relaxed) != 0) cpu_relax();
        }
    }
    void unlock() { v.store(0, std::memory_order_release); }
};

// ───────────────────────────── configuration ────────────────────────────────

enum class Scope : uint8_t { All, App, Region };

struct Config {
    std::atomic<bool> ready{false};
    std::atomic<bool> claims_allowed{false};
    bool enabled       = false;
    bool stats         = false;
    bool log           = false;
    bool cond_strict   = true;
    bool strict        = false;
    bool fence         = true;
    Scope scope        = Scope::All;
    int  region_flags  = 0;        // 0=MAP_SHARED, 1=+anon, 2=all mmaps
    uint32_t max_threads = 128;
    char lock_name[64]   = {0};
    char stats_file[256] = {0};
    char target_libs[512] = {0};   // comma-separated substrings
};
Config g_cfg;

// ─────────────────────────── per-thread state (POD) ─────────────────────────
// POD only: no constructors/destructors, so it is safe to touch at any point
// in a thread's life, including during late TLS destruction.

constexpr int kTLIgnoreCache = 64;

struct TLState {
    uint32_t tid_plus1;                       // 0 = no dense id assigned yet
    uint32_t guard;                           // >0: inside shim internals
    const void* ignore_cache[kTLIgnoreCache]; // direct-mapped negative cache
};

thread_local TLState t_state
#if defined(__linux__)
    __attribute__((tls_model("initial-exec")))
#endif
    ;

// ───────────────────────────── stats counters ───────────────────────────────

struct alignas(64) ThreadSlot {
    uint64_t acquisitions;
    uint64_t cond_waits;
    uint64_t trylock_busy;
};

ThreadSlot* g_slots = nullptr;
std::atomic<uint64_t> g_claimed{0};
std::atomic<uint64_t> g_ignored{0};
std::atomic<uint64_t> g_passthrough{0};
std::atomic<uint64_t> g_eperm_unlocks{0};

// ───────────────────────────── dense thread ids ─────────────────────────────

ShimSpinLock g_tid_lock;
uint32_t* g_free_tids = nullptr;
uint32_t g_free_top = 0;
std::atomic<uint32_t> g_tids_created{0};
pthread_key_t g_tid_key;

void tid_key_dtor(void* p) {
    uint32_t tid = (uint32_t)(uintptr_t)p - 1;
    g_tid_lock.lock();
    g_free_tids[g_free_top++] = tid;
    g_tid_lock.unlock();
    t_state.tid_plus1 = 0;
}

__attribute__((noreturn))
void shim_fatal(const char* why) {
    shim_msg("mutex_shim: FATAL: %s\n", why);
    abort();
}

uint32_t acquire_tid_slow() {
    t_state.guard++;
    uint32_t tid = UINT32_MAX;
    g_tid_lock.lock();
    if (g_free_top > 0) tid = g_free_tids[--g_free_top];
    g_tid_lock.unlock();
    if (tid == UINT32_MAX) {
        uint32_t n = g_tids_created.fetch_add(1, std::memory_order_relaxed);
        if (n >= g_cfg.max_threads) {
            shim_msg("mutex_shim: more than MUTEX_SHIM_MAX_THREADS=%u threads "
                     "are concurrently using claimed mutexes.\n"
                     "mutex_shim: raise MUTEX_SHIM_MAX_THREADS (lock arrays are "
                     "sized by it) and re-run.\n", g_cfg.max_threads);
            shim_fatal("dense thread-id pool exhausted");
        }
        tid = n;
    }
    t_state.tid_plus1 = tid + 1;
    // Registers this thread for id recycling at exit. May allocate; guard is
    // held so any nested pthread calls pass through to the real functions.
    pthread_setspecific(g_tid_key, (void*)(uintptr_t)(tid + 1));
    t_state.guard--;
    return tid;
}

inline uint32_t my_tid() {
    uint32_t t = t_state.tid_plus1;
    return t != 0 ? t - 1 : acquire_tid_slow();
}

// ────────────────── fixed-size lock-free-read pointer table ─────────────────
// Open addressing, linear probing, tombstones. Inserts/erases serialize on a
// spinlock; find() is lock-free. Used for (a) the sticky ignore set
// (pshared/robust/PI mutexes, claim-probe failures) and (b) condvar -> shim
// association for strict signal handling. Never grows: on overflow the shim
// degrades safely (stops claiming / falls back to plain signal).

struct PtrTable {
    struct Entry {
        std::atomic<uintptr_t> key;
        std::atomic<void*> val;
    };
    static constexpr uintptr_t kTomb = ~(uintptr_t)0;

    Entry* e = nullptr;
    size_t mask = 0;
    size_t used = 0; // guarded by mu
    ShimSpinLock mu;

    void init(size_t cap_pow2) {
        e = (Entry*)calloc(cap_pow2, sizeof(Entry));
        mask = cap_pow2 - 1;
    }
    static size_t hash(const void* p) {
        uintptr_t x = (uintptr_t)p;
        x ^= x >> 12;
        x *= (uintptr_t)0x9E3779B97F4A7C15ull;
        x ^= x >> 29;
        return (size_t)x;
    }
    void* find(const void* k) const {
        if (e == nullptr) return nullptr;
        size_t i = hash(k) & mask;
        for (size_t n = 0; n <= mask; n++, i = (i + 1) & mask) {
            uintptr_t kk = e[i].key.load(std::memory_order_acquire);
            if (kk == 0) return nullptr;
            if (kk == (uintptr_t)k) return e[i].val.load(std::memory_order_acquire);
        }
        return nullptr;
    }
    bool insert(const void* k, void* v) {
        if (e == nullptr) return false;
        mu.lock();
        if (used * 4 >= (mask + 1) * 3) { mu.unlock(); return false; }
        size_t i = hash(k) & mask;
        size_t tomb = SIZE_MAX;
        for (size_t n = 0; n <= mask; n++, i = (i + 1) & mask) {
            uintptr_t kk = e[i].key.load(std::memory_order_relaxed);
            if (kk == (uintptr_t)k) {
                e[i].val.store(v, std::memory_order_release);
                mu.unlock();
                return true;
            }
            if (kk == kTomb) {
                if (tomb == SIZE_MAX) tomb = i;
                continue;
            }
            if (kk == 0) {
                size_t slot = (tomb != SIZE_MAX) ? tomb : i;
                e[slot].val.store(v, std::memory_order_release);
                e[slot].key.store((uintptr_t)k, std::memory_order_release);
                used++;
                mu.unlock();
                return true;
            }
        }
        mu.unlock();
        return false;
    }
    void erase(const void* k) {
        if (e == nullptr) return;
        mu.lock();
        size_t i = hash(k) & mask;
        for (size_t n = 0; n <= mask; n++, i = (i + 1) & mask) {
            uintptr_t kk = e[i].key.load(std::memory_order_relaxed);
            if (kk == 0) break;
            if (kk == (uintptr_t)k) {
                e[i].key.store(kTomb, std::memory_order_release);
                break;
            }
        }
        mu.unlock();
    }
};

PtrTable g_ignore_set; // mutex addr -> (void*)1
PtrTable g_cond_map;   // condvar addr -> ShimMutex*

inline bool tl_ignored(const void* m) {
    return t_state.ignore_cache[(PtrTable::hash(m) >> 8) & (kTLIgnoreCache - 1)] == m;
}
inline void tl_ignore_add(const void* m) {
    t_state.ignore_cache[(PtrTable::hash(m) >> 8) & (kTLIgnoreCache - 1)] = m;
}

// ─────────────────────────── app text-range filter ──────────────────────────

struct Range { uintptr_t lo, hi; };
constexpr int kMaxAppRanges = 64;
Range g_app_ranges[kMaxAppRanges];
int g_app_nranges = 0;

bool target_libs_match(const char* path) {
    if (g_cfg.target_libs[0] == 0 || path == nullptr) return false;
    char buf[sizeof g_cfg.target_libs];
    memcpy(buf, g_cfg.target_libs, sizeof buf);
    char* save = nullptr;
    for (char* tok = strtok_r(buf, ",", &save); tok != nullptr;
         tok = strtok_r(nullptr, ",", &save)) {
        if (*tok != 0 && strstr(path, tok) != nullptr) return true;
    }
    return false;
}

void add_app_range(uintptr_t lo, uintptr_t hi) {
    if (g_app_nranges < kMaxAppRanges && lo < hi) {
        g_app_ranges[g_app_nranges++] = Range{lo, hi};
    }
}

#ifdef __APPLE__
void init_app_ranges() {
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct mach_header* mh = _dyld_get_image_header(i);
        const char* name = _dyld_get_image_name(i);
        if (mh == nullptr) continue;
        bool is_main = mh->filetype == MH_EXECUTE;
        if (!is_main && !target_libs_match(name)) continue;
        unsigned long size = 0;
        uint8_t* seg = getsegmentdata((const struct mach_header_64*)mh, "__TEXT", &size);
        if (seg != nullptr && size > 0) {
            add_app_range((uintptr_t)seg, (uintptr_t)seg + size);
        }
    }
}
#else
void init_app_ranges() {
    char exe[512];
    ssize_t en = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[en > 0 ? en : 0] = 0;

    int fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return;

    char chunk[4096];
    char line[1024];
    size_t lp = 0;
    ssize_t rd;
    while ((rd = read(fd, chunk, sizeof chunk)) > 0) {
        for (ssize_t i = 0; i < rd; i++) {
            char c = chunk[i];
            if (c != '\n' && lp < sizeof line - 1) {
                line[lp++] = c;
                continue;
            }
            line[lp] = 0;
            lp = 0;
            // "lo-hi perms offset dev inode      path"
            char* dash = strchr(line, '-');
            char* sp1 = dash ? strchr(dash, ' ') : nullptr;
            if (dash == nullptr || sp1 == nullptr) continue;
            if (sp1[1] == 0 || sp1[3] != 'x') continue; // want r?xp
            const char* path = strchr(sp1, '/');
            if (path == nullptr) continue;
            bool is_exe = en > 0 && strcmp(path, exe) == 0;
            if (!is_exe && !target_libs_match(path)) continue;
            uintptr_t lo = strtoull(line, nullptr, 16);
            uintptr_t hi = strtoull(dash + 1, nullptr, 16);
            add_app_range(lo, hi);
        }
    }
    close(fd);
}
#endif

inline bool ip_in_app(const void* ip) {
    uintptr_t p = (uintptr_t)ip;
    for (int i = 0; i < g_app_nranges; i++) {
        if (p >= g_app_ranges[i].lo && p < g_app_ranges[i].hi) return true;
    }
    return false;
}

// ───────────────────────── tracked memory regions ───────────────────────────

constexpr int kMaxRegions = 512;
ShimSpinLock g_region_lock;
Range g_regions[kMaxRegions];
int g_nregions = 0;

void add_region(void* addr, size_t len) {
    if (addr == nullptr || len == 0) return;
    g_region_lock.lock();
    if (g_nregions < kMaxRegions) {
        g_regions[g_nregions++] = Range{(uintptr_t)addr, (uintptr_t)addr + len};
    }
    g_region_lock.unlock();
}

void remove_region(void* addr, size_t len) {
    uintptr_t lo = (uintptr_t)addr, hi = lo + len;
    g_region_lock.lock();
    for (int i = 0; i < g_nregions; i++) {
        Range& r = g_regions[i];
        if (r.hi <= lo || r.lo >= hi) continue;      // no overlap
        if (r.lo >= lo && r.hi <= hi) {              // fully covered: drop
            r = g_regions[--g_nregions];
            i--;
        } else if (r.lo < lo && r.hi > hi) {         // split
            uintptr_t old_hi = r.hi;
            r.hi = lo;
            if (g_nregions < kMaxRegions) {
                g_regions[g_nregions++] = Range{hi, old_hi};
            }
        } else if (r.lo < lo) {                      // trim tail
            r.hi = lo;
        } else {                                     // trim head
            r.lo = hi;
        }
    }
    g_region_lock.unlock();
}

bool addr_in_region(const void* p) {
    uintptr_t a = (uintptr_t)p;
    bool hit = false;
    g_region_lock.lock();
    for (int i = 0; i < g_nregions; i++) {
        if (a >= g_regions[i].lo && a < g_regions[i].hi) { hit = true; break; }
    }
    g_region_lock.unlock();
    return hit;
}

// ─────────────────────────── the mutex wrapper ──────────────────────────────

constexpr uint64_t kClaimMagic = 0x51C0FFEEB10C4ED5ull;

struct ClaimedHeader {
    uint64_t magic;
    struct ShimMutex* shim;
};
static_assert(sizeof(ClaimedHeader) <= sizeof(pthread_mutex_t),
              "pthread_mutex_t too small to claim");

struct ShimMutex {
    SoftwareMutex* impl = nullptr;
    std::atomic<uint32_t> owner_plus1{0}; // dense tid + 1; 0 = unowned
    uint32_t depth = 0;                   // recursion depth, owner-only
    uint64_t acquisitions = 0;            // updated while holding the lock
    pthread_mutex_t shadow;               // real mutex for the condvar protocol
};

inline ShimMutex* lookup(pthread_mutex_t* m) {
    if (((uintptr_t)m & 7) != 0) return nullptr;
    ClaimedHeader* h = reinterpret_cast<ClaimedHeader*>(m);
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == kClaimMagic) {
        return h->shim;
    }
    return nullptr;
}

ShimSpinLock g_claim_lock;

void disable_claims(const char* why) {
    g_cfg.claims_allowed.store(false, std::memory_order_release);
    shim_msg("mutex_shim: WARNING: %s — no further mutexes will be claimed; "
             "the target now runs (partly) on real pthread mutexes.\n", why);
    if (g_cfg.strict) shim_fatal(why);
}

// Claim `m`, i.e. install a ShimMutex over it. Returns nullptr if the mutex
// must stay on the real implementation (already ignored, currently held via
// the real path, or lock construction failed).
ShimMutex* try_claim(pthread_mutex_t* m) {
    if (((uintptr_t)m & 7) != 0) return nullptr;
    t_state.guard++;
    ShimMutex* result = nullptr;
    g_claim_lock.lock();

    ClaimedHeader* h = reinterpret_cast<ClaimedHeader*>(m);
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == kClaimMagic) {
        result = h->shim; // raced with another claimer: fine
    } else if (g_ignore_set.find(m) != nullptr) {
        result = nullptr;
    } else {
        // Probe: if the mutex is currently locked through the real path
        // (taken before the shim was ready, or from guarded shim internals),
        // claiming it now would let two owners coexist. Leave it real.
        int rc = CALL_REAL(pthread_mutex_trylock, m);
        if (rc == 0) CALL_REAL(pthread_mutex_unlock, m);
        if (rc == EBUSY) {
            if (!g_ignore_set.insert(m, (void*)1)) {
                disable_claims("ignore table full");
            }
            g_ignored.fetch_add(1, std::memory_order_relaxed);
        } else {
            SoftwareMutex* impl = get_mutex(g_cfg.lock_name, g_cfg.max_threads);
            if (impl == nullptr) {
                disable_claims("MUTEX_SHIM_LOCK did not name a known lock");
            } else {
                impl->init(g_cfg.max_threads);
                ShimMutex* sm = new ShimMutex();
                sm->impl = impl;
                CALL_REAL(pthread_mutex_init, &sm->shadow, nullptr);
                h->shim = sm;
                __atomic_store_n(&h->magic, kClaimMagic, __ATOMIC_RELEASE);
                g_claimed.fetch_add(1, std::memory_order_relaxed);
                if (g_cfg.log) shim_msg("mutex_shim: claimed mutex %p (#%llu)\n",
                                        (void*)m,
                                        (unsigned long long)g_claimed.load(std::memory_order_relaxed));
                result = sm;
            }
        }
    }

    g_claim_lock.unlock();
    t_state.guard--;
    return result;
}

// Undo a claim (pthread_mutex_destroy, or re-init over a claimed mutex).
// The ShimMutex itself (and its shadow) is intentionally leaked: a concurrent
// strict cond_signal may still dereference it, and the impl is the only
// meaningful memory consumer.
void release_claim(pthread_mutex_t* m, ShimMutex* sm) {
    t_state.guard++;
    g_claim_lock.lock();
    ClaimedHeader* h = reinterpret_cast<ClaimedHeader*>(m);
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) == kClaimMagic && h->shim == sm) {
        __atomic_store_n(&h->magic, 0, __ATOMIC_RELEASE);
        if (sm->impl != nullptr) {
            sm->impl->destroy();
            delete sm->impl;
            sm->impl = nullptr;
        }
    }
    g_claim_lock.unlock();
    t_state.guard--;
}

// ───────────────────────── lock/unlock on a claim ───────────────────────────

// The repo's locks were validated under a benchmark whose critical sections
// call Fence() themselves (criticalSection(), max_contention_bench), so some
// implementations lean on those fences instead of carrying release/acquire
// on their handoff path (e.g. MCS hands off through a plain volatile
// locked-flag: fine on x86/TSO, loses critical-section writes on arm64).
// Reproduce that discipline here: full fence after acquiring and before
// releasing. Disable with MUTEX_SHIM_FENCE=0 for locks known to be
// self-contained.
inline void acquire_side_fence() {
    if (g_cfg.fence) std::atomic_thread_fence(std::memory_order_seq_cst);
}
inline void release_side_fence() {
    if (g_cfg.fence) std::atomic_thread_fence(std::memory_order_seq_cst);
}

int shim_lock_acquire(ShimMutex* sm) {
    uint32_t me = my_tid() + 1;
    if (sm->owner_plus1.load(std::memory_order_relaxed) == me) {
        sm->depth++; // recursion emulation
        return 0;
    }
    t_state.guard++;
    sm->impl->lock(me - 1);
    t_state.guard--;
    acquire_side_fence();
    sm->owner_plus1.store(me, std::memory_order_relaxed);
    sm->depth = 1;
    sm->acquisitions++;
    g_slots[me - 1].acquisitions++;
    return 0;
}

int shim_trylock(ShimMutex* sm) {
    uint32_t me = my_tid() + 1;
    uint32_t cur = sm->owner_plus1.load(std::memory_order_relaxed);
    if (cur == me) {
        sm->depth++;
        return 0;
    }
    if (cur != 0) {
        g_slots[me - 1].trylock_busy++;
        return EBUSY;
    }
    // Observed free: acquire. Under a race this can block briefly — the
    // SoftwareMutex interface has no native trylock (documented caveat).
    t_state.guard++;
    sm->impl->lock(me - 1);
    t_state.guard--;
    acquire_side_fence();
    sm->owner_plus1.store(me, std::memory_order_relaxed);
    sm->depth = 1;
    sm->acquisitions++;
    g_slots[me - 1].acquisitions++;
    return 0;
}

int shim_unlock(ShimMutex* sm) {
    uint32_t me = t_state.tid_plus1; // never locked anything => can't own
    if (me == 0 || sm->owner_plus1.load(std::memory_order_relaxed) != me) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            shim_msg("mutex_shim: WARNING: unlock of a claimed mutex by a "
                     "non-owner thread (app uses cross-thread unlock?) — "
                     "returning EPERM.\n");
        }
        g_eperm_unlocks.fetch_add(1, std::memory_order_relaxed);
        return EPERM;
    }
    if (--sm->depth > 0) return 0;
    sm->owner_plus1.store(0, std::memory_order_relaxed);
    release_side_fence();
    t_state.guard++;
    sm->impl->unlock(me - 1);
    t_state.guard--;
    return 0;
}

// ───────────────────────────── hook bodies ──────────────────────────────────

inline bool shim_active() {
    return g_cfg.ready.load(std::memory_order_acquire) &&
           g_cfg.enabled &&
           g_cfg.claims_allowed.load(std::memory_order_relaxed);
}

inline void count_passthrough() {
    if (g_cfg.stats) g_passthrough.fetch_add(1, std::memory_order_relaxed);
}

bool filter_claimable(pthread_mutex_t* m, const void* caller) {
    switch (g_cfg.scope) {
        case Scope::All:    return true;
        case Scope::App:    return ip_in_app(caller);
        case Scope::Region: return addr_in_region(m);
    }
    return false;
}

enum class AcqOp { Lock, Try, Timed };

int hook_mutex_acquire(pthread_mutex_t* m, const void* caller, AcqOp op,
                       const struct timespec* ts) {
    ShimMutex* sm = lookup(m);
    if (sm == nullptr && t_state.guard == 0 && shim_active() &&
        !tl_ignored(m)) {
        if (g_ignore_set.find(m) != nullptr || !filter_claimable(m, caller)) {
            tl_ignore_add(m);
        } else {
            sm = try_claim(m);
            if (sm == nullptr) tl_ignore_add(m);
        }
    }
    if (sm != nullptr) {
        if (op == AcqOp::Try) return shim_trylock(sm);
        // Timed degrades to a blocking lock (documented caveat).
        return shim_lock_acquire(sm);
    }
    count_passthrough();
    switch (op) {
        case AcqOp::Try:   return CALL_REAL(pthread_mutex_trylock, m);
#ifndef __APPLE__
        case AcqOp::Timed: return CALL_REAL(pthread_mutex_timedlock, m, ts);
#endif
        default:           (void)ts; return CALL_REAL(pthread_mutex_lock, m);
    }
}

int hook_mutex_unlock(pthread_mutex_t* m) {
    ShimMutex* sm = lookup(m);
    if (sm != nullptr) return shim_unlock(sm);
    return CALL_REAL(pthread_mutex_unlock, m);
}

int hook_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    if (g_cfg.ready.load(std::memory_order_acquire) && t_state.guard == 0) {
        ShimMutex* old = lookup(m);
        if (old != nullptr) release_claim(m, old); // re-init over a live claim
        if (g_cfg.enabled) {
            bool ignore = false;
            if (a != nullptr) {
                int v = 0;
                if (pthread_mutexattr_getpshared(a, &v) == 0 &&
                    v == PTHREAD_PROCESS_SHARED) {
                    ignore = true;
                }
#ifdef __linux__
                if (!ignore && pthread_mutexattr_getprotocol(a, &v) == 0 &&
                    v != PTHREAD_PRIO_NONE) {
                    ignore = true;
                }
                if (!ignore && pthread_mutexattr_getrobust(a, &v) == 0 &&
                    v == PTHREAD_MUTEX_ROBUST) {
                    ignore = true;
                }
#endif
            }
            if (ignore) {
                if (!g_ignore_set.insert(m, (void*)1)) {
                    disable_claims("ignore table full");
                }
                g_ignored.fetch_add(1, std::memory_order_relaxed);
                if (g_cfg.log) shim_msg("mutex_shim: ignoring mutex %p "
                                        "(pshared/robust/priority attr)\n", (void*)m);
            } else {
                g_ignore_set.erase(m); // address reuse: allow claiming again
            }
        }
    }
    return CALL_REAL(pthread_mutex_init, m, a);
}

int hook_mutex_destroy(pthread_mutex_t* m) {
    ShimMutex* sm = lookup(m);
    if (sm != nullptr) {
        release_claim(m, sm);
        return 0; // the real mutex was left unlocked and holds no resources
    }
    if (g_cfg.ready.load(std::memory_order_acquire) && g_cfg.enabled &&
        t_state.guard == 0) {
        g_ignore_set.erase(m);
    }
    return CALL_REAL(pthread_mutex_destroy, m);
}

int hook_cond_wait_common(pthread_cond_t* c, pthread_mutex_t* m,
                          const struct timespec* abstime) {
    ShimMutex* sm = lookup(m);
    if (sm == nullptr) {
        return abstime != nullptr ? CALL_REAL(pthread_cond_timedwait, c, m, abstime)
                                  : CALL_REAL(pthread_cond_wait, c, m);
    }
    uint32_t me = t_state.tid_plus1;
    if (me == 0 || sm->owner_plus1.load(std::memory_order_relaxed) != me) {
        return EPERM;
    }
    if (g_cfg.cond_strict && g_cond_map.find(c) != (void*)sm) {
        g_cond_map.insert(c, (void*)sm); // best effort; full table = LiTL mode
    }
    uint32_t saved_depth = sm->depth;
    t_state.guard++;
    // Shadow-mutex protocol: hold the shadow across "release custom lock" so
    // a strict signaler (which also takes the shadow) cannot fire the condvar
    // before this thread is atomically parked on it.
    CALL_REAL(pthread_mutex_lock, &sm->shadow);
    sm->owner_plus1.store(0, std::memory_order_relaxed);
    sm->depth = 0;
    release_side_fence();
    sm->impl->unlock(me - 1);
    int rc = abstime != nullptr
                 ? CALL_REAL(pthread_cond_timedwait, c, &sm->shadow, abstime)
                 : CALL_REAL(pthread_cond_wait, c, &sm->shadow);
    CALL_REAL(pthread_mutex_unlock, &sm->shadow);
    sm->impl->lock(me - 1);
    t_state.guard--;
    acquire_side_fence();
    sm->owner_plus1.store(me, std::memory_order_relaxed);
    sm->depth = saved_depth;
    sm->acquisitions++;
    g_slots[me - 1].acquisitions++;
    g_slots[me - 1].cond_waits++;
    return rc;
}

int hook_cond_wake(pthread_cond_t* c, bool broadcast) {
    ShimMutex* sm = nullptr;
    if (g_cfg.ready.load(std::memory_order_acquire) && g_cfg.enabled &&
        g_cfg.cond_strict) {
        sm = (ShimMutex*)g_cond_map.find(c);
    }
    if (sm != nullptr) {
        t_state.guard++;
        CALL_REAL(pthread_mutex_lock, &sm->shadow);
        int rc = broadcast ? CALL_REAL(pthread_cond_broadcast, c)
                           : CALL_REAL(pthread_cond_signal, c);
        CALL_REAL(pthread_mutex_unlock, &sm->shadow);
        t_state.guard--;
        return rc;
    }
    return broadcast ? CALL_REAL(pthread_cond_broadcast, c)
                     : CALL_REAL(pthread_cond_signal, c);
}

int hook_cond_destroy(pthread_cond_t* c) {
    if (g_cfg.ready.load(std::memory_order_acquire) && g_cfg.enabled) {
        g_cond_map.erase(c);
    }
    return CALL_REAL(pthread_cond_destroy, c);
}

bool region_flags_match(int flags) {
    switch (g_cfg.region_flags) {
        case 0:  return (flags & MAP_SHARED) != 0;
#ifdef MAP_ANONYMOUS
        case 1:  return (flags & (MAP_SHARED | MAP_ANONYMOUS)) != 0;
#else
        case 1:  return (flags & (MAP_SHARED | MAP_ANON)) != 0;
#endif
        default: return true;
    }
}

void* hook_mmap(void* a, size_t len, int prot, int flags, int fd, off_t off) {
    void* r = CALL_REAL(mmap, a, len, prot, flags, fd, off);
    if (r != MAP_FAILED && g_cfg.ready.load(std::memory_order_acquire) &&
        g_cfg.enabled && g_cfg.scope == Scope::Region && region_flags_match(flags)) {
        add_region(r, len);
    }
    return r;
}

int hook_munmap(void* a, size_t len) {
    if (g_cfg.ready.load(std::memory_order_acquire) && g_cfg.enabled &&
        g_cfg.scope == Scope::Region) {
        remove_region(a, len);
    }
    return CALL_REAL(munmap, a, len);
}

// ───────────────────────── init / stats teardown ────────────────────────────

uint32_t env_u32(const char* name, uint32_t dflt, uint32_t lo, uint32_t hi) {
    const char* s = getenv(name);
    if (s == nullptr || *s == 0) return dflt;
    unsigned long v = strtoul(s, nullptr, 10);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (uint32_t)v;
}

bool env_flag(const char* name, bool dflt) {
    const char* s = getenv(name);
    if (s == nullptr || *s == 0) return dflt;
    return !(s[0] == '0' && s[1] == 0);
}

__attribute__((constructor))
void shim_init() {
    if (g_cfg.ready.load(std::memory_order_acquire)) return;
#ifndef __APPLE__
    ensure_reals();
#endif
    const char* ln = getenv("MUTEX_SHIM_LOCK");
    g_cfg.enabled = ln != nullptr && *ln != 0;
    if (g_cfg.enabled) {
        strncpy(g_cfg.lock_name, ln, sizeof g_cfg.lock_name - 1);
    }
    g_cfg.max_threads = env_u32("MUTEX_SHIM_MAX_THREADS", 128, 2, 65536);
    g_cfg.stats = env_flag("MUTEX_SHIM_STATS", false);
    g_cfg.log = env_flag("MUTEX_SHIM_LOG", false);
    g_cfg.cond_strict = env_flag("MUTEX_SHIM_COND_STRICT", true);
    g_cfg.strict = env_flag("MUTEX_SHIM_STRICT", false);
    g_cfg.fence = env_flag("MUTEX_SHIM_FENCE", true);
    const char* sf = getenv("MUTEX_SHIM_STATS_FILE");
    if (sf != nullptr) strncpy(g_cfg.stats_file, sf, sizeof g_cfg.stats_file - 1);
    const char* tl = getenv("MUTEX_SHIM_TARGET_LIBS");
    if (tl != nullptr) strncpy(g_cfg.target_libs, tl, sizeof g_cfg.target_libs - 1);

    const char* sc = getenv("MUTEX_SHIM_SCOPE");
    if (sc != nullptr) {
        if (strcmp(sc, "app") == 0) g_cfg.scope = Scope::App;
        else if (strcmp(sc, "region") == 0) g_cfg.scope = Scope::Region;
        else g_cfg.scope = Scope::All;
    }
    const char* rf = getenv("MUTEX_SHIM_REGION_FLAGS");
    if (rf != nullptr) {
        if (strcmp(rf, "anon") == 0) g_cfg.region_flags = 1;
        else if (strcmp(rf, "all") == 0) g_cfg.region_flags = 2;
    }

    g_free_tids = (uint32_t*)calloc(g_cfg.max_threads, sizeof(uint32_t));
    void* slots = nullptr;
    if (posix_memalign(&slots, 64, (size_t)g_cfg.max_threads * sizeof(ThreadSlot)) == 0) {
        memset(slots, 0, (size_t)g_cfg.max_threads * sizeof(ThreadSlot));
        g_slots = (ThreadSlot*)slots;
    }
    g_ignore_set.init(16384);
    g_cond_map.init(8192);
    pthread_key_create(&g_tid_key, tid_key_dtor);

    if (g_cfg.scope == Scope::App) {
        init_app_ranges();
        if (g_app_nranges == 0) {
            shim_msg("mutex_shim: WARNING: scope=app but no executable text "
                     "ranges found; nothing will be claimed.\n");
        }
    }

    bool ok = g_free_tids != nullptr && g_slots != nullptr &&
              g_ignore_set.e != nullptr && g_cond_map.e != nullptr;
    g_cfg.claims_allowed.store(g_cfg.enabled && ok, std::memory_order_release);
    g_cfg.ready.store(true, std::memory_order_release);

    if (g_cfg.log) {
        shim_msg("mutex_shim: ready (lock=%s scope=%d max_threads=%u)\n",
                 g_cfg.enabled ? g_cfg.lock_name : "<none: passthrough>",
                 (int)g_cfg.scope, g_cfg.max_threads);
    }
}

__attribute__((destructor))
void shim_fini() {
    // Shutdown mode: exit() runs static destructors, and libc teardown paths
    // (e.g. fflush -> flockfile on macOS) can present never-before-seen
    // mutexes to the hooks. Claiming one now would call get_mutex() on an
    // already-destroyed factory table, so stop claiming; existing claims keep
    // working. This TU is linked after bench_utils.cpp, so this finalizer
    // runs before bench_utils' static destructors.
    g_cfg.claims_allowed.store(false, std::memory_order_release);
    if (!g_cfg.ready.load(std::memory_order_acquire) || !g_cfg.stats) return;

    uint64_t acq = 0, cwaits = 0, tbusy = 0;
    uint32_t created = g_tids_created.load(std::memory_order_relaxed);
    if (created > g_cfg.max_threads) created = g_cfg.max_threads;
    if (g_slots != nullptr) {
        for (uint32_t i = 0; i < created; i++) {
            acq += g_slots[i].acquisitions;
            cwaits += g_slots[i].cond_waits;
            tbusy += g_slots[i].trylock_busy;
        }
    }

    int fd = 2;
    bool close_fd = false;
    if (g_cfg.stats_file[0] != 0) {
        int f = open(g_cfg.stats_file, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (f >= 0) { fd = f; close_fd = true; }
    }
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
                     "lock_name=%s\n"
                     "claimed_mutexes=%llu\n"
                     "ignored_mutexes=%llu\n"
                     "shim_acquisitions=%llu\n"
                     "cond_waits=%llu\n"
                     "trylock_busy=%llu\n"
                     "passthrough_locks=%llu\n"
                     "eperm_unlocks=%llu\n"
                     "thread_ids_created=%llu\n",
                     g_cfg.enabled ? g_cfg.lock_name : "<none>",
                     (unsigned long long)g_claimed.load(std::memory_order_relaxed),
                     (unsigned long long)g_ignored.load(std::memory_order_relaxed),
                     (unsigned long long)acq,
                     (unsigned long long)cwaits,
                     (unsigned long long)tbusy,
                     (unsigned long long)g_passthrough.load(std::memory_order_relaxed),
                     (unsigned long long)g_eperm_unlocks.load(std::memory_order_relaxed),
                     (unsigned long long)g_tids_created.load(std::memory_order_relaxed));
    if (n > 0) {
        ssize_t r = write(fd, buf, (size_t)n);
        (void)r;
    }
    if (close_fd) close(fd);
}

} // namespace

// ───────────────────────────── exported hooks ───────────────────────────────

#ifdef __APPLE__

// dyld interposing: tuples in __DATA,__interpose rebind every image except
// this one. Requires DYLD_INSERT_LIBRARIES and a non-hardened target.
#define SHIM_INTERPOSE(name)                                                   \
    __attribute__((used, section("__DATA,__interpose")))                       \
    static const void* const interpose_##name[2] = {                           \
        reinterpret_cast<const void*>(&my_##name),                             \
        reinterpret_cast<const void*>(&::name)};

static int my_pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    return hook_mutex_init(m, a);
}
static int my_pthread_mutex_destroy(pthread_mutex_t* m) { return hook_mutex_destroy(m); }
static int my_pthread_mutex_lock(pthread_mutex_t* m) {
    return hook_mutex_acquire(m, __builtin_return_address(0), AcqOp::Lock, nullptr);
}
static int my_pthread_mutex_trylock(pthread_mutex_t* m) {
    return hook_mutex_acquire(m, __builtin_return_address(0), AcqOp::Try, nullptr);
}
static int my_pthread_mutex_unlock(pthread_mutex_t* m) { return hook_mutex_unlock(m); }
static int my_pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    return hook_cond_wait_common(c, m, nullptr);
}
static int my_pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                                     const struct timespec* ts) {
    return hook_cond_wait_common(c, m, ts);
}
static int my_pthread_cond_signal(pthread_cond_t* c) { return hook_cond_wake(c, false); }
static int my_pthread_cond_broadcast(pthread_cond_t* c) { return hook_cond_wake(c, true); }
static int my_pthread_cond_destroy(pthread_cond_t* c) { return hook_cond_destroy(c); }
static void* my_mmap(void* a, size_t l, int p, int f, int fd, off_t o) {
    return hook_mmap(a, l, p, f, fd, o);
}
static int my_munmap(void* a, size_t l) { return hook_munmap(a, l); }

SHIM_INTERPOSE(pthread_mutex_init)
SHIM_INTERPOSE(pthread_mutex_destroy)
SHIM_INTERPOSE(pthread_mutex_lock)
SHIM_INTERPOSE(pthread_mutex_trylock)
SHIM_INTERPOSE(pthread_mutex_unlock)
SHIM_INTERPOSE(pthread_cond_wait)
SHIM_INTERPOSE(pthread_cond_timedwait)
SHIM_INTERPOSE(pthread_cond_signal)
SHIM_INTERPOSE(pthread_cond_broadcast)
SHIM_INTERPOSE(pthread_cond_destroy)
SHIM_INTERPOSE(mmap)
SHIM_INTERPOSE(munmap)

#else // ── Linux: export the POSIX names; LD_PRELOAD puts us first in scope ──

// glibc declares most of these noexcept in C++ (__THROW/__THROWNL); the
// definitions must match or the compiler rejects the redeclaration.
#if defined(__GLIBC__)
#define SHIM_NOTHROW noexcept
#else
#define SHIM_NOTHROW
#endif

extern "C" {

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) SHIM_NOTHROW {
    return hook_mutex_init(m, a);
}
int pthread_mutex_destroy(pthread_mutex_t* m) SHIM_NOTHROW {
    return hook_mutex_destroy(m);
}
int pthread_mutex_lock(pthread_mutex_t* m) SHIM_NOTHROW {
    return hook_mutex_acquire(m, __builtin_return_address(0), AcqOp::Lock, nullptr);
}
int pthread_mutex_trylock(pthread_mutex_t* m) SHIM_NOTHROW {
    return hook_mutex_acquire(m, __builtin_return_address(0), AcqOp::Try, nullptr);
}
int pthread_mutex_timedlock(pthread_mutex_t* m, const struct timespec* ts) SHIM_NOTHROW {
    return hook_mutex_acquire(m, __builtin_return_address(0), AcqOp::Timed, ts);
}
int pthread_mutex_unlock(pthread_mutex_t* m) SHIM_NOTHROW {
    return hook_mutex_unlock(m);
}
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    return hook_cond_wait_common(c, m, nullptr);
}
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                           const struct timespec* ts) {
    return hook_cond_wait_common(c, m, ts);
}
int pthread_cond_signal(pthread_cond_t* c) SHIM_NOTHROW {
    return hook_cond_wake(c, false);
}
int pthread_cond_broadcast(pthread_cond_t* c) SHIM_NOTHROW {
    return hook_cond_wake(c, true);
}
int pthread_cond_destroy(pthread_cond_t* c) SHIM_NOTHROW {
    return hook_cond_destroy(c);
}
void* mmap(void* a, size_t l, int p, int f, int fd, off_t o) SHIM_NOTHROW {
    return hook_mmap(a, l, p, f, fd, o);
}
#if defined(__GLIBC__)
// Apps built with _FILE_OFFSET_BITS=64 bind to the distinct mmap64 symbol;
// without this hook their shared-memory regions would escape region scope.
void* mmap64(void* a, size_t l, int p, int f, int fd, off64_t o) SHIM_NOTHROW {
    return hook_mmap(a, l, p, f, fd, (off_t)o);
}
#endif
int munmap(void* a, size_t l) SHIM_NOTHROW {
    return hook_munmap(a, l);
}

#ifdef SHIM_HAVE_NUMAIF
// numa_alloc_onnode() & friends mmap anonymous memory and then mbind it;
// in region scope an mbind()-ed range is exactly the "interesting" memory.
long mbind(void* start, unsigned long len, int mode, const unsigned long* nmask,
           unsigned long maxnode, unsigned flags) {
    typedef long (*fn_mbind)(void*, unsigned long, int, const unsigned long*,
                             unsigned long, unsigned);
    static std::atomic<fn_mbind> real_mbind{nullptr};
    fn_mbind fn = real_mbind.load(std::memory_order_acquire);
    if (fn == nullptr) {
        fn = (fn_mbind)dlsym(RTLD_NEXT, "mbind");
        if (fn == nullptr) { errno = ENOSYS; return -1; }
        real_mbind.store(fn, std::memory_order_release);
    }
    long rc = fn(start, len, mode, nmask, maxnode, flags);
    if (rc == 0 && g_cfg.ready.load(std::memory_order_acquire) && g_cfg.enabled &&
        g_cfg.scope == Scope::Region) {
        add_region(start, len);
    }
    return rc;
}
#endif // SHIM_HAVE_NUMAIF

} // extern "C"

#endif // __APPLE__
