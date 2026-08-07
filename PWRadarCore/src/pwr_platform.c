/* ==========================================================================
 *  PWRadarSystem - PWRadarCore (internal)
 *  File    : pwr_platform.c
 *  Purpose : Win32 / POSIX implementation of the platform abstraction.
 *  Language: ISO C17
 * ========================================================================== */
/* Feature-test macros must precede every system header so that the POSIX
 * threading, monotonic-clock and aligned-allocation declarations are visible
 * even under a strict `-std=c17` (no `-std=gnu17`) compilation. */
#if !defined(_WIN32)
#  if !defined(_GNU_SOURCE)
#    define _GNU_SOURCE 1
#  endif
#  if !defined(_POSIX_C_SOURCE)
#    define _POSIX_C_SOURCE 200809L
#  endif
#  if !defined(_XOPEN_SOURCE)
#    define _XOPEN_SOURCE 700
#  endif
#endif

#include "pwr_platform.h"

#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 *  Windows
 * ========================================================================== */
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#  define NOMINMAX 1
#endif
#include <windows.h>
#include <malloc.h>
#include <process.h>
#include <intrin.h>

/* ---- aligned allocation ------------------------------------------------- */
void* pwr_aligned_alloc(size_t bytes, size_t alignment)
{
    if (bytes == 0u) { bytes = 1u; }
    if (alignment < sizeof(void*)) { alignment = sizeof(void*); }
    return _aligned_malloc(bytes, alignment);
}

void pwr_aligned_free(void* p)
{
    if (p != NULL) { _aligned_free(p); }
}

/* ---- clock -------------------------------------------------------------- */
static double   g_qpc_period   = 0.0;
static LONGLONG g_qpc_origin   = 0;
static LONG     g_clock_ready  = 0;

static void pwr_clock_init(void)
{
    if (InterlockedCompareExchange(&g_clock_ready, 1, 0) == 0)
    {
        LARGE_INTEGER f, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t);
        g_qpc_period = (f.QuadPart != 0) ? (1.0 / (double)f.QuadPart) : 0.0;
        g_qpc_origin = t.QuadPart;
        /* Publish after the fields are written. */
        MemoryBarrier();
        InterlockedExchange(&g_clock_ready, 2);
    }
    else
    {
        while (InterlockedCompareExchange(&g_clock_ready, 2, 2) != 2)
        {
            YieldProcessor();
        }
    }
}

double pwr_plat_now_s(void)
{
    LARGE_INTEGER t;
    pwr_clock_init();
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc_origin) * g_qpc_period;
}

void pwr_plat_sleep_s(double s)
{
    if (s <= 0.0) { SwitchToThread(); return; }
    /* Busy-wait the last 1 ms so that CPI pacing stays tight even with the
     * default 15.6 ms scheduler quantum. */
    {
        const double deadline = pwr_plat_now_s() + s;
        double remaining = s;
        while (remaining > 0.0015)
        {
            DWORD ms = (DWORD)((remaining - 0.0010) * 1000.0);
            if (ms == 0u) { break; }
            Sleep(ms);
            remaining = deadline - pwr_plat_now_s();
        }
        while (pwr_plat_now_s() < deadline) { YieldProcessor(); }
    }
}

void pwr_plat_yield(void) { SwitchToThread(); }

