/* Production Sleep/SleepEx with a deterministic clock.
 *
 * With frame pacing off nothing calls host_frame_idle_wait, and Sleep, SleepEx
 * and Sleep(0) must account exactly as they did before pacing existed. The
 * bearing budget subtracts host_yield_spin_ns from every frame, so a change
 * here changes bearing choices and engine-thread timing with the feature off.
 * The values below are the Build78 shim's own, quirks included: a short
 * Sleep(n) between close Sleep(0) calls is counted by both, SleepEx is not
 * presenter idle, and the Sleep(0) burst counter carries across frames and
 * gaps. Changing any of those is a separate change with its own evidence.
 *
 * The frame pacer's own wait, which only happens while pacing is on, counts
 * once as presenter idle, and the next Sleep(0) does not time the gap across
 * it. Each scenario runs on a fresh thread, so the shims' thread-local
 * history starts empty exactly as it does for a new engine thread. */
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
static _Thread_local uint64_t fake_now, fake_oversleep;
static _Thread_local unsigned native_sleeps;
static uint64_t fake_clock(clockid_t clock) {
    assert(clock == CLOCK_UPTIME_RAW);
    return fake_now;
}
static int fake_usleep(useconds_t us) {
    native_sleeps++;
    fake_now += (uint64_t)us * 1000u + fake_oversleep;
    return 0;
}
#define clock_gettime_nsec_np fake_clock
#define usleep fake_usleep
#include "../shims_kernel32.c"
#undef usleep
#undef clock_gettime_nsec_np

uint8_t *engine_flat_base;
pthread_t host_present_thread;
int host_present_thread_set;
uint32_t host_call_guest(EngineCPU *cpu, uint32_t routine, int argc, const uint32_t *args, int pops) {
    (void)cpu; (void)routine; (void)argc; (void)args; (void)pops;
    assert(!"No APC was queued");
    return 0;
}
static void call(EngineCPU *cpu, HostShim fn, unsigned argc, uint32_t ms) {
    cpu->gpr[4] = 0x1000;
    S32(0x1000, 0x12345678); S32(0x1004, ms); S32(0x1008, 0);
    fn(cpu);
    assert(cpu->pc == 0x12345678 && cpu->gpr[4] == 0x1004 + 4 * argc && cpu->gpr[0] == 0);
}
static void run(void *(*scenario)(void *)) {
    host_sleep_calls = host_sleep_ns = host_yield_calls = host_yield_spin_ns = 0;
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, scenario, NULL));
    assert(!pthread_join(thread, NULL));
}
static void present_here(void) {
    host_present_thread = pthread_self(); host_present_thread_set = 1;
    fake_now = 1000000000u; fake_oversleep = 0; native_sleeps = 0;
}

static void *sleeps_and_yields(void *opaque) {
    (void)opaque; present_here();
    EngineCPU cpu = {0};
    call(&cpu, shim_Sleep, 1, 0);
    fake_now += 100000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 100000u);
    /* A presenter Sleep(n) is idle for its measured length. */
    fake_now += 100000u;
    call(&cpu, shim_Sleep, 1, 1);
    assert(host_sleep_calls == 1 && host_sleep_ns == 1000000u && host_yield_spin_ns == 1100000u);
    /* Build78 also counts the 1.2 ms gap across it as spin. */
    fake_now += 100000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 2300000u);
    /* SleepEx is timed but was never presenter idle. */
    call(&cpu, shim_SleepEx, 2, 1);
    assert(host_sleep_calls == 2 && host_sleep_ns == 2000000u && host_yield_spin_ns == 2300000u);
    fake_now += 100000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 3400000u);
    /* Oversleep is measured, not assumed. */
    fake_oversleep = 3000000u;
    call(&cpu, shim_SleepEx, 2, 2);
    assert(host_sleep_calls == 3 && host_sleep_ns == 7000000u && host_yield_spin_ns == 3400000u);
    call(&cpu, shim_Sleep, 1, 2);
    assert(host_sleep_calls == 4 && host_sleep_ns == 12000000u && host_yield_spin_ns == 8400000u);
    assert(native_sleeps == 4 && host_yield_calls == 4);
    return NULL;
}

static void *spin_burst(void *opaque) {
    (void)opaque; present_here();
    EngineCPU cpu = {0};
    for (unsigned i = 0; i < 256; i++) {
        fake_now += 1000u;
        call(&cpu, shim_Sleep, 1, 0);
    }
    assert(native_sleeps == 1 && host_yield_calls == 256 && host_sleep_calls == 0);
    assert(host_yield_spin_ns == 255000u + 250000u);
    fake_now += 10000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 515000u); /* The 250 us wait is not counted again. */
    return NULL;
}

