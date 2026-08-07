/* ==========================================================================
 *  PWRadarSystem - PWRadarCore (internal)
 *  ------------------------------------------------------------------------
 *  File     : pwr_platform.h
 *  Purpose  : The single place where operating-system dependencies live.
 *             Threads, mutexes, atomics, monotonic clock, aligned memory.
 *             Nothing else in PWRadarCore may include <windows.h>/<pthread.h>.
 *
 *  Language : ISO C17
 * ========================================================================== */
#ifndef PWRADAR_PWR_PLATFORM_H
#define PWRADAR_PWR_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "pwradar/pwr_status.h"

/* --------------------------------------------------------------------------
 *  Compiler portability shims
 * ------------------------------------------------------------------------ */
#if defined(_MSC_VER)
#  define PWR_INLINE        __inline
#  define PWR_FORCEINLINE   __forceinline
#  define PWR_RESTRICT      __restrict
#  define PWR_ALIGN(n)      __declspec(align(n))
#  define PWR_LIKELY(x)     (x)
#  define PWR_UNLIKELY(x)   (x)
#  define PWR_UNUSED(x)     ((void)(x))
#else
#  define PWR_INLINE        inline
#  define PWR_FORCEINLINE   inline __attribute__((always_inline))
#  define PWR_RESTRICT      __restrict__
#  define PWR_ALIGN(n)      __attribute__((aligned(n)))
#  define PWR_LIKELY(x)     __builtin_expect(!!(x), 1)
#  define PWR_UNLIKELY(x)   __builtin_expect(!!(x), 0)
#  define PWR_UNUSED(x)     ((void)(x))
#endif

/* Cache-line size assumed for false-sharing padding. */
#define PWR_CACHELINE   64u
/* Alignment used for every bulk signal buffer (AVX-512 friendly). */
#define PWR_SIMD_ALIGN  64u

/* --------------------------------------------------------------------------
 *  Aligned allocation
 * ------------------------------------------------------------------------ */
void*  pwr_aligned_alloc(size_t bytes, size_t alignment);
void*  pwr_aligned_calloc(size_t count, size_t elem_size, size_t alignment);
void   pwr_aligned_free(void* p);

/* Convenience wrappers used throughout the DSP code. */
#define PWR_ALLOC_ARRAY(type, n)                                              \
    (type*)pwr_aligned_calloc((size_t)(n), sizeof(type), PWR_SIMD_ALIGN)
#define PWR_FREE(p)  do { pwr_aligned_free((void*)(p)); (p) = NULL; } while (0)

/* --------------------------------------------------------------------------
 *  Monotonic clock
 * ------------------------------------------------------------------------ */
double pwr_plat_now_s(void);          /* monotonic, seconds, ~ns resolution  */
void   pwr_plat_sleep_s(double s);    /* coarse sleep, >= requested          */
void   pwr_plat_yield(void);

/* --------------------------------------------------------------------------
 *  Mutex (non-recursive, no allocation)
 * ------------------------------------------------------------------------ */
typedef struct PWR_Mutex
{
    /* Opaque storage large enough for CRITICAL_SECTION (40 B on x64) and
     * pthread_mutex_t (40 B on glibc x86-64).  Aligned to a pointer.        */
    union { void* align; unsigned char raw[64]; } storage;
    int32_t initialised;
} PWR_Mutex;

PWR_Status pwr_mutex_init(PWR_Mutex* m);
void       pwr_mutex_destroy(PWR_Mutex* m);
void       pwr_mutex_lock(PWR_Mutex* m);
/** Non-blocking acquire.  Returns 1 when the lock was taken, 0 otherwise. */
int        pwr_mutex_trylock(PWR_Mutex* m);
void       pwr_mutex_unlock(PWR_Mutex* m);

/* --------------------------------------------------------------------------
 *  Condition variable (used to park the worker while paused)
 * ------------------------------------------------------------------------ */
typedef struct PWR_Cond
{
    union { void* align; unsigned char raw[64]; } storage;
    int32_t initialised;
} PWR_Cond;

PWR_Status pwr_cond_init(PWR_Cond* c);
void       pwr_cond_destroy(PWR_Cond* c);
void       pwr_cond_wait(PWR_Cond* c, PWR_Mutex* m);
void       pwr_cond_wait_timed(PWR_Cond* c, PWR_Mutex* m, double timeout_s);
void       pwr_cond_signal(PWR_Cond* c);
void       pwr_cond_broadcast(PWR_Cond* c);

/* --------------------------------------------------------------------------
 *  Thread
 * ------------------------------------------------------------------------ */
typedef struct PWR_Thread PWR_Thread;
typedef void (*PWR_ThreadFn)(void* arg);

PWR_Status pwr_thread_create(PWR_Thread** out, PWR_ThreadFn fn, void* arg,
                             const char* name);
void       pwr_thread_join(PWR_Thread* t);   /* also frees the handle        */
uint32_t   pwr_cpu_count(void);

/* --------------------------------------------------------------------------
 *  Relaxed / acquire-release atomics on 32- and 64-bit words.
 *  Implemented with intrinsics rather than <stdatomic.h> because MSVC's C
 *  atomics support is still gated behind /experimental:c11atomics.
 * ------------------------------------------------------------------------ */
int32_t  pwr_atomic_load_i32(const volatile int32_t* p);
void     pwr_atomic_store_i32(volatile int32_t* p, int32_t v);
int32_t  pwr_atomic_add_i32(volatile int32_t* p, int32_t delta);
int32_t  pwr_atomic_cas_i32(volatile int32_t* p, int32_t expected, int32_t desired);

uint64_t pwr_atomic_load_u64(const volatile uint64_t* p);
void     pwr_atomic_store_u64(volatile uint64_t* p, uint64_t v);

/* Full barrier, both compiler and hardware. */
void     pwr_atomic_fence(void);

#endif /* PWRADAR_PWR_PLATFORM_H */
