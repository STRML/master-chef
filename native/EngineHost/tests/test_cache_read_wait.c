/* Halo's blocking cache-read waits (Sleep(0) returning to 00443EEA/004446D8)
 * block until the reader delivers a completion, bounded, never missing one.
 * clang -O2 -pthread -DENGINE_FLAT_MEMORY=1 -I native/EngineHost -I native/EngineReuse \
 *   -ffunction-sections -fdata-sections native/EngineHost/tests/test_cache_read_wait.c \
 *   -Wl,-dead_strip -o /tmp/cache-read-wait */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "../shims_kernel32.c"
uint8_t *engine_flat_base;
pthread_t host_present_thread; int host_present_thread_set;
static FileObj file;
static _Thread_local uint32_t error_code;
static _Atomic int request_done, reader_go, reader_finished;
static _Atomic unsigned completions_run;
void host_set_last_error(uint32_t e) { error_code = e; }
void *host_handle_object(uint32_t h, int kind) { return h == 64 && kind == HANDLE_FILE ? &file : NULL; }
void host_log(const char *fmt, ...) { (void)fmt; }
_Noreturn void host_exit(int code) { (void)code; abort(); }
uint32_t host_wait_single(uint32_t h, uint32_t timeout) { (void)h; (void)timeout; return HOST_WAIT_TIMEOUT; }
/* 00443AE0: the completion routine marks its request done. */
uint32_t host_call_guest(EngineCPU *cpu, uint32_t routine, int argc, const uint32_t *args, int pops) {
    (void)cpu; (void)args;
    assert(routine == 0x00443AE0u && argc == 3 && pops == 1);
    atomic_fetch_add(&completions_run, 1);
    atomic_store(&request_done, 1);
    return 0;
}
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint64_t thread_cpu_ns(void) { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }
static uint32_t call(EngineCPU *cpu, HostShim fn, uint32_t ret, unsigned argc, const uint32_t *argv) {
    uint32_t stack = cpu->fs_base ? cpu->fs_base : 0x1000;
    cpu->gpr[4] = stack;
    S32(stack, ret);
    for (unsigned i = 0; i < argc; i++) S32(stack + 4 + 4*i, argv[i]);
    fn(cpu);
    assert(cpu->pc == ret && cpu->gpr[4] == stack + 4 + 4*argc);
    return cpu->gpr[0];
}
static void sleep0(EngineCPU *cpu, uint32_t ret) { uint32_t zero = 0; call(cpu, shim_Sleep, ret, 1, &zero); }
/* The reader: 00442C70 issues ReadFileEx, then 00443940's alertable wait runs
 * the completion. */