/* ---- mutex -------------------------------------------------------------- */
PWR_Status pwr_mutex_init(PWR_Mutex* m)
{
    if (m == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(m, 0, sizeof(*m));
    InitializeCriticalSectionAndSpinCount((CRITICAL_SECTION*)m->storage.raw, 2000u);
    m->initialised = 1;
    return PWR_STATUS_OK;
}

void pwr_mutex_destroy(PWR_Mutex* m)
{
    if (m != NULL && m->initialised)
    {
        DeleteCriticalSection((CRITICAL_SECTION*)m->storage.raw);
        m->initialised = 0;
    }
}

void pwr_mutex_lock(PWR_Mutex* m)
{
    if (m != NULL && m->initialised)
    {
        EnterCriticalSection((CRITICAL_SECTION*)m->storage.raw);
    }
}

void pwr_mutex_unlock(PWR_Mutex* m)
{
    if (m != NULL && m->initialised)
    {
        LeaveCriticalSection((CRITICAL_SECTION*)m->storage.raw);
    }
}

/* ---- condition variable ------------------------------------------------- */
PWR_Status pwr_cond_init(PWR_Cond* c)
{
    if (c == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(c, 0, sizeof(*c));
    InitializeConditionVariable((CONDITION_VARIABLE*)c->storage.raw);
    c->initialised = 1;
    return PWR_STATUS_OK;
}

void pwr_cond_destroy(PWR_Cond* c)
{
    /* CONDITION_VARIABLE needs no explicit teardown. */
    if (c != NULL) { c->initialised = 0; }
}

void pwr_cond_wait(PWR_Cond* c, PWR_Mutex* m)
{
    if (c == NULL || m == NULL || !c->initialised) { return; }
    SleepConditionVariableCS((CONDITION_VARIABLE*)c->storage.raw,
                             (CRITICAL_SECTION*)m->storage.raw, INFINITE);
}

void pwr_cond_wait_timed(PWR_Cond* c, PWR_Mutex* m, double timeout_s)
{
    DWORD ms;
    if (c == NULL || m == NULL || !c->initialised) { return; }
    ms = (timeout_s <= 0.0) ? 0u : (DWORD)(timeout_s * 1000.0 + 0.5);
    SleepConditionVariableCS((CONDITION_VARIABLE*)c->storage.raw,
                             (CRITICAL_SECTION*)m->storage.raw, ms);
}

void pwr_cond_signal(PWR_Cond* c)
{
    if (c != NULL && c->initialised)
    {
        WakeConditionVariable((CONDITION_VARIABLE*)c->storage.raw);
    }
}

void pwr_cond_broadcast(PWR_Cond* c)
{
    if (c != NULL && c->initialised)
    {
        WakeAllConditionVariable((CONDITION_VARIABLE*)c->storage.raw);
    }
}

/* ---- thread ------------------------------------------------------------- */
struct PWR_Thread
{
    HANDLE       handle;
    PWR_ThreadFn fn;
    void*        arg;
};

static unsigned __stdcall pwr_thread_trampoline(void* p)
{
    PWR_Thread* t = (PWR_Thread*)p;
    t->fn(t->arg);
    return 0u;
}

PWR_Status pwr_thread_create(PWR_Thread** out, PWR_ThreadFn fn, void* arg,
                             const char* name)
{
    PWR_Thread* t;
    PWR_UNUSED(name);
    if (out == NULL || fn == NULL) { return PWR_ERR_NULL_POINTER; }
    *out = NULL;
    t = (PWR_Thread*)calloc(1u, sizeof(*t));
    if (t == NULL) { return PWR_ERR_OUT_OF_MEMORY; }
    t->fn  = fn;
    t->arg = arg;
    t->handle = (HANDLE)_beginthreadex(NULL, 0u, pwr_thread_trampoline, t, 0u, NULL);
    if (t->handle == NULL) { free(t); return PWR_ERR_THREAD; }
    *out = t;
    return PWR_STATUS_OK;
}

void pwr_thread_join(PWR_Thread* t)
{
    if (t == NULL) { return; }
    if (t->handle != NULL)
    {
        WaitForSingleObject(t->handle, INFINITE);
        CloseHandle(t->handle);
    }
    free(t);
}

uint32_t pwr_cpu_count(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (si.dwNumberOfProcessors > 0u) ? (uint32_t)si.dwNumberOfProcessors : 1u;
}

/* ---- atomics ------------------------------------------------------------ */
int32_t pwr_atomic_load_i32(const volatile int32_t* p)
{
    int32_t v = *p;
    _ReadWriteBarrier();
    return v;
}

void pwr_atomic_store_i32(volatile int32_t* p, int32_t v)
{
    _InterlockedExchange((volatile long*)p, (long)v);
}

int32_t pwr_atomic_add_i32(volatile int32_t* p, int32_t delta)
{
    return (int32_t)_InterlockedExchangeAdd((volatile long*)p, (long)delta) + delta;
}

int32_t pwr_atomic_cas_i32(volatile int32_t* p, int32_t expected, int32_t desired)
{
    return (int32_t)_InterlockedCompareExchange((volatile long*)p,
                                                (long)desired, (long)expected);
}

uint64_t pwr_atomic_load_u64(const volatile uint64_t* p)
{
#if defined(_WIN64)
    uint64_t v = *p;
    _ReadWriteBarrier();
    return v;
#else
    return (uint64_t)_InterlockedCompareExchange64((volatile __int64*)p, 0, 0);
#endif
}

void pwr_atomic_store_u64(volatile uint64_t* p, uint64_t v)
{
    _InterlockedExchange64((volatile __int64*)p, (__int64)v);
}

void pwr_atomic_fence(void) { MemoryBarrier(); }

/* ==========================================================================
 *  POSIX
 * ========================================================================== */
#else /* !_WIN32 */

#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>

void* pwr_aligned_alloc(size_t bytes, size_t alignment)
{
    void* p = NULL;
    if (bytes == 0u) { bytes = 1u; }
    if (alignment < sizeof(void*)) { alignment = sizeof(void*); }
    /* posix_memalign requires alignment to be a power of two multiple of
     * sizeof(void*), which PWR_SIMD_ALIGN satisfies. */
    if (posix_memalign(&p, alignment, bytes) != 0) { return NULL; }
    return p;
}

void pwr_aligned_free(void* p) { free(p); }

static double g_origin_s = 0.0;
static pthread_once_t g_clock_once = PTHREAD_ONCE_INIT;

static double pwr_raw_now_s(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC_RAW)
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
#endif
    {
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) { return 0.0; }
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void pwr_clock_once_fn(void) { g_origin_s = pwr_raw_now_s(); }

double pwr_plat_now_s(void)
{
    pthread_once(&g_clock_once, pwr_clock_once_fn);
    return pwr_raw_now_s() - g_origin_s;
}

void pwr_plat_sleep_s(double s)
{
    struct timespec req, rem;
    if (s <= 0.0) { sched_yield(); return; }
    req.tv_sec  = (time_t)s;
    req.tv_nsec = (long)((s - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0) { req.tv_nsec = 0; }
    if (req.tv_nsec > 999999999L) { req.tv_nsec = 999999999L; }
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) { req = rem; }
}

void pwr_plat_yield(void) { sched_yield(); }

PWR_Status pwr_mutex_init(PWR_Mutex* m)
{
    pthread_mutexattr_t attr;
    if (m == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(m, 0, sizeof(*m));
    if (pthread_mutexattr_init(&attr) != 0) { return PWR_ERR_THREAD; }
    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
    if (pthread_mutex_init((pthread_mutex_t*)m->storage.raw, &attr) != 0)
    {
        pthread_mutexattr_destroy(&attr);
        return PWR_ERR_THREAD;
    }
    pthread_mutexattr_destroy(&attr);
    m->initialised = 1;
    return PWR_STATUS_OK;
}

void pwr_mutex_destroy(PWR_Mutex* m)
{
    if (m != NULL && m->initialised)
    {
        pthread_mutex_destroy((pthread_mutex_t*)m->storage.raw);
        m->initialised = 0;
    }
}

void pwr_mutex_lock(PWR_Mutex* m)
{
    if (m != NULL && m->initialised)
    {
        (void)pthread_mutex_lock((pthread_mutex_t*)m->storage.raw);
    }
}

void pwr_mutex_unlock(PWR_Mutex* m)
{
    if (m != NULL && m->initialised)
    {
        (void)pthread_mutex_unlock((pthread_mutex_t*)m->storage.raw);
    }
}

PWR_Status pwr_cond_init(PWR_Cond* c)
{
    if (c == NULL) { return PWR_ERR_NULL_POINTER; }
    memset(c, 0, sizeof(*c));
    if (pthread_cond_init((pthread_cond_t*)c->storage.raw, NULL) != 0)
    {
        return PWR_ERR_THREAD;
    }
    c->initialised = 1;
    return PWR_STATUS_OK;
}

void pwr_cond_destroy(PWR_Cond* c)
{
    if (c != NULL && c->initialised)
    {
        pthread_cond_destroy((pthread_cond_t*)c->storage.raw);
        c->initialised = 0;
    }
}

void pwr_cond_wait(PWR_Cond* c, PWR_Mutex* m)
{
    if (c == NULL || m == NULL || !c->initialised) { return; }
    (void)pthread_cond_wait((pthread_cond_t*)c->storage.raw,
                            (pthread_mutex_t*)m->storage.raw);
}

void pwr_cond_wait_timed(PWR_Cond* c, PWR_Mutex* m, double timeout_s)
{
    struct timespec ts;
    long   add_ns;
    time_t add_s;
    if (c == NULL || m == NULL || !c->initialised) { return; }
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { return; }
    if (timeout_s < 0.0) { timeout_s = 0.0; }
    add_s  = (time_t)timeout_s;
    add_ns = (long)((timeout_s - (double)add_s) * 1e9);
    ts.tv_sec  += add_s;
    ts.tv_nsec += add_ns;
    while (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
    (void)pthread_cond_timedwait((pthread_cond_t*)c->storage.raw,
                                 (pthread_mutex_t*)m->storage.raw, &ts);
}

void pwr_cond_signal(PWR_Cond* c)
{
    if (c != NULL && c->initialised)
    {
        (void)pthread_cond_signal((pthread_cond_t*)c->storage.raw);
    }
}

void pwr_cond_broadcast(PWR_Cond* c)
{
    if (c != NULL && c->initialised)
    {
        (void)pthread_cond_broadcast((pthread_cond_t*)c->storage.raw);
    }
}

struct PWR_Thread
{
    pthread_t    handle;
    PWR_ThreadFn fn;
    void*        arg;
    int          started;
};

static void* pwr_thread_trampoline(void* p)
{
    PWR_Thread* t = (PWR_Thread*)p;
    t->fn(t->arg);
    return NULL;
}

PWR_Status pwr_thread_create(PWR_Thread** out, PWR_ThreadFn fn, void* arg,
                             const char* name)
{
    PWR_Thread* t;
    if (out == NULL || fn == NULL) { return PWR_ERR_NULL_POINTER; }
    *out = NULL;
    t = (PWR_Thread*)calloc(1u, sizeof(*t));
    if (t == NULL) { return PWR_ERR_OUT_OF_MEMORY; }
    t->fn  = fn;
    t->arg = arg;
    if (pthread_create(&t->handle, NULL, pwr_thread_trampoline, t) != 0)
    {
        free(t);
        return PWR_ERR_THREAD;
    }
    t->started = 1;
#if defined(__linux__) && defined(_GNU_SOURCE)
    if (name != NULL) { (void)pthread_setname_np(t->handle, name); }
#else
    PWR_UNUSED(name);
#endif
    *out = t;
    return PWR_STATUS_OK;
}

void pwr_thread_join(PWR_Thread* t)
{
    if (t == NULL) { return; }
    if (t->started) { (void)pthread_join(t->handle, NULL); }
    free(t);
}

uint32_t pwr_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1u;
}

int32_t pwr_atomic_load_i32(const volatile int32_t* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

void pwr_atomic_store_i32(volatile int32_t* p, int32_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

int32_t pwr_atomic_add_i32(volatile int32_t* p, int32_t delta)
{
    return __atomic_add_fetch(p, delta, __ATOMIC_ACQ_REL);
}

int32_t pwr_atomic_cas_i32(volatile int32_t* p, int32_t expected, int32_t desired)
{
    int32_t exp = expected;
    __atomic_compare_exchange_n(p, &exp, desired, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return exp;
}

uint64_t pwr_atomic_load_u64(const volatile uint64_t* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

void pwr_atomic_store_u64(volatile uint64_t* p, uint64_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

void pwr_atomic_fence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

#endif /* _WIN32 */

/* ==========================================================================
 *  Portable helpers
 * ========================================================================== */
void* pwr_aligned_calloc(size_t count, size_t elem_size, size_t alignment)
{
    size_t bytes;
    void*  p;
    if (elem_size != 0u && count > (SIZE_MAX / elem_size)) { return NULL; }
    bytes = count * elem_size;
    p = pwr_aligned_alloc(bytes, alignment);
    if (p != NULL) { memset(p, 0, (bytes == 0u) ? 1u : bytes); }
    return p;
}
