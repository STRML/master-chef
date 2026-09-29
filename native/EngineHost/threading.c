#include "host.h"
#include "threading.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int manual_reset;
    int signaled;
} HostEvent;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint32_t owner;
    unsigned recursion;
} HostMutex;

typedef struct HostThread {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t native;
    EngineCPU cpu;
    uint32_t tid;
    uint32_t start_address;
    uint32_t parameter;
    uint32_t exit_code;
    unsigned suspend_count;
    int done;
    int terminate_requested;
} HostThread;

typedef struct HostCriticalSection {
    uint32_t guest_address;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint32_t owner;
    unsigned recursion;
    struct HostCriticalSection *next;
} HostCriticalSection;

static pthread_mutex_t critical_sections_lock = PTHREAD_MUTEX_INITIALIZER;
static HostCriticalSection *critical_sections;
static uint32_t next_tid = 2;
static _Thread_local HostThread *current_thread;

static void deadline_from_now(struct timespec *deadline, uint32_t milliseconds) {
    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += milliseconds / 1000u;
    deadline->tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

static int wait_changed(pthread_cond_t *changed, pthread_mutex_t *lock,
                        uint32_t timeout_ms, const struct timespec *deadline) {
    /* On Darwin, a signal/cancel race in a shared condition variable can run
     * cancellation cleanup without reacquiring its mutex. Never cancel inside
     * the native wait: use bounded slices, then deliver pending cancellation
     * only after unlocking. Callers recheck their predicates after relocking.
     * Keep the caller's absolute deadline; a polling slice is not a timeout. */
    struct timespec slice;
    deadline_from_now(&slice, 50);
    int final_slice = timeout_ms != 0xFFFFFFFFu &&
        (deadline->tv_sec < slice.tv_sec ||
         (deadline->tv_sec == slice.tv_sec && deadline->tv_nsec <= slice.tv_nsec));
    if (final_slice) slice = *deadline;
    int old_cancel_state;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);
    int error = pthread_cond_timedwait(changed, lock, &slice);
    pthread_mutex_unlock(lock);
    pthread_setcancelstate(old_cancel_state, NULL);
    if (old_cancel_state == PTHREAD_CANCEL_ENABLE) pthread_testcancel();
    pthread_mutex_lock(lock);
    return error == ETIMEDOUT && !final_slice ? 0 : error;
}

uint32_t host_event_create(int manual_reset, int initial_state) {
    HostEvent *event = calloc(1, sizeof *event);
    if (!event) return 0;
    pthread_mutex_init(&event->lock, NULL);
    pthread_cond_init(&event->changed, NULL);
    event->manual_reset = !!manual_reset;
    event->signaled = !!initial_state;
    return host_handle_new(HANDLE_EVENT, event);
}

int host_event_set(uint32_t handle) {
    HostEvent *event = host_handle_object(handle, HANDLE_EVENT);
    if (!event) return 0;
    pthread_mutex_lock(&event->lock);
    event->signaled = 1;
    if (event->manual_reset) pthread_cond_broadcast(&event->changed);
    else pthread_cond_signal(&event->changed);
    pthread_mutex_unlock(&event->lock);
    return 1;
}

static uint32_t event_wait(HostEvent *event, uint32_t timeout_ms) {
    struct timespec deadline;
    if (timeout_ms != 0xFFFFFFFFu) deadline_from_now(&deadline, timeout_ms);
    pthread_mutex_lock(&event->lock);
    while (!event->signaled) {
        if (!timeout_ms) {
            pthread_mutex_unlock(&event->lock);
            return HOST_WAIT_TIMEOUT;
        }
        int error = wait_changed(&event->changed, &event->lock, timeout_ms, &deadline);
        if (error == ETIMEDOUT) {
            pthread_mutex_unlock(&event->lock);
            return HOST_WAIT_TIMEOUT;
        }
        if (error) {
            pthread_mutex_unlock(&event->lock);
            return HOST_WAIT_FAILED;
        }
    }
    if (!event->manual_reset) event->signaled = 0;
    pthread_mutex_unlock(&event->lock);
    return HOST_WAIT_OBJECT_0;
}

