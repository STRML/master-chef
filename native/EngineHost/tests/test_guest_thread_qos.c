/* Guest threads take their creator's QoS class, or the one
 * HALO_GUEST_THREAD_QOS names; "off" is the old thread without attributes.
 * clang -O2 -pthread -I native/EngineHost -I native/EngineReuse \
 *   native/EngineHost/tests/test_guest_thread_qos.c -o /tmp/guest-thread-qos */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "../threading.c"
uint8_t *engine_flat_base;
static pthread_mutex_t handles_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind; void *object; } handles[64];
static _Atomic uint32_t guest_class;
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
/* The guest body reports the class it is running at. */
int host_execute_guest_thread(EngineCPU *cpu, uint32_t start, uint32_t parameter, uint32_t *exit_code) {
    (void)cpu; (void)start;
    atomic_store(&guest_class, (uint32_t)qos_class_self());
    *exit_code = parameter;
    return 1;
}

typedef struct { const char *option; uint32_t flags; qos_class_t seen; uint32_t applied; } Case;
static void *creator(void *opaque) {
    Case *c = opaque;
    if (c->option) setenv("HALO_GUEST_THREAD_QOS", c->option, 1); else unsetenv("HALO_GUEST_THREAD_QOS");
    atomic_store(&guest_class, 0);
    uint32_t tid = 0, before = host_guest_threads();
    uint32_t h = host_thread_create(0, 0x00443940u, 7, c->flags, &tid);
    assert(h && tid && host_guest_threads() == before + 1);
    if (c->flags & HOST_CREATE_SUSPENDED) {
        usleep(20000);
        assert(atomic_load(&guest_class) == 0);   /* still held */
        assert(host_thread_resume(h) == 1);
    }
    assert(host_wait_single(h, 5000) == HOST_WAIT_OBJECT_0);
    uint32_t code = 0;
    assert(host_thread_get_exit_code(h, &code) && code == 7);
    c->seen = (qos_class_t)atomic_load(&guest_class);
    c->applied = host_guest_thread_qos();
    host_handle_close(h);
    return NULL;
}
/* Runs one creation from a thread of class `from`, as the engine worker is. */
static Case run(qos_class_t from, const char *option, uint32_t flags) {
    Case c = { option, flags, QOS_CLASS_UNSPECIFIED, 0xFFFFFFFFu };
    pthread_attr_t attr; assert(!pthread_attr_init(&attr));
    assert(!pthread_attr_set_qos_class_np(&attr, from, 0));
    pthread_t t; assert(!pthread_create(&t, &attr, creator, &c));
    pthread_attr_destroy(&attr);
    assert(!pthread_join(t, NULL));
    return c;
}
int main(void) {
    alarm(30);
    engine_flat_base = calloc(1, 0x2000); assert(engine_flat_base);
    /* Before: a thread without attributes runs at DEFAULT whatever made it. */
    Case c = run(QOS_CLASS_USER_INTERACTIVE, "off", 0);
    assert(c.seen == QOS_CLASS_DEFAULT && c.applied == 0);
    /* Default: the reader made by the USER_INTERACTIVE engine thread runs
     * beside it, and a creator of another class passes that class on. */
    c = run(QOS_CLASS_USER_INTERACTIVE, NULL, 0);
    assert(c.seen == QOS_CLASS_USER_INTERACTIVE && c.applied == QOS_CLASS_USER_INTERACTIVE);
    c = run(QOS_CLASS_USER_INTERACTIVE, "inherit", HOST_CREATE_SUSPENDED);
    assert(c.seen == QOS_CLASS_USER_INTERACTIVE && c.applied == QOS_CLASS_USER_INTERACTIVE);
    c = run(QOS_CLASS_USER_INITIATED, NULL, 0);
    assert(c.seen == QOS_CLASS_USER_INITIATED && c.applied == QOS_CLASS_USER_INITIATED);
    /* Named classes, whatever the creator. */
    c = run(QOS_CLASS_UTILITY, "interactive", 0);
    assert(c.seen == QOS_CLASS_USER_INTERACTIVE && c.applied == QOS_CLASS_USER_INTERACTIVE);
    c = run(QOS_CLASS_USER_INTERACTIVE, "initiated", 0);
    assert(c.seen == QOS_CLASS_USER_INITIATED && c.applied == QOS_CLASS_USER_INITIATED);
    c = run(QOS_CLASS_USER_INTERACTIVE, "default", 0);
    assert(c.seen == QOS_CLASS_DEFAULT && c.applied == QOS_CLASS_DEFAULT);
    c = run(QOS_CLASS_USER_INTERACTIVE, "utility", HOST_CREATE_SUSPENDED);
    assert(c.seen == QOS_CLASS_UTILITY && c.applied == QOS_CLASS_UTILITY);
    /* Anything unrecognised is the old thread. */
    c = run(QOS_CLASS_USER_INTERACTIVE, "bogus", 0);
    assert(c.seen == QOS_CLASS_DEFAULT && c.applied == 0);
    free(engine_flat_base);
    puts("PASS guest thread QoS: inherits the creator's class by default (USER_INTERACTIVE from the engine thread), "
         "named classes applied, suspended creation held until resumed, off/unknown is the old DEFAULT thread");
}
