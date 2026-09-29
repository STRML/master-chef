/* Actual host pacing glue with deterministic clocks, real live settings and
 * the real bearing budget. No guest routine, GPU or scheduler is exercised. */
#include "../host.h"
#include "../halo_settings.h"
#include "../panorama_budget.h"
#include "../frame_pacer.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdatomic.h>
#include <assert.h>
#include <sys/wait.h>
extern char **environ;

static uint8_t guest_memory[8u * 1024u * 1024u];
uint8_t *engine_flat_base = guest_memory;
uint64_t host_yield_spin_ns;
static HaloPanoramaBudget panorama_budget;
static float panorama_target_fps(void) { return halo_settings_panorama_target_fps(); }
static uint64_t fake_now, fake_oversleep, fake_controller_work;
static uint64_t wait_deadline, idle_start, idle_end, measured_idle;
static unsigned clock_calls, wait_calls, idle_calls, interrupted_waits;

static uint64_t fake_clock(clockid_t clock) {
    assert(clock == CLOCK_UPTIME_RAW);
    /* The second read begins the wait after the controller ran. */
    if (++clock_calls == 2) fake_now += fake_controller_work;
    return fake_now;
}
static kern_return_t fake_timebase(mach_timebase_info_t info) {
    info->numer = info->denom = 1;
    return KERN_SUCCESS;
}
static kern_return_t fake_wait(uint64_t deadline) {
    wait_calls++;
    wait_deadline = deadline;
    assert(deadline > fake_now);
    if (interrupted_waits) {
        interrupted_waits--;
        fake_now += 100000u;
        return KERN_ABORTED;
    }
    fake_now = deadline + fake_oversleep;
    return KERN_SUCCESS;
}
void host_frame_idle_wait(uint64_t start, uint64_t end) {
    assert(end >= start);
    idle_calls++;
    idle_start = start; idle_end = end;
    measured_idle += end - start;
    host_yield_spin_ns += end - start;
}
void host_log(const char *format, ...) { (void)format; }
#define clock_gettime_nsec_np fake_clock
#define mach_timebase_info fake_timebase
#define mach_wait_until fake_wait
#include "../frame_pacing_hooks.inc"
#undef mach_wait_until
#undef mach_timebase_info
#undef clock_gettime_nsec_np

