/* Optional publication pacing; included after panorama_hooks.inc. All control
 * state belongs to the engine thread. Diagnostics reads only a locked copy.
 * HALO_FRAME_PACING seeds the live setting (default off). No guest function
 * is replaced: Halo's limiter, game time and cutscene catch-up stay intact. */
#include "frame_pacer.h"
#include <mach/mach_time.h>
#include <pthread.h>

static HaloFramePacer frame_pacer;
static int frame_pacer_ready;
static uint64_t frame_pacer_release_ns, frame_pacer_release_idle_ns, frame_pacer_scene_epoch;
static const double frame_pacer_period = 1.0 / 90.0;
static double frame_pacer_lead, frame_pacer_lag, frame_pacer_observed_period, frame_pacer_wait_seconds;
static int frame_pacer_locked;
static pthread_mutex_t frame_pacer_report_lock = PTHREAD_MUTEX_INITIALIZER;
static HaloFramePacerReport frame_pacer_snapshot;

/* 004C9F90 limits intervals between QPC samples, not time added to each
 * frame. Keep it unchanged and respect its 30 fps ceiling whenever enabled
 * by framerate_throttle (0x6894BA) or the cinematic flag. */
static double frame_pacer_min_period(void) {
    uint32_t cinematic = G32(0x006F187Cu);
    /* "Keep at least" (panorama_target_fps) is the bearing budget's goal,
     * not a speed cap. A goal of 27 Hz must not forbid the 30 Hz cadence.
     * Without Halo's limiter, 45 Hz is the pacer's fastest supported rung. */
    return G8(0x006894BAu) || (cinematic && G8(cinematic + 9u)) ? 1.0 / 30.0 : 1.0 / 45.0;
}

static void frame_pacer_wait_until(uint64_t deadline_ns) {
    static mach_timebase_info_data_t timebase;
    if (!timebase.denom) mach_timebase_info(&timebase);
    /* CLOCK_UPTIME_RAW and mach_absolute_time share an epoch. Avoid losing
     * low bits converting a long system uptime through floating point. Each
     * retry blocks, including after an interrupted wait; there is no spin. */
    uint64_t deadline = (uint64_t)((__uint128_t)deadline_ns * timebase.denom / timebase.numer);
    for (int tries = 0; tries < 4; tries++) {
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline_ns) break;
        mach_wait_until(deadline);
    }
}

static void frame_pacer_publish_report(int mode) {
    HaloFramePacerReport report = { 0 };
    report.mode = mode;
    report.rung = frame_pacer.rung;
    report.latch_locked = (uint32_t)(mode != HALO_PACER_OFF && frame_pacer.rung && frame_pacer_locked);
    report.display_period_ms = (float)(frame_pacer_period * 1e3);
    report.observed_display_period_ms = (float)(frame_pacer_observed_period * 1e3);
    report.target_period_ms = (float)(frame_pacer.rung * frame_pacer_period * 1e3);
    report.lead_ms = (float)(frame_pacer_lead * 1e3);
    report.publish_lag_ms = (float)(frame_pacer_lag * 1e3);
    report.work_ms = (float)(frame_pacer.work_ema * 1e3);
    report.floor_work_ms = (float)(frame_pacer.floor_ema * 1e3);
    report.budget_target_ms = (float)(host_frame_pacer_budget_target(1.f / panorama_target_fps()) * 1e3);
    report.late_share = (float)frame_pacer.late_ema;
    report.frames = frame_pacer.frames;
    report.paced_frames = frame_pacer.paced_frames;
    report.late_frames = frame_pacer.late_frames;
    report.free_frames = frame_pacer.free_frames;
    report.rung_changes = frame_pacer.rung_changes;
    memcpy(report.rung_frames, frame_pacer.rung_frames, sizeof report.rung_frames);
    report.wait_seconds = frame_pacer_wait_seconds;
    report.requested_wait_seconds = frame_pacer.wait_seconds;
    pthread_mutex_lock(&frame_pacer_report_lock);
    frame_pacer_snapshot = report;
    pthread_mutex_unlock(&frame_pacer_report_lock);
}