static void reader_complete_one(EngineCPU *cpu) {
    uint32_t ov = 0x3000; S32(ov + 8, 0); S32(ov + 12, 0);
    uint32_t read_args[] = { 64, 0x6000, 4, ov, 0x00443AE0u };
    assert(call(cpu, shim_ReadFileEx, 0x44000000u, 5, read_args) == 1);
    uint32_t wait_args[] = { 88, 0xFFFFFFFFu, 1 };
    assert(call(cpu, shim_WaitForSingleObjectEx, 0x44000000u, 3, wait_args) == 0xC0);
}
static void *reader(void *delay_us) {
    EngineCPU cpu = {0}; cpu.fs_base = 0x1800;
    while (!atomic_load(&reader_go)) usleep(100);
    usleep((useconds_t)(uintptr_t)delay_us);
    reader_complete_one(&cpu);
    atomic_store(&reader_finished, 1);
    return NULL;
}
/* Halo's loop: test the request's flag, Sleep(0), test again. */
typedef struct { unsigned calls; uint64_t wall_ns, cpu_ns; } Waited;
static Waited guest_wait_loop(EngineCPU *cpu, uint32_t site, unsigned delay_us) {
    atomic_store(&request_done, 0); atomic_store(&reader_finished, 0); atomic_store(&reader_go, 0);
    pthread_t t; assert(!pthread_create(&t, NULL, reader, (void *)(uintptr_t)delay_us));
    Waited w = {0};
    uint64_t wall = now_ns(), used = thread_cpu_ns();
    atomic_store(&reader_go, 1);
    while (!atomic_load(&request_done)) { sleep0(cpu, site); w.calls++; }
    w.wall_ns = now_ns() - wall; w.cpu_ns = thread_cpu_ns() - used;
    assert(!pthread_join(t, NULL) && atomic_load(&reader_finished));
    return w;
}
int main(void) {
    alarm(60);
    engine_flat_base = calloc(1, 0x10000); assert(engine_flat_base);
    char path[] = "/private/tmp/halo-cache-wait-XXXXXX";
    file.fd = mkstemp(path); assert(file.fd >= 0);
    assert(write(file.fd, "0123456789abcdef", 16) == 16);
    EngineCPU cpu = {0};
    unsetenv("HALO_IO_WAIT"); assert(host_cache_wait_blocks() == 1);

    /* Any other Sleep(0) is untouched: no wait, not counted. */
    cache_wait_limit_ns = 5000000000ull;
    uint64_t t0 = now_ns();
    for (int i = 0; i < 100; i++) sleep0(&cpu, 0x004C9FA0u);
    assert(now_ns() - t0 < 500000000ull && host_cache_wait_calls == 0);

    /* Nothing completes: the wait gives up at its bound. */
    cache_wait_limit_ns = 30000000ull;
    t0 = now_ns(); sleep0(&cpu, 0x00443EEAu);
    uint64_t waited = now_ns() - t0;
    assert(waited >= 25000000ull && waited < 1000000000ull);
    assert(host_cache_wait_calls == 1 && host_cache_wait_ns >= 25000000ull);

    /* A completion wakes the wait at once, well inside a five-second bound,
     * at both sites. */
    cache_wait_limit_ns = 5000000000ull;
    Waited w = guest_wait_loop(&cpu, 0x00443EEAu, 50000);
    assert(w.wall_ns < 1000000000ull && w.calls <= 3);
    w = guest_wait_loop(&cpu, 0x004446D8u, 20000);
    assert(w.wall_ns < 1000000000ull && w.calls <= 3);

    /* A completion delivered before the call, after this thread last
     * returned, sends it straight back to its test instead of sleeping. */
    { pthread_t t; atomic_store(&reader_go, 1);
      assert(!pthread_create(&t, NULL, reader, (void *)(uintptr_t)0));
      assert(!pthread_join(t, NULL)); }
    t0 = now_ns(); sleep0(&cpu, 0x00443EEAu);
    assert(now_ns() - t0 < 1000000000ull);

    /* The presenting thread's wait is idle time for the bearing budget, as
     * the spin it replaces was. */
    host_present_thread = pthread_self(); host_present_thread_set = 1;
    cache_wait_limit_ns = 20000000ull;
    uint64_t spin_before = host_yield_spin_ns, blocked_before = host_present_sleep_ns;
    sleep0(&cpu, 0x004446D8u);
    assert(host_yield_spin_ns - spin_before >= 15000000ull);
    assert(host_present_sleep_ns - blocked_before >= 15000000ull);
    host_present_thread_set = 0;

    /* With the production bound, a 30 ms read: the wait mode against the old
     * spin (HALO_IO_WAIT=0), on the same loop. */
    cache_wait_limit_ns = 1000000ull;
    Waited blocked = guest_wait_loop(&cpu, 0x00443EEAu, 30000);
    setenv("HALO_IO_WAIT", "0", 1); cache_wait_mode = -1; assert(host_cache_wait_blocks() == 0);
    uint64_t calls_before = host_cache_wait_calls;
    Waited spun = guest_wait_loop(&cpu, 0x00443EEAu, 30000);
    assert(host_cache_wait_calls - calls_before == spun.calls);
    assert(blocked.calls * 10 < spun.calls);
    assert(atomic_load(&completions_run) >= 5 && host_cache_reads >= 5);
    close(file.fd); assert(!unlink(path));
    free(engine_flat_base);
    printf("PASS cache-read wait: other Sleep(0) untouched, bounded without a completion, woken by the reader's "
           "completion, none slept through, idle for the budget; 30 ms read: wait %u calls %.2f ms engine CPU "
           "(wall %.1f ms) against spin %u calls %.2f ms CPU (wall %.1f ms)\n",
           blocked.calls, blocked.cpu_ns / 1e6, blocked.wall_ns / 1e6, spun.calls, spun.cpu_ns / 1e6, spun.wall_ns / 1e6);
}