static void setting(int enabled, float fps) {
    HaloSettings settings;
    halo_settings_get(&settings);
    settings.frame_pacing = enabled;
    settings.panorama_target_fps = fps;
    halo_settings_set(&settings);
}
static void fixture(int enabled, float fps, int throttle, int cinematic) {
    setting(enabled, fps);
    memset(guest_memory, 0, sizeof guest_memory);
    S8(0x006894BAu, throttle);
    S32(0x006F187Cu, 0x1000u); S8(0x1009u, cinematic);
    S32(0x006F1D6Cu, 0x2000u); S16(0x2010u, 1);
    halo_panorama_budget_init(&panorama_budget);
    halo_frame_pacer_init(&frame_pacer);
    frame_pacer_ready = 0;
    frame_pacer_release_ns = frame_pacer_release_idle_ns = frame_pacer_scene_epoch = 0;
    frame_pacer_lead = frame_pacer_lag = frame_pacer_observed_period = frame_pacer_wait_seconds = 0.0;
    frame_pacer_locked = 0;
    memset(&frame_pacer_snapshot, 0, sizeof frame_pacer_snapshot);
    fake_now = 1000000000000ull;
    fake_oversleep = fake_controller_work = 0;
    wait_deadline = idle_start = idle_end = measured_idle = 0;
    host_yield_spin_ns = 0;
    clock_calls = wait_calls = idle_calls = interrupted_waits = 0;
}
static HaloFramePacerDisplay display(uint64_t observed_period) {
    /* Exact rational 90 Hz grid, rounded down to integer nanoseconds. */
    uint64_t tick = (uint64_t)((__uint128_t)fake_now * 90u / 1000000000u);
    return (HaloFramePacerDisplay){
        .latch_ns = (uint64_t)((__uint128_t)tick * 1000000000u / 90u),
        .period_ns = observed_period, .lag_ns = 4000000u
    };
}
static uint64_t present(uint64_t work_ns, uint64_t observed_period) {
    fake_now += work_ns;
    clock_calls = 0;
    HaloFramePacerDisplay d = display(observed_period);
    uint64_t release = host_frame_pacer_present(&d);
    assert(release == fake_now);
    return release;
}
static HaloFramePacerReport report(void) {
    HaloFramePacerReport r;
    host_frame_pacer_report(&r);
    return r;
}
static void coherent(const HaloFramePacerReport *r) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < HALO_PACER_RUNGS; i++) sum += r->rung_frames[i];
    assert(sum == r->frames && r->frames == r->paced_frames + r->free_frames);
    assert(r->late_frames <= r->paced_frames && r->rung < HALO_PACER_RUNGS);
    assert(fabs(r->target_period_ms - r->rung * (1000.0 / 90.0)) < 0.0001);
    assert(isfinite(r->work_ms) && isfinite(r->floor_work_ms) && isfinite(r->budget_target_ms));
    assert(isfinite(r->wait_seconds) && r->wait_seconds >= 0.0);
    assert(isfinite(r->requested_wait_seconds) && r->requested_wait_seconds >= 0.0);
}
static void settle(unsigned expected_rung, uint64_t work_ns, uint64_t observed_period) {
    uint8_t *before = malloc(sizeof guest_memory); assert(before);
    memcpy(before, guest_memory, sizeof guest_memory);
    uint64_t last = 0;
    for (unsigned i = 0; i < 160; i++) {
        uint64_t now = present(work_ns, observed_period);
        if (i >= 148) assert(fabs((double)(now - last) - expected_rung * (1e9 / 90.0)) < 10.0);
        last = now;
    }
    HaloFramePacerReport r = report();
    coherent(&r);
    assert(r.mode == HALO_PACER_EVEN && r.rung == expected_rung && r.paced_frames > 0);
    assert(r.latch_locked && fabs(r.display_period_ms - 1000.0 / 90.0) < 0.0001);
    assert(fabs(r.observed_display_period_ms - observed_period * 1e-6) < 0.0001);
    assert(wait_calls > 0 && measured_idle == host_yield_spin_ns);
    assert(!memcmp(before, guest_memory, sizeof guest_memory));
    free(before);
}

static _Atomic int reader_started, reader_done;
static _Atomic unsigned reader_samples;
static void *snapshot_reader(void *unused) {
    (void)unused;
    uint64_t frames = 0;
    double waits = 0.0;
    do {
        HaloFramePacerReport r = report();
        coherent(&r);
        assert(r.frames >= frames && r.wait_seconds >= waits);
        frames = r.frames; waits = r.wait_seconds;
        atomic_fetch_add(&reader_samples, 1);
        atomic_store(&reader_started, 1);
    } while (!atomic_load(&reader_done));
    return NULL;
}