uint32_t host_mutex_create(int initial_owner) {
    HostMutex *mutex = calloc(1, sizeof *mutex);
    if (!mutex) return 0;
    pthread_mutex_init(&mutex->lock, NULL);
    pthread_cond_init(&mutex->changed, NULL);
    if (initial_owner) {
        mutex->owner = host_current_thread_id();
        mutex->recursion = 1;
    }
    return host_handle_new(HANDLE_MUTEX, mutex);
}

static uint32_t mutex_wait(HostMutex *mutex, uint32_t timeout_ms) {
    uint32_t tid = host_current_thread_id();
    struct timespec deadline;
    if (timeout_ms != 0xFFFFFFFFu) deadline_from_now(&deadline, timeout_ms);
    pthread_mutex_lock(&mutex->lock);
    while (mutex->owner && mutex->owner != tid) {
        if (!timeout_ms) {
            pthread_mutex_unlock(&mutex->lock);
            return HOST_WAIT_TIMEOUT;
        }
        int error = wait_changed(&mutex->changed, &mutex->lock, timeout_ms, &deadline);
        if (error == ETIMEDOUT) {
            pthread_mutex_unlock(&mutex->lock);
            return HOST_WAIT_TIMEOUT;
        }
        if (error) {
            pthread_mutex_unlock(&mutex->lock);
            return HOST_WAIT_FAILED;
        }
    }
    mutex->owner = tid;
    mutex->recursion++;
    pthread_mutex_unlock(&mutex->lock);
    return HOST_WAIT_OBJECT_0;
}

int host_mutex_release(uint32_t handle) {
    HostMutex *mutex = host_handle_object(handle, HANDLE_MUTEX);
    uint32_t tid = host_current_thread_id();
    if (!mutex) return 0;
    pthread_mutex_lock(&mutex->lock);
    if (mutex->owner != tid || !mutex->recursion) {
        pthread_mutex_unlock(&mutex->lock);
        return 0;
    }
    if (!--mutex->recursion) {
        mutex->owner = 0;
        pthread_cond_signal(&mutex->changed);
    }
    pthread_mutex_unlock(&mutex->lock);
    return 1;
}

static void thread_mark_done(HostThread *thread, uint32_t exit_code) {
    pthread_mutex_lock(&thread->lock);
    if (!thread->done) {
        thread->done = 1;
        if (!thread->terminate_requested) thread->exit_code = exit_code;
        pthread_cond_broadcast(&thread->changed);
    }
    pthread_mutex_unlock(&thread->lock);
}

static void thread_cancel_cleanup(void *opaque) {
    HostThread *thread = opaque;
    thread_mark_done(thread, 0);
}

static void *thread_start(void *opaque) {
    HostThread *thread = opaque;
    current_thread = thread;
    pthread_cleanup_push(thread_cancel_cleanup, thread);
    pthread_mutex_lock(&thread->lock);
    while (thread->suspend_count && !thread->terminate_requested)
        wait_changed(&thread->changed, &thread->lock, 0xFFFFFFFFu, NULL);
    int terminate = thread->terminate_requested;
    uint32_t termination_code = thread->exit_code;
    pthread_mutex_unlock(&thread->lock);
    if (!terminate) {
        host_log("thread %u entering guest %08X with TEB %08X",
                 thread->tid, thread->start_address, thread->cpu.fs_base);
        uint32_t exit_code = 1;
        int clean = host_execute_guest_thread(&thread->cpu, thread->start_address,
                                              thread->parameter, &exit_code);
        thread_mark_done(thread, exit_code);
        host_log("thread %u left guest %08X with code %08X%s", thread->tid,
                 thread->start_address, exit_code, clean ? "" : " after failure");
    } else {
        thread_mark_done(thread, termination_code);
    }
    pthread_cleanup_pop(0);
    return NULL;
}