uint64_t host_frame_pacer_present(const HaloFramePacerDisplay *display) {
    uint64_t now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), idle_ns = host_yield_spin_ns;
    if (!frame_pacer_ready) { halo_frame_pacer_init(&frame_pacer); frame_pacer_ready = 1; }
    if (frame_pacer_scene_epoch != panorama_budget.scene_epoch) {
        /* The panorama host detected the front end closing or a >1 s load
         * gap. Its new scene already starts at the heavy bearing tier; old
         * menu/load timings must not hold this publication to an old slot. */
        if (frame_pacer.rung) frame_pacer.rung_changes++;
        halo_frame_pacer_reset(&frame_pacer);
        frame_pacer_release_ns = 0;
        frame_pacer_release_idle_ns = idle_ns;
        frame_pacer_scene_epoch = panorama_budget.scene_epoch;
    }
    /* Timedemo explicitly bypasses Halo's own throttle. Leave benchmark
     * timing alone even when the user has enabled pacing for play. */
    int mode = G32(0x007196D8u) ? HALO_PACER_OFF : halo_settings_frame_pacing();
    uint64_t interval = frame_pacer_release_ns && now_ns > frame_pacer_release_ns ? now_ns - frame_pacer_release_ns : 0;
    uint64_t idle = idle_ns >= frame_pacer_release_idle_ns ? idle_ns - frame_pacer_release_idle_ns : 0;
    double work = interval ? (double)(interval > idle ? interval - idle : 0) * 1.0e-9 : 0.0;
    frame_pacer_observed_period = display ? (double)display->period_ns * 1.0e-9 : 0.0;
    double lag = display && display->lag_ns && display->lag_ns < 100000000u ? (double)display->lag_ns * 1.0e-9 : 0.004;
    frame_pacer_lag = frame_pacer_lag > 0.0 ? frame_pacer_lag + 0.1 * (lag - frame_pacer_lag) : lag;
    /* Aim for GPU completion half a nominal display period before the
     * compositor samples. A median observed interval of 22 ms can represent
     * missed compositor callbacks, so it must not halve all pacing rungs. */
    frame_pacer_lead = frame_pacer_lag + 0.5 * frame_pacer_period;
    frame_pacer_locked = display && display->latch_ns && now_ns >= display->latch_ns &&
                         now_ns - display->latch_ns < 250000000u;
    if (mode == HALO_PACER_OFF) {
        if (frame_pacer.rung || frame_pacer.have_work) {
            int was_pacing = frame_pacer.rung != 0;
            if (was_pacing) frame_pacer.rung_changes++;
            halo_frame_pacer_reset(&frame_pacer);
            if (was_pacing) halo_panorama_budget_retarget(&panorama_budget, 1.f / panorama_target_fps());
        }
        frame_pacer.frames++; frame_pacer.free_frames++; frame_pacer.rung_frames[0]++;
    } else {
        /* Read only the tick count 00470BF0 left in the time globals. */
        uint32_t time_globals = G32(0x006F1D6Cu);
        HaloFramePacerInput in = {
            .now = (double)now_ns * 1.0e-9, .work = work,
            .floor_work = halo_panorama_budget_floor_busy(&panorama_budget, (float)work),
            .ticks = time_globals ? (int)G16(time_globals + 0x10u) : -1,
            .display_period = frame_pacer_period,
            .latch = frame_pacer_locked ? (double)display->latch_ns * 1.0e-9 : 0.0,
            .lead = frame_pacer_lead, .min_period = frame_pacer_min_period(), .max_period = 1.0 / 15.0,
            .mode = mode };
        unsigned before = frame_pacer.rung;
        int shortened = 0;
        double slot = halo_frame_pacer_slot(&frame_pacer, &in, &shortened);
        if (shortened)
            halo_panorama_budget_retarget(&panorama_budget, host_frame_pacer_budget_target(1.f / panorama_target_fps()));
        if (frame_pacer.rung != before) {
            static unsigned reports;
            if (reports++ < 64)
                host_log("[pacer] rung %u -> %u (%.1f fps) work=%.1fms floor=%.1fms period=%.2fms lead=%.1fms locked=%d",
                         before, frame_pacer.rung,
                         frame_pacer.rung ? 1.0 / ((double)frame_pacer.rung * frame_pacer_period) : 0.0,
                         frame_pacer.work_ema * 1e3, frame_pacer.floor_ema * 1e3,
                         frame_pacer_period * 1e3, frame_pacer_lead * 1e3, frame_pacer_locked);
        }
        if (slot > in.now) {
            uint64_t deadline = (uint64_t)(slot * 1.0e9);
            if (deadline > now_ns + 100000000u) deadline = now_ns + 100000000u;
            /* Account only the blocking wait, excluding controller/logging
             * work. The next Sleep(0) will not time the gap across it as spin,
             * so it is counted once. Only this wait calls it: pacing off
             * leaves Build78's Sleep/SleepEx accounting untouched. */
            uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (deadline > start) {
                frame_pacer_wait_until(deadline);
                uint64_t end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                host_frame_idle_wait(start, end);
                frame_pacer_wait_seconds += (double)(end - start) * 1.0e-9;
            }
        }
    }
    frame_pacer_release_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    frame_pacer_release_idle_ns = host_yield_spin_ns;
    frame_pacer_publish_report(mode);
    return frame_pacer_release_ns;
}

float host_frame_pacer_budget_target(float fallback_seconds) {
    if (!frame_pacer_ready || halo_settings_frame_pacing() == HALO_PACER_OFF ||
        frame_pacer_scene_epoch != panorama_budget.scene_epoch) return fallback_seconds;
    return (float)halo_frame_pacer_budget_target(&frame_pacer, frame_pacer_period, fallback_seconds);
}

void host_frame_pacer_report(HaloFramePacerReport *out) {
    if (!out) return;
    pthread_mutex_lock(&frame_pacer_report_lock);
    *out = frame_pacer_snapshot;
    pthread_mutex_unlock(&frame_pacer_report_lock);
    /* A live toggle is visible before the next engine frame publishes. */
    out->mode = halo_settings_frame_pacing();
}
