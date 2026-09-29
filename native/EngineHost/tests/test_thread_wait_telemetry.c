/* Real guest wait paths, with a clock wrapper that deliberately clobbers
 * errno to verify instrumentation transparency, with the core telemetry on
 * and off (host_core_telemetry, HALO_CORE_TELEMETRY). No engine/game files. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static _Atomic unsigned entered_wait;
static _Thread_local unsigned clock_reads;
/* errno as the first and the latest timestamp found it, and as the latest
 * native wait left it. The native wait owns its errno: wait_changed waits in
 * 50 ms slices, and on Darwin a slice that times out can leave errno set
 * (0x13C) although pthread_cond_timedwait returns ETIMEDOUT. The telemetry
 * must hand back exactly what the wait left, whatever that is. */
static _Thread_local int first_clock_errno, last_clock_errno, last_wait_errno;
static uint64_t observed_clock(clockid_t clock) {
    if (!clock_reads) first_clock_errno = errno;
    last_clock_errno = errno;
    uint64_t now = clock_gettime_nsec_np(clock);
    clock_reads++;
    errno = EDOM;
    return now;
}
static int observed_wait(pthread_cond_t *condition, pthread_mutex_t *lock,
                         const struct timespec *deadline) {
    atomic_store(&entered_wait, 1);
    int result = pthread_cond_timedwait(condition, lock, deadline);
    last_wait_errno = errno;
    return result;
}
#define clock_gettime_nsec_np observed_clock
#define pthread_cond_timedwait observed_wait
#include "../threading.c"
#undef pthread_cond_timedwait
#undef clock_gettime_nsec_np

uint8_t *engine_flat_base;
static struct { int kind; void *object; } handles[16];
static unsigned next_handle = 1;
static _Thread_local int check_lookup_errno;

uint32_t host_handle_new(int kind, void *object) {
    assert(next_handle < 16);
    handles[next_handle].kind = kind;
    handles[next_handle].object = object;
    return next_handle++;
}
void *host_handle_object(uint32_t handle, int kind) {
    if (check_lookup_errno) {
        assert(errno == ERANGE || errno == ENOTTY);
        errno = ENOTTY;
    }
    return handle < 16 && handles[handle].kind == kind ? handles[handle].object : NULL;
}
void host_handle_close(uint32_t handle) {
    assert(handle < 16);
    handles[handle].kind = 0;
    handles[handle].object = NULL;
}
void host_log(const char *format, ...) { (void)format; }
_Noreturn void host_exit(int code) { (void)code; abort(); }
int host_initialize_guest_thread(EngineCPU *cpu, uint32_t tid, uint32_t stack) {
    (void)cpu; (void)tid; (void)stack; abort();
}
int host_execute_guest_thread(EngineCPU *cpu, uint32_t start, uint32_t parameter,
                              uint32_t *exit_code) {
    (void)cpu; (void)start; (void)parameter; (void)exit_code; abort();
}

typedef struct {
    uint32_t event;
    int section, wait_errno;
    uint64_t elapsed, calls;
    unsigned clocks;
} Worker;

static void *worker_wait(void *opaque) {
    Worker *worker = opaque;
    HostThread identity = {.tid = 2};
    current_thread = &identity;
    assert(host_thread_wait_ns == 0 && host_thread_waits == 0 && clock_reads == 0);
    errno = ERANGE;
    if (worker->section) {
        host_critical_section_enter(0x1000);
        host_critical_section_leave(0x1000);
    } else {
        assert(host_wait_single(worker->event, UINT32_MAX) == HOST_WAIT_OBJECT_0);
    }
    /* Whatever the blocking wait left, never the clock's EDOM: the start
     * timestamp saw (and restored) the caller's ERANGE, the end timestamp
     * saw (and restored) the wait's own errno. Off, there is no timestamp. */
    int after = errno;
    assert(after == last_wait_errno);
    if (clock_reads) assert(first_clock_errno == ERANGE && last_clock_errno == after);
    worker->wait_errno = after;
    worker->elapsed = host_thread_wait_ns;
    worker->calls = host_thread_waits;
    worker->clocks = clock_reads;
    current_thread = NULL;
    return NULL;
}

/* A worker blocks on an event, or on a critical section this thread owns,
 * for at least hold_ms. Holds past 50 ms always cross a timed-out wait slice,
 * so the errno that slice leaves is exercised every run, not only when the
 * scheduler happens to delay this thread. */