/* The quality of service a guest thread runs at.
 *
 * Halo makes two threads of its own: the cache file's reader (00443940),
 * which the engine thread spins on in 00443E10 and 00444550 whenever
 * something it needs this frame has not been read yet, and the saved-game
 * writer (00538980). Created without attributes they ran at DEFAULT, below
 * the USER_INTERACTIVE engine thread that waits for them and below the 90 Hz
 * compositor, free to be left on an efficiency core behind both while the
 * game stood still for a read. Darwin does not pass a creator's class to a
 * thread made with no attributes. On Windows both start at the process's
 * normal priority, the same as the thread that made them, and inheriting the
 * creator's class is that arrangement here. HALO_GUEST_THREAD_QOS names a
 * class instead: interactive, initiated, default or utility, or off for the
 * old thread without attributes. */
static uint32_t guest_thread_qos_applied, guest_threads_created;
uint32_t host_guest_thread_qos(void) { return __atomic_load_n(&guest_thread_qos_applied, __ATOMIC_RELAXED); }
uint32_t host_guest_threads(void) { return __atomic_load_n(&guest_threads_created, __ATOMIC_RELAXED); }
#if defined(__APPLE__)
static qos_class_t guest_thread_qos(void) {
    const char *option = getenv("HALO_GUEST_THREAD_QOS");
    if (!option || !option[0] || !strcmp(option, "inherit")) return qos_class_self();
    if (!strcmp(option, "interactive")) return QOS_CLASS_USER_INTERACTIVE;
    if (!strcmp(option, "initiated")) return QOS_CLASS_USER_INITIATED;
    if (!strcmp(option, "default")) return QOS_CLASS_DEFAULT;
    if (!strcmp(option, "utility")) return QOS_CLASS_UTILITY;
    return QOS_CLASS_UNSPECIFIED;   /* off, or anything unrecognised */
}
#endif

uint32_t host_thread_create(uint32_t stack_size, uint32_t start_address,
                            uint32_t parameter, uint32_t flags,
                            uint32_t *thread_id) {
    HostThread *thread = calloc(1, sizeof *thread);
    if (!thread) return 0;
    pthread_mutex_init(&thread->lock, NULL);
    pthread_cond_init(&thread->changed, NULL);
    thread->tid = __atomic_fetch_add(&next_tid, 1u, __ATOMIC_RELAXED);
    thread->start_address = start_address;
    thread->parameter = parameter;
    thread->exit_code = HOST_STILL_ACTIVE;
    thread->suspend_count = (flags & HOST_CREATE_SUSPENDED) ? 1u : 0u;
    if (!host_initialize_guest_thread(&thread->cpu, thread->tid, stack_size)) {
        free(thread);
        return 0;
    }
    uint32_t handle = host_handle_new(HANDLE_THREAD, thread);
    uint32_t qos = 0;
    int error = -1;
#if defined(__APPLE__)
    qos_class_t wanted = guest_thread_qos();
    pthread_attr_t attr;
    if (wanted != QOS_CLASS_UNSPECIFIED && !pthread_attr_init(&attr)) {
        if (!pthread_attr_set_qos_class_np(&attr, wanted, 0)) {
            error = pthread_create(&thread->native, &attr, thread_start, thread);
            if (!error) qos = (uint32_t)wanted;
        }
        pthread_attr_destroy(&attr);
    }
#endif
    /* No class asked for, or the attributes were refused: the old thread. */
    if (error) error = pthread_create(&thread->native, NULL, thread_start, thread);
    if (error) {
        host_handle_close(handle);
        free(thread);
        return 0;
    }
    pthread_detach(thread->native);
    if (thread_id) *thread_id = thread->tid;
    __atomic_store_n(&guest_thread_qos_applied, qos, __ATOMIC_RELAXED);
    __atomic_fetch_add(&guest_threads_created, 1u, __ATOMIC_RELAXED);
    host_log("CreateThread(start %08X, param %08X) -> handle %08X tid %u%s qos 0x%02X",
             start_address, parameter, handle, thread->tid,
             (flags & HOST_CREATE_SUSPENDED) ? " suspended" : "", qos);
    return handle;
}