static void *scattered_yields(void *opaque) {
    (void)opaque; present_here();
    EngineCPU cpu = {0};
    /* Gaps over 2 ms are work, never spin, but Build78's burst counter still
     * reaches 256 across them and gives the core up once per 256 calls. */
    for (unsigned i = 0; i < 512; i++) {
        fake_now += 2100000u;
        call(&cpu, shim_Sleep, 1, 0);
    }
    assert(native_sleeps == 2 && host_yield_calls == 512 && host_yield_spin_ns == 2u * 250000u);
    return NULL;
}

static void *play_frames(void *opaque) {
    (void)opaque; present_here();
    EngineCPU cpu = {0};
    /* 300 play frames: 20 yields 2.5 ms apart, then Halo's limiter sleeping
     * 10 ms and spinning 40 Sleep(0) calls 20 us apart; every native sleep
     * overshoots by 20 us. These totals are the Build78 shim's. */
    fake_oversleep = 20000u;
    for (unsigned f = 0; f < 300; f++) {
        for (unsigned y = 0; y < 20; y++) { fake_now += 2500000u; call(&cpu, shim_Sleep, 1, 0); }
        call(&cpu, shim_Sleep, 1, 10);
        for (unsigned y = 0; y < 40; y++) { fake_now += 20000u; call(&cpu, shim_Sleep, 1, 0); }
    }
    assert(host_yield_calls == 18000u && host_sleep_calls == 300u);
    assert(native_sleeps == 370u && host_yield_spin_ns == 3258900000ull);
    return NULL;
}

static void *pacer_wait(void *opaque) {
    (void)opaque; present_here();
    EngineCPU cpu = {0};
    call(&cpu, shim_Sleep, 1, 0);
    fake_now += 100000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 100000u);
    /* The pacer's wait counts once; the 650 us gap across it is not spin. */
    fake_now += 50000u;
    uint64_t start = fake_now;
    fake_now += 500000u;
    host_frame_idle_wait(start, fake_now);
    assert(host_yield_spin_ns == 600000u);
    fake_now += 100000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 600000u);
    fake_now += 100000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 700000u);
    /* An empty or reversed interval charges nothing. */
    host_frame_idle_wait(fake_now, fake_now);
    host_frame_idle_wait(fake_now + 1u, fake_now);
    assert(host_yield_spin_ns == 700000u);
    /* The burst counter stays Sleep's own: 4 calls so far, 252 more reach 256. */
    for (unsigned i = 0; i < 200; i++) { fake_now += 1000u; call(&cpu, shim_Sleep, 1, 0); }
    start = fake_now; fake_now += 500000u;
    host_frame_idle_wait(start, fake_now);
    assert(native_sleeps == 0);
    for (unsigned i = 0; i < 52; i++) { fake_now += 1000u; call(&cpu, shim_Sleep, 1, 0); }
    assert(native_sleeps == 1 && host_sleep_calls == 0 && host_sleep_ns == 0);
    return NULL;
}

static void *other_thread(void *opaque) {
    (void)opaque;
    EngineCPU cpu = {0};
    fake_now = 2000000000u;
    call(&cpu, shim_Sleep, 1, 1);
    call(&cpu, shim_SleepEx, 2, 1);
    for (unsigned i = 0; i < 256; i++) {
        fake_now += 1000u;
        call(&cpu, shim_Sleep, 1, 0);
    }
    uint64_t start = fake_now;
    fake_now += 500000u;
    host_frame_idle_wait(start, fake_now);
    return NULL;
}
static void *presenter_and_other(void *opaque) {
    (void)opaque; present_here();
    EngineCPU cpu = {0};
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, other_thread, NULL));
    assert(!pthread_join(worker, NULL));
    assert(host_sleep_calls == 2 && host_sleep_ns == 2000000u && host_yield_calls == 256);
    assert(host_yield_spin_ns == 0); /* Only the presenter contributes frame idle. */
    call(&cpu, shim_Sleep, 1, 0);
    fake_now += 10000u;
    call(&cpu, shim_Sleep, 1, 0);
    assert(host_yield_spin_ns == 10000u); /* Worker history never affects it. */
    return NULL;
}

int main(void) {
    engine_flat_base = calloc(1, 0x2000); assert(engine_flat_base);
    run(sleeps_and_yields);
    run(spin_burst);
    run(scattered_yields);
    run(play_frames);
    run(pacer_wait);
    run(presenter_and_other);
    free(engine_flat_base);
    puts("PASS sleep accounting: Sleep/SleepEx/Sleep(0) as in Build78 with pacing off, pacer waits counted once, other threads excluded");
    return 0;
}