static int check_blocked_worker(uint32_t event, int section, unsigned hold_ms) {
    int timed = host_core_telemetry;
    if (section) host_critical_section_enter(0x1000);   /* uncontended: no clock */
    uint64_t own_ns = host_thread_wait_ns, own_calls = host_thread_waits;
    unsigned own_clocks = clock_reads;
    Worker worker = {.event = event, .section = section};
    pthread_t thread;
    atomic_store(&entered_wait, 0);
    assert(!pthread_create(&thread, NULL, worker_wait, &worker));
    while (!atomic_load(&entered_wait)) usleep(100);
    usleep(hold_ms * 1000u);
    if (section) host_critical_section_leave(0x1000);
    else assert(host_event_set(event));
    assert(!pthread_join(thread, NULL));
    if (timed) assert(worker.elapsed >= hold_ms * 500000ull && worker.calls == 1 && worker.clocks == 2);
    else assert(!worker.elapsed && !worker.calls && !worker.clocks);
    assert(host_thread_wait_ns == own_ns && host_thread_waits == own_calls);
    assert(clock_reads == own_clocks);
    if (section) assert(G32(0x1008) == 0 && G32(0x100C) == 0);
    return worker.wait_errno;
}

int main(void) {
    alarm(10);
    engine_flat_base = calloc(1, 0x2000);
    assert(engine_flat_base);
    uint32_t event = host_event_create(0, 0);
    assert(!host_core_telemetry);   /* off until the app turns it on */
    host_core_telemetry = 1;

    /* Polling neither reads clocks nor changes the original wait result. */
    assert(host_wait_single(event, 0) == HOST_WAIT_TIMEOUT);
    assert(host_wait_single(99, 0) == HOST_WAIT_FAILED);
    assert(host_event_set(event));
    assert(host_wait_single(event, 0) == HOST_WAIT_OBJECT_0);
    assert(host_wait_single(event, 0) == HOST_WAIT_TIMEOUT);
    assert(!host_thread_wait_ns && !host_thread_waits && !clock_reads);

    /* Both incoming errno and the underlying operation's outgoing errno
     * survive clock reads; even immediate non-polling returns are counted. */
    check_lookup_errno = 1;
    errno = ERANGE;
    assert(host_wait_single(99, 100) == HOST_WAIT_FAILED);
    assert(errno == ENOTTY && host_thread_waits == 1 && clock_reads == 2);
    check_lookup_errno = 0;
    assert(host_event_set(event));
    errno = ERANGE;
    assert(host_wait_single(event, 100) == HOST_WAIT_OBJECT_0);
    assert(errno == ERANGE && host_thread_waits == 2 && clock_reads == 4);

    uint64_t before = host_thread_wait_ns;
    assert(host_wait_single(event, 20) == HOST_WAIT_TIMEOUT);
    assert(host_thread_wait_ns - before >= 10000000);
    assert(host_thread_waits == 3 && clock_reads == 6);
    check_blocked_worker(event, 0, 20);
    int slice_errno = check_blocked_worker(event, 0, 120);

    /* Recursion and uncontended ownership keep the clock off the fast path. */
    host_critical_section_initialize(0x1000);
    host_critical_section_enter(0x1000);
    host_critical_section_enter(0x1000);
    assert(G32(0x1008) == 2 && G32(0x100C) == 1);
    host_critical_section_leave(0x1000);
    host_critical_section_leave(0x1000);
    assert(G32(0x1008) == 0 && G32(0x100C) == 0);
    assert(host_thread_waits == 3 && clock_reads == 6);
    check_blocked_worker(event, 1, 20);
    check_blocked_worker(event, 1, 120);

    /* Off: the same waits, results and errno, and not one clock read or count. */
    host_core_telemetry = 0;
    uint64_t off_ns = host_thread_wait_ns, off_calls = host_thread_waits;
    unsigned off_clocks = clock_reads;
    errno = ERANGE;
    assert(host_wait_single(event, 20) == HOST_WAIT_TIMEOUT);
    assert(host_event_set(event));
    errno = ERANGE;
    assert(host_wait_single(event, 100) == HOST_WAIT_OBJECT_0 && errno == ERANGE);
    assert(host_thread_wait_ns == off_ns && host_thread_waits == off_calls && clock_reads == off_clocks);
    check_blocked_worker(event, 0, 20);
    check_blocked_worker(event, 1, 120);
    host_critical_section_delete(0x1000);

    HostEvent *event_object = host_handle_object(event, HANDLE_EVENT);
    assert(!pthread_cond_destroy(&event_object->changed));
    assert(!pthread_mutex_destroy(&event_object->lock));
    free(event_object);
    HostCriticalSection *section_object = critical_sections;
    assert(section_object && !section_object->next);
    assert(!pthread_cond_destroy(&section_object->changed));
    assert(!pthread_mutex_destroy(&section_object->lock));
    free(section_object);
    free(engine_flat_base);
    printf("PASS guest wait telemetry: polling, results, timeout, wakeup, TLS, errno (a timed-out slice left %d), "
           "critical sections; off reads no clock\n", slice_errno);
    return 0;
}
