/* Production waits/cancellation; native threads stay joinable only in this
 * fixture so cancellation must also complete pthread_join.
 * clang -O2 -I../EngineReuse tests/test_thread_cancellation.c -o /tmp/test_thread_cancellation
 */
#include <pthread.h>
#include <stdatomic.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
static _Atomic(uintptr_t) entered_wait;
static int observed_wait(pthread_cond_t *cv, pthread_mutex_t *lock) {
    atomic_store(&entered_wait, (uintptr_t)cv);
    return pthread_cond_wait(cv, lock);
}
static int observed_timedwait(pthread_cond_t *cv, pthread_mutex_t *lock, const struct timespec *deadline) {
    atomic_store(&entered_wait, (uintptr_t)cv);
    return pthread_cond_timedwait(cv, lock, deadline);
}
static int keep_joinable(pthread_t thread) { (void)thread; return 0; }
#define pthread_cond_wait observed_wait
#define pthread_cond_timedwait observed_timedwait
#define pthread_detach keep_joinable
#include "../threading.c"
#undef pthread_cond_wait
#undef pthread_cond_timedwait
#undef pthread_detach
uint8_t *engine_flat_base;
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind; void *object; } handles[64];
static _Atomic unsigned guest_entries;
uint32_t host_handle_new(int kind, void *object) {
    pthread_mutex_lock(&handles_lock);
    uint32_t h;
    for (h = 1; h < 64 && handles[h].kind; ++h) {}
    assert(h < 64);
    handles[h].kind = kind; handles[h].object = object;
    pthread_mutex_unlock(&handles_lock);
    return h;
}
void *host_handle_object(uint32_t h, int kind) {
    pthread_mutex_lock(&handles_lock);
    void *object = h < 64 && handles[h].kind == kind ? handles[h].object : NULL;
    pthread_mutex_unlock(&handles_lock);
    return object;
}
void host_handle_close(uint32_t h) {
    pthread_mutex_lock(&handles_lock);
    handles[h].kind = 0; handles[h].object = NULL;
    pthread_mutex_unlock(&handles_lock);
}
void host_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void host_exit(int code) { fprintf(stderr, "unexpected host_exit %d\n", code); abort(); }
int host_initialize_guest_thread(EngineCPU *cpu, uint32_t tid, uint32_t stack) {
    (void)tid; (void)stack; memset(cpu, 0, sizeof *cpu); return 1;
}
enum { WAIT_FOREVER = 1, WAIT_TIMED, ENTER_SECTION, EXIT_EXPLICIT, RETURN_NORMAL, YIELD_FOREVER };
int host_execute_guest_thread(EngineCPU *cpu, uint32_t start, uint32_t parameter, uint32_t *exit_code) {
    (void)cpu;
    atomic_fetch_add(&guest_entries, 1);
    switch (start) {
        case WAIT_FOREVER: host_wait_single(parameter, UINT32_MAX); break;
        case WAIT_TIMED: host_wait_single(parameter, 60000); break;
        case ENTER_SECTION: host_critical_section_enter(parameter); host_critical_section_leave(parameter); break;
        case EXIT_EXPLICIT: host_thread_exit(parameter);
        case YIELD_FOREVER: for (;;) host_thread_yield();
        default: break;
    }
    *exit_code = parameter;
    return 1;
}
static void await_wait(pthread_cond_t *changed) {
    for (unsigned n = 0; n < 5000; n++) {
        if (atomic_load(&entered_wait) == (uintptr_t)changed) return;
        usleep(1000);
    }
    assert(!"thread did not reach condition wait within five seconds");
}
static void dispose_thread(uint32_t h, uint32_t expected_code) {
    HostThread *thread = host_handle_object(h, HANDLE_THREAD);
    assert(thread);
    assert(host_wait_single(h, 3000) == HOST_WAIT_OBJECT_0);
    uint32_t code = 0;
    assert(host_thread_get_exit_code(h, &code) && code == expected_code);
    assert(!pthread_join(thread->native, NULL));
    assert(!host_thread_terminate(h, 999));
    assert(!pthread_mutex_trylock(&thread->lock));
    pthread_mutex_unlock(&thread->lock);
    host_handle_close(h);
    assert(!pthread_cond_destroy(&thread->changed));
    assert(!pthread_mutex_destroy(&thread->lock));
    free(thread);
}
static void cancel_waiter(uint32_t start, uint32_t parameter, pthread_cond_t *changed) {
    atomic_store(&entered_wait, 0);
    uint32_t h = host_thread_create(0, start, parameter, 0, NULL);
    assert(h);
    await_wait(changed);
    assert(host_thread_terminate(h, 0xB69));
    dispose_thread(h, 0xB69);
}
int main(void) {
    alarm(30); /* Old-code deadlocked joins/locks fail boundedly. */
    engine_flat_base = calloc(1, 0x2000); assert(engine_flat_base);
    uint32_t event_h = host_event_create(0, 0);
    uint32_t mutex_h = host_mutex_create(1);
    HostEvent *event = host_handle_object(event_h, HANDLE_EVENT);
    HostMutex *mutex = host_handle_object(mutex_h, HANDLE_MUTEX);
    host_critical_section_initialize(0x1000);
    HostCriticalSection *section = critical_section_find(0x1000, 0);
    host_critical_section_enter(0x1000);
    for (unsigned cycle = 0; cycle < 40; cycle++) {
        unsigned entries = atomic_load(&guest_entries);
        atomic_store(&entered_wait, 0);
        uint32_t suspended = host_thread_create(0, RETURN_NORMAL, 12, HOST_CREATE_SUSPENDED, NULL);
        HostThread *thread = host_handle_object(suspended, HANDLE_THREAD);
        await_wait(&thread->changed);
        assert(host_thread_terminate(suspended, 0x69));
        dispose_thread(suspended, 0x69);
        assert(atomic_load(&guest_entries) == entries);
        cancel_waiter(WAIT_FOREVER, event_h, &event->changed);
        cancel_waiter(WAIT_TIMED, event_h, &event->changed);
        assert(host_event_set(event_h));
        assert(host_wait_single(event_h, 0) == HOST_WAIT_OBJECT_0);
        assert(host_wait_single(event_h, 0) == HOST_WAIT_TIMEOUT);
        cancel_waiter(WAIT_FOREVER, mutex_h, &mutex->changed);
        assert(host_mutex_release(mutex_h));
        assert(host_wait_single(mutex_h, 0) == HOST_WAIT_OBJECT_0);
        atomic_store(&entered_wait, 0);
        uint32_t target_h = host_thread_create(0, RETURN_NORMAL, 13, HOST_CREATE_SUSPENDED, NULL);
        HostThread *target = host_handle_object(target_h, HANDLE_THREAD);
        await_wait(&target->changed);
        cancel_waiter(WAIT_TIMED, target_h, &target->changed);
        assert(host_thread_resume(target_h) == 1);
        dispose_thread(target_h, 13);
        cancel_waiter(ENTER_SECTION, 0x1000, &section->changed);
        host_critical_section_leave(0x1000);
        host_critical_section_enter(0x1000);
    }
    uint32_t explicit_h = host_thread_create(0, EXIT_EXPLICIT, 42, 0, NULL);
    dispose_thread(explicit_h, 42);
    uint32_t yielding = host_thread_create(0, YIELD_FOREVER, 0, 0, NULL);
    assert(host_thread_terminate(yielding, 43));
    dispose_thread(yielding, 43);
    /* Internal cancellation polling must not become a guest-visible timeout. */
    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    assert(host_wait_single(event_h, 125) == HOST_WAIT_TIMEOUT);
    clock_gettime(CLOCK_MONOTONIC, &end);
    int64_t elapsed_ns = (int64_t)(end.tv_sec - begin.tv_sec) * 1000000000LL +
                         end.tv_nsec - begin.tv_nsec;
    assert(elapsed_ns >= 120000000LL && elapsed_ns < 3000000000LL);
    host_critical_section_leave(0x1000);
    host_critical_section_delete(0x1000);
    critical_sections = NULL;
    pthread_cond_destroy(&section->changed); pthread_mutex_destroy(&section->lock); free(section);
    host_mutex_release(mutex_h);
    host_handle_close(event_h); host_handle_close(mutex_h);
    pthread_cond_destroy(&event->changed); pthread_mutex_destroy(&event->lock); free(event);
    pthread_cond_destroy(&mutex->changed); pthread_mutex_destroy(&mutex->lock); free(mutex);
    free(engine_flat_base);
    puts("PASS: suspended/event/timed/mutex/thread/critical-section cancellation, bounded wait and join, lock reuse, explicit exit, resume and yielding termination.");
}