static void environment_checks(const char *program) {
    const struct { const char *value, *expected; } cases[] = {
        { "on", "1" }, { "even", "1" }, { "1", "1" },
        { "0", "0" }, { "off", "0" }, { "invalid", "0" },
        { "2", "0" }, { "", "0" }
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        assert(!setenv("HALO_FRAME_PACING", cases[i].value, 1));
        char *args[] = { (char *)program, "--check-env", (char *)cases[i].expected, NULL };
        pid_t child;
        assert(!posix_spawnp(&child, program, NULL, NULL, args, environ));
        int status;
        assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    unsetenv("HALO_FRAME_PACING");
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--check-env")) {
        /* Each exec has fresh settings state, so this exercises real seeding. */
        assert(halo_settings_frame_pacing() == atoi(argv[2]));
        return 0;
    }
    /* Before the real settings module initializes, remove inherited opt-ins. */
    unsetenv("HALO_FRAME_PACING"); unsetenv("HALO_PANORAMA_TARGET_FPS");
    assert(halo_settings_frame_pacing() == HALO_PACER_OFF);
    environment_checks(argv[0]);
    fixture(0, 30.f, 1, 0);
    HaloPanoramaBudget before = panorama_budget;
    for (unsigned i = 0; i < 30; i++) present(12000000u, 22222222u);
    HaloFramePacerReport r = report();
    coherent(&r);
    assert(r.mode == HALO_PACER_OFF && !r.rung && !r.latch_locked && !wait_calls && !idle_calls);
    assert(r.frames == 30 && r.free_frames == 30 && r.wait_seconds == 0.0);
    assert(host_frame_pacer_budget_target(0.037f) == 0.037f);
    assert(!memcmp(&before, &panorama_budget, sizeof before));

    /* A live enable keeps the nominal 90 Hz grid even if callbacks arrived
     * every 22.2 ms. The resulting 30 fps interval stays 33.3 ms. */
    setting(1, 45.f);
    settle(3, 12000000u, 22222222u);
    assert(fabs(host_frame_pacer_budget_target(0.01f) - 0.03f) < 0.00001f);

    /* Only Halo's guest/cinematic limiter caps frame speed. The live
     * "Keep at least" goal controls bearing work, including a goal of 27 Hz,
     * and must not forbid 30 Hz or cap a deliberately unthrottled engine. */
    fixture(1, 45.f, 0, 1); settle(3, 12000000u, 11111111u);
    fixture(1, 27.f, 1, 0); settle(3, 12000000u, 11111111u);
    fixture(1, 20.f, 1, 0); settle(3, 12000000u, 11111111u);
    fixture(1, 30.f, 0, 0); settle(2, 12000000u, 11111111u);
    fixture(1, 27.f, 0, 0); settle(2, 12000000u, 11111111u);
    fixture(1, 45.f, 0, 0); settle(2, 12000000u, 11111111u);

    /* Timedemo bypasses an already active pacer without changing the user's
     * live preference. Returning to play re-engages it from recent work. */
    unsigned waits = wait_calls;
    uint64_t free_before = report().free_frames;
    S32(0x007196D8u, 1);
    for (unsigned i = 0; i < 8; i++) present(12000000u, 11111111u);
    r = report(); coherent(&r);
    assert(r.mode == HALO_PACER_EVEN && !r.rung && wait_calls == waits);
    assert(r.free_frames == free_before + 8 && host_frame_pacer_budget_target(0.037f) == 0.037f);
    assert(G32(0x007196D8u) == 1);
    S32(0x007196D8u, 0);
    settle(2, 12000000u, 11111111u);

    /* A scene entry may complete quickly, before the ordinary hitch guard.
     * The first budget query uses its fallback and the first publication
     * clears the old window without waiting or losing cumulative counts. */
    HaloFramePacerReport before_scene = report();
    waits = wait_calls;
    halo_panorama_budget_scene_entry(&panorama_budget);
    assert(host_frame_pacer_budget_target(0.037f) == 0.037f);
    present(12000000u, 11111111u);
    r = report(); coherent(&r);
    assert(!r.rung && !r.latch_locked && !frame_pacer.have_work && frame_pacer.win_count == 0);
    assert(wait_calls == waits && r.frames == before_scene.frames + 1);
    assert(r.rung_changes == before_scene.rung_changes + 1);
    assert(r.paced_frames == before_scene.paced_frames && r.wait_seconds == before_scene.wait_seconds);
    settle(2, 12000000u, 11111111u);

    /* The switch is visible before a frame and stops waiting on its very
     * next call; its former cadence no longer controls the bearing budget. */
    waits = wait_calls;
    setting(0, 45.f);
    assert(report().mode == HALO_PACER_OFF);
    assert(host_frame_pacer_budget_target(0.037f) == 0.037f);
    present(12000000u, 11111111u);
    r = report(); coherent(&r);
    assert(!r.rung && !frame_pacer.have_work && wait_calls == waits);

    fixture(1, 30.f, 1, 0); settle(3, 12000000u, 11111111u);
    fake_now += 12000000u; clock_calls = 0;
    HaloFramePacerDisplay stale = display(22222222u);
    stale.latch_ns = fake_now - 250000000u;
    host_frame_pacer_present(&stale);
    assert(!report().latch_locked && report().rung == 3);
    fake_now += 12000000u; clock_calls = 0;
    stale.latch_ns = fake_now + 1u;
    host_frame_pacer_present(&stale);
    assert(!report().latch_locked);
    fake_now += 12000000u; clock_calls = 0;
    host_frame_pacer_present(NULL);
    assert(!report().latch_locked && report().observed_display_period_ms == 0.0f);

    /* Inject 7 ms of guest-limiter idle, 2 ms controller cost and 1 ms
     * scheduler oversleep. Only the actual blocking interval is charged. */
    fixture(1, 30.f, 1, 0); settle(3, 12000000u, 11111111u);
    HaloFramePacerReport previous = report();
    uint64_t idle_before = host_yield_spin_ns;
    unsigned idle_calls_before = idle_calls;
    fake_now += 7000000u; host_yield_spin_ns += 7000000u;
    fake_controller_work = 2000000u; fake_oversleep = 1000000u;
    uint64_t arrival = fake_now + 12000000u;
    present(12000000u, 11111111u);
    r = report(); coherent(&r);
    double requested = r.requested_wait_seconds - previous.requested_wait_seconds;
    double actual = r.wait_seconds - previous.wait_seconds;
    assert(idle_calls == idle_calls_before + 1 && idle_start == arrival + 2000000u);
    assert(idle_end == wait_deadline + 1000000u && idle_end == fake_now);
    assert(fabs(actual - (double)(idle_end - idle_start) * 1e-9) < 1e-10);
    assert(fabs(actual - (requested - 0.001)) < 1e-8);
    assert(host_yield_spin_ns - idle_before == 7000000u + idle_end - idle_start);
    assert(fabs(r.work_ms - 12.0) < 0.0001);

    /* Interrupted waits block again, with one measured accounting interval. */
    fake_controller_work = fake_oversleep = 0;
    interrupted_waits = 2;
    waits = wait_calls; idle_calls_before = idle_calls;
    present(12000000u, 11111111u);
    assert(wait_calls == waits + 3 && idle_calls == idle_calls_before + 1);
    assert(fake_now == wait_deadline);
    interrupted_waits = 10;
    waits = wait_calls; idle_calls_before = idle_calls;
    previous = report();
    present(12000000u, 11111111u);
    r = report();
    assert(wait_calls == waits + 4 && idle_calls == idle_calls_before + 1);
    assert(fabs(r.wait_seconds - previous.wait_seconds - 0.0004) < 1e-10);
    assert(r.requested_wait_seconds - previous.requested_wait_seconds > 0.001);

    /* The report thread may read during publication; every complete snapshot
     * must preserve its counters and cadence relation without torn fields. */
    fixture(1, 30.f, 1, 0); settle(3, 12000000u, 11111111u);
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, snapshot_reader, NULL));
    while (!atomic_load(&reader_started)) sched_yield();
    for (unsigned i = 0; i < 2000; i++) present(12000000u, 11111111u);
    atomic_store(&reader_done, 1);
    assert(!pthread_join(reader, NULL) && atomic_load(&reader_samples) > 0);
    host_frame_pacer_report(NULL);
    puts("PASS frame pacing host: default/environment/live settings, nominal 90 Hz, guest/cinematic caps, independent 20/27/30 Hz bearing goals, timedemo bypass, scene-entry reset, stale latches, measured waits, retries, unchanged guest memory, concurrent snapshots");
    return 0;
}