uint32_t host_thread_resume(uint32_t handle) {
    HostThread *thread = host_handle_object(handle, HANDLE_THREAD);
    if (!thread) return 0xFFFFFFFFu;
    pthread_mutex_lock(&thread->lock);
    uint32_t previous = thread->suspend_count;
    if (thread->suspend_count) thread->suspend_count--;
    if (!thread->suspend_count) pthread_cond_broadcast(&thread->changed);
    pthread_mutex_unlock(&thread->lock);
    return previous;
}

int host_thread_terminate(uint32_t handle, uint32_t exit_code) {
    HostThread *thread = host_handle_object(handle, HANDLE_THREAD);
    if (!thread) return 0;
    pthread_mutex_lock(&thread->lock);
    if (thread->done) {
        pthread_mutex_unlock(&thread->lock);
        return 0;
    }
    thread->terminate_requested = 1;
    thread->exit_code = exit_code;
    pthread_cond_broadcast(&thread->changed);
    /* Keep completion from publishing done and retiring the detached native
     * thread between our done check and pthread_cancel's use of its ID. */
    int error = pthread_cancel(thread->native);
    pthread_mutex_unlock(&thread->lock);
    return error == 0;
}

int host_thread_get_exit_code(uint32_t handle, uint32_t *exit_code) {
    HostThread *thread = host_handle_object(handle, HANDLE_THREAD);
    if (!thread || !exit_code) return 0;
    pthread_mutex_lock(&thread->lock);
    *exit_code = thread->done ? thread->exit_code : HOST_STILL_ACTIVE;
    pthread_mutex_unlock(&thread->lock);
    return 1;
}

static uint32_t thread_wait(HostThread *thread, uint32_t timeout_ms) {
    struct timespec deadline;
    if (timeout_ms != 0xFFFFFFFFu) deadline_from_now(&deadline, timeout_ms);
    pthread_mutex_lock(&thread->lock);
    while (!thread->done) {
        if (!timeout_ms) {
            pthread_mutex_unlock(&thread->lock);
            return HOST_WAIT_TIMEOUT;
        }
        int error = wait_changed(&thread->changed, &thread->lock, timeout_ms, &deadline);
        if (error == ETIMEDOUT) {
            pthread_mutex_unlock(&thread->lock);
            return HOST_WAIT_TIMEOUT;
        }
        if (error) {
            pthread_mutex_unlock(&thread->lock);
            return HOST_WAIT_FAILED;
        }
    }
    pthread_mutex_unlock(&thread->lock);
    return HOST_WAIT_OBJECT_0;
}

_Thread_local uint64_t host_thread_wait_ns, host_thread_waits;
int host_core_telemetry;
static int wait_timed(void) { return __atomic_load_n(&host_core_telemetry, __ATOMIC_RELAXED); }
static uint64_t wait_clock_ns(void) {
    int saved_errno = errno;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    errno = saved_errno;
    return now;
}
static void record_wait(uint64_t start) {
    uint64_t end = wait_clock_ns();
    if (start && end >= start) host_thread_wait_ns += end - start;
    host_thread_waits++;
}
static uint32_t wait_single(uint32_t handle, uint32_t timeout_ms) {
    HostEvent *event = host_handle_object(handle, HANDLE_EVENT);
    if (event) return event_wait(event, timeout_ms);
    HostMutex *mutex = host_handle_object(handle, HANDLE_MUTEX);
    if (mutex) return mutex_wait(mutex, timeout_ms);
    HostThread *thread = host_handle_object(handle, HANDLE_THREAD);
    if (thread) return thread_wait(thread, timeout_ms);
    return HOST_WAIT_FAILED;
}
/* A zero timeout only polls. Measure the elapsed call for other timeouts,
 * including calls that return immediately; this is not exact off-core time.
 * Timestamp reads must not alter the underlying call's errno. With the core
 * telemetry off, nothing is measured. */
