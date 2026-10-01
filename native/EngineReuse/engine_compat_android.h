/* engine_compat_android.h - macOS/Darwin API stand-ins for the Android (bionic)
 * build of the translated Halo engine host.
 *
 * Everything here is gated on __ANDROID__, so the macOS build sees exactly the
 * system headers it saw before. The lifted engine code and the host shims use
 * three Darwin-only facilities that bionic does not provide:
 *
 *  1. clock_gettime_nsec_np() with CLOCK_UPTIME_RAW / CLOCK_MONOTONIC_RAW.
 *     Bionic has clock_gettime() with CLOCK_MONOTONIC; we pack the result into
 *     nanoseconds. The two raw clock ids collapse onto CLOCK_MONOTONIC: the
 *     engine only ever compares these against each other (deltas, deadlines),
 *     and CLOCK_MONOTONIC is the suspension-safe clock that fits both uses.
 *  2. pthread cancellation. Bionic does not implement cancellation at all; the
 *     host's use of it (threading.c, shims_kernel32.c) is a belt-and-braces
 *     guard around blocking waits, so the no-ops preserve the behaviour that
 *     cancellation was never meant to interrupt anything here.
 *  3. (Not here: CommonCrypto is replaced locally in shims_misc.c.)
 */
#ifndef HALO_ENGINE_COMPAT_ANDROID_H
#define HALO_ENGINE_COMPAT_ANDROID_H

#ifdef __ANDROID__

#include <stdint.h>
#include <time.h>
#include <pthread.h>

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif

/* macOS' fast ns clock, in the same units. Returns 0 on failure, matching
 * the documented -1/error path closely enough for delta-only use. */
static inline uint64_t clock_gettime_nsec_np(clockid_t clock_id) {
    struct timespec ts;
    if (clock_gettime(clock_id, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* Bionic's pthread.h has no cancellation API. The host disables cancellation
 * around lock/wait pairs and re-enables it (with a testcancel) afterwards;
 * with cancellation never enabled, those are pure no-ops. The old state is
 * reported as ENABLED so the surrounding `if (old == ENABLE) testcancel()`
 * logic reads exactly as it does on macOS. */
#ifndef PTHREAD_CANCEL_ENABLE
#define PTHREAD_CANCEL_ENABLE 0
#endif
#ifndef PTHREAD_CANCEL_DISABLE
#define PTHREAD_CANCEL_DISABLE 1
#endif

static inline int pthread_setcancelstate(int state, int *oldstate) {
    if (oldstate) *oldstate = PTHREAD_CANCEL_ENABLE;
    (void)state;
    return 0;
}

static inline void pthread_testcancel(void) { }

/* pthread_cancel is likewise absent. host_thread_terminate() already sets a
 * cooperative terminate_requested flag and broadcasts the thread's condition
 * variable before calling this; the worker observes the flag and returns, so
 * the force-cancel is redundant on bionic. Report success so the caller's
 * `error == 0` path proceeds as on macOS. */
static inline int pthread_cancel(pthread_t thread) { (void)thread; return 0; }

/* macOS' relative-timeout condvar wait. Convert to the absolute deadline
 * pthread_cond_timedwait expects (measured on CLOCK_REALTIME, the clock a
 * default-initialised condvar times against). Used by the KERNEL32 I/O shim's
 * completion wait. */
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *cond,
                                                       pthread_mutex_t *mutex,
                                                       const struct timespec *rel) {
    struct timespec now, deadline;
    clock_gettime(CLOCK_REALTIME, &now);
    deadline.tv_sec = now.tv_sec + rel->tv_sec;
    deadline.tv_nsec = now.tv_nsec + rel->tv_nsec;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(cond, mutex, &deadline);
}

#endif /* __ANDROID__ */
#endif /* HALO_ENGINE_COMPAT_ANDROID_H */