uint32_t host_wait_single(uint32_t handle, uint32_t timeout_ms) {
    if (!timeout_ms || !wait_timed()) return wait_single(handle, timeout_ms);
    uint64_t start = wait_clock_ns();
    uint32_t result = wait_single(handle, timeout_ms);
    record_wait(start);
    return result;
}

uint32_t host_current_thread_id(void) {
    return current_thread ? current_thread->tid : 1u;
}

void host_thread_yield(void) {
    pthread_testcancel();
    sched_yield();
}

_Noreturn void host_thread_exit(uint32_t exit_code) {
    if (!current_thread) host_exit((int)exit_code);
    thread_mark_done(current_thread, exit_code);
    pthread_exit(NULL);
}

static HostCriticalSection *critical_section_find(uint32_t guest_address, int create) {
    pthread_mutex_lock(&critical_sections_lock);
    HostCriticalSection *section = critical_sections;
    while (section && section->guest_address != guest_address) section = section->next;
    if (!section && create) {
        section = calloc(1, sizeof *section);
        if (section) {
            section->guest_address = guest_address;
            pthread_mutex_init(&section->lock, NULL);
            pthread_cond_init(&section->changed, NULL);
            section->next = critical_sections;
            critical_sections = section;
        }
    }
    pthread_mutex_unlock(&critical_sections_lock);
    return section;
}

void host_critical_section_initialize(uint32_t guest_address) {
    HostCriticalSection *section = critical_section_find(guest_address, 1);
    if (!section) host_exit(3);
    memset(GPTR(guest_address), 0, 24);
    S32(guest_address + 4, 0xFFFFFFFFu);
}

void host_critical_section_delete(uint32_t guest_address) {
    HostCriticalSection *section = critical_section_find(guest_address, 0);
    if (!section) return;
    pthread_mutex_lock(&section->lock);
    if (!section->owner) {
        S32(guest_address + 4, 0);
        S32(guest_address + 8, 0);
        S32(guest_address + 12, 0);
    }
    pthread_mutex_unlock(&section->lock);
}

void host_critical_section_enter(uint32_t guest_address) {
    HostCriticalSection *section = critical_section_find(guest_address, 1);
    if (!section) host_exit(3);
    uint32_t tid = host_current_thread_id();
    pthread_mutex_lock(&section->lock);
    /* Only contention is timed, and only with the core telemetry on. */
    int timed = section->owner && section->owner != tid && wait_timed();
    uint64_t start = timed ? wait_clock_ns() : 0;
    while (section->owner && section->owner != tid)
        wait_changed(&section->changed, &section->lock, 0xFFFFFFFFu, NULL);
    if (timed) record_wait(start);
    section->owner = tid;
    section->recursion++;
    S32(guest_address + 4, section->recursion - 1);
    S32(guest_address + 8, section->recursion);
    S32(guest_address + 12, tid);
    pthread_mutex_unlock(&section->lock);
}

void host_critical_section_leave(uint32_t guest_address) {
    HostCriticalSection *section = critical_section_find(guest_address, 0);
    uint32_t tid = host_current_thread_id();
    if (!section) return;
    pthread_mutex_lock(&section->lock);
    if (section->owner == tid && section->recursion) {
        section->recursion--;
        S32(guest_address + 8, section->recursion);
        if (!section->recursion) {
            section->owner = 0;
            S32(guest_address + 4, 0xFFFFFFFFu);
            S32(guest_address + 12, 0);
            pthread_cond_signal(&section->changed);
        } else {
            S32(guest_address + 4, section->recursion - 1);
        }
    }
    pthread_mutex_unlock(&section->lock);
}
