/* Arithmetic controller checks plus trace-derived cadence simulation.
 * Build75/74 timelines contain interval aggregates, NOT per-frame samples.
 * frame_pacer_traces.inc preserves those observations; the reconstruction
 * below labels and varies every per-frame assumption. This is a regression
 * test, not headset evidence and not sufficient to enable pacing by default.
 *
 * clang -O2 -Wall -Wextra -I native/EngineHost \
 *   native/EngineHost/tests/test_frame_pacer.c -lm -o /tmp/halo-pacer && /tmp/halo-pacer
 */
#include "../frame_pacer.h"
#include "../panorama_budget.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "frame_pacer_traces.inc"

#define D90 (1.0 / 90.0)
#define LEAD (0.004 + 0.5 * D90)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static HaloFramePacerInput input(double now, double work, double phase, int mode) {
    HaloFramePacerInput in = {
        .now = now, .work = work, .floor_work = work, .ticks = -1,
        .display_period = D90,
        .latch = phase >= 0 ? phase + floor((now - phase) / D90) * D90 : 0,
        .lead = LEAD, .min_period = 1.0 / 30.0, .max_period = 1.0 / 15.0, .mode = mode
    };
    return in;
}

static double steady_t;
static unsigned steady(HaloFramePacer *p, double work, unsigned frames, double phase) {
    for (unsigned i = 0; i < frames; i++) {
        HaloFramePacerInput in = input(steady_t + work, work, phase, HALO_PACER_EVEN);
        double slot = halo_frame_pacer_slot(p, &in, NULL);
        assert(isfinite(slot) && slot >= in.now - 1e-10);
        assert(slot - in.now <= in.max_period + D90);
        steady_t = slot + 0.0001;
    }
    return p->rung;
}

static void controller_checks(void) {
    HaloFramePacer p;
    const double phase = 0.0031;
    const double work[] = {0.020, 0.039, 0.049, 0.060, 0.075};
    const unsigned rung[] = {3, 4, 5, 6, 0};
    for (unsigned k = 0; k < COUNT(work); k++) {
        halo_frame_pacer_init(&p); steady_t = 100;
        assert(steady(&p, work[k], 240, phase) == rung[k]);
        double previous = 0;
        for (unsigned i = 0; i < 60; i++) {
            HaloFramePacerInput in = input(steady_t + work[k], work[k], phase, HALO_PACER_EVEN);
            double slot = halo_frame_pacer_slot(&p, &in, NULL);
            if (rung[k]) {
                if (previous) assert(fabs(slot - previous - rung[k] * D90) < 1e-8);
                double cycles = (slot - phase + LEAD) / D90;
                assert(fabs(cycles - round(cycles)) < 1e-7);
            } else assert(slot == in.now);
            previous = slot; steady_t = slot + 0.0001;
        }
    }
    /* Exactly one late frame slips to the next display latch. */
    halo_frame_pacer_init(&p); steady_t = 200;
    steady(&p, 0.020, 120, phase);
    HaloFramePacerInput in = input(steady_t + 0.045, 0.045, phase, HALO_PACER_EVEN);
    uint64_t late = p.late_frames;
    double slot = halo_frame_pacer_slot(&p, &in, NULL);
    assert(p.late_frames == late + 1 && slot >= in.now && slot - in.now <= D90 + 0.0002);
    in = input(slot + 0.020, 0.020, phase, HALO_PACER_EVEN);
    assert(fabs(halo_frame_pacer_slot(&p, &in, NULL) - slot - 3 * D90) < 1e-8);
    /* Missing display phase: relative cadence, still no spinning required. */
    halo_frame_pacer_init(&p); steady_t = 300;
    assert(steady(&p, 0.020, 120, -1) == 3);
    slot = p.slot; steady(&p, 0.020, 60, -1);
    assert(fabs(p.slot - slot - 60 * 3 * D90) < 1e-8);
    /* An explicitly uncapped engine may choose the 45 Hz divisor. */
    halo_frame_pacer_init(&p); steady_t = 400;
    for (unsigned i = 0; i < 120; i++) {
        in = input(steady_t + 0.020, 0.020, phase, HALO_PACER_EVEN);
        in.min_period = 1.0 / 60.0;
        steady_t = halo_frame_pacer_slot(&p, &in, NULL) + 0.0001;
    }
    assert(p.rung == 2);
    /* A scene/load gap and live disable immediately release the frame. */
    in = input(steady_t + 2, 0.020, phase, HALO_PACER_EVEN);
    assert(halo_frame_pacer_slot(&p, &in, NULL) == in.now && p.rung == 0);
    steady_t = in.now; steady(&p, 0.020, 120, phase);
    in = input(steady_t + 0.020, 0.020, phase, HALO_PACER_OFF);
    assert(halo_frame_pacer_slot(&p, &in, NULL) == in.now && p.rung == 0);
    /* OFF discards all stale timing measurements; ON must engage anew. */
    assert(p.win_count == 0 && !p.have_work && p.tick_cost == 0);
    in = input(in.now + 0.020, 0.020, phase, HALO_PACER_EVEN);
    assert(halo_frame_pacer_slot(&p, &in, NULL) == in.now && p.rung == 0);
    /* A severe hitch releases immediately rather than pacing using old load. */
    steady_t = in.now; steady(&p, 0.020, 120, phase);
    in = input(steady_t + 0.300, 0.300, phase, HALO_PACER_EVEN);
    assert(halo_frame_pacer_slot(&p, &in, NULL) == in.now && p.rung == 0);
    assert(p.win_count == 0 && !p.have_work);
    steady_t = in.now;
    assert(steady(&p, 0.020, 120, phase) == 3);
    /* A slow scene may recover to 30 Hz after sustained measured headroom. */
    halo_frame_pacer_init(&p); steady_t = 450;
    assert(steady(&p, 0.060, 240, phase) == 6);
    assert(steady(&p, 0.020, 600, phase) == 3);
    /* Borderline measured load must not switch every few frames. */
    halo_frame_pacer_init(&p); steady_t = 500;
    for (unsigned i = 0; i < 1800; i++) {
        double w = 0.028 + 0.0065 * (double)((i * 17) % 101) / 100;
        steady(&p, w, 1, phase);
    }
    assert(p.rung_changes <= 4);
    /* Median ignores empty slots, so a fresh GPU history has no zero bias. */
    uint64_t values[] = {0, 11100000, 0, 11200000, 22200000, 11000000};
    assert(halo_frame_pacer_median(values, COUNT(values)) == 11200000);
    puts("PASS pacer controller: 45/30/22.5/18/15 divisors, phase, late slip, low-fps fallback, off, load reset, hysteresis");
}

static void budget_checks(void) {
    HaloFramePacer p; halo_frame_pacer_init(&p);
    assert(halo_frame_pacer_budget_target(&p, D90, 0.033) == 0.033);
    p.rung = 4;
    for (unsigned i = 0; i < 48; i++) halo_frame_pacer_note(&p, 0.030, 0.030, -1);
    double target = halo_frame_pacer_budget_target(&p, D90, 0.033);
    assert(target > 1.0 / 30.0 && target <= 0.9 * 4 * D90 + 1e-9);
    HaloPanoramaBudget b; halo_panorama_budget_init(&b);
    assert(halo_panorama_budget_floor_busy(&b, 0.050f) == 0.050f);
    b.pass_ema = 0.015f; b.ppf_ema = 3.625f;
    assert(fabsf(halo_panorama_budget_floor_busy(&b, 0.050f) - 0.020f) < 1e-6f);
    /* The same busy observation earns extra bearings when pacing leaves room. */
    HaloPanoramaBudget normal, paced;
    halo_panorama_budget_init(&normal); halo_panorama_budget_init(&paced);
    normal.tier = paced.tier = 0; normal.extra_half = paced.extra_half = 1;
    for (unsigned i = 0; i < 120; i++) {
        halo_panorama_budget_observe_timed(&normal, 0.030f, 0.04f, 1.f / 30.f, 18);
        halo_panorama_budget_observe_timed(&paced, 0.030f, 0.04f, (float)target, 18);
    }
    assert(paced.extra_half > normal.extra_half);
    puts("PASS pacer budget: target exposes spare period and earns extra bearings");
}

/* Synthetic reconstruction: no phase, GPU latency or within-interval busy
 * distribution was recorded. Each interval's measured frame count is kept.
 * Below the cap, mean busy time is inferred as interval/frame count. At the
 * cap (>=29 Hz), the recorded busy EMA supplies an approximate mean; it is
 * not an interval average. Positive jitter weights are normalized per row,
 * preserving that inferred mean. The two runs consume identical work/GPU
 * samples in identical order and retain the observed bearing schedule.
 * No scaling of Build74 timings substitutes for actual Build75 timings.
 */
static uint64_t rng;
static double uniform(void) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return ((rng >> 11) + 0.5) / 9007199254740992.0;
}
static double normal(void) { return sqrt(-2 * log(uniform())) * cos(6.283185307179586 * uniform()); }

typedef struct {
    double fps, shown_fps, spread_ms, change_share, modal_share, game_speed, paced_share, late_share, changes_per_min;
    unsigned frames, shown, intervals, hist[128];
} SimResult;

static SimResult simulate(const TraceInterval *trace, size_t rows, unsigned seed, double sigma, int mode) {
    rng = 1 + (uint64_t)seed * 2654435761u;
    const double phase = uniform() * D90, gpu_mean = 0.004;
    HaloFramePacer p; halo_frame_pacer_init(&p);
    double release = 100, last_sample = release - 1.0 / 30.0, carry = 0;
    double total_time = 0, sum = 0, square = 0, total_ticks = 0;
    long last_latch = -1, previous_hold = -1;
    unsigned changes = 0, paced = 0, late = 0;
    SimResult r = {0};
    for (size_t row_index = 0; row_index < rows; row_index++) {
        const TraceInterval *row = &trace[row_index];
        assert(row->frames < 1024 && row->frames > 0 && row->seconds > 0);
        if (row->segment) {
            halo_frame_pacer_reset(&p);
            last_sample = release - 1.0 / 30.0;
            last_latch = -1; previous_hold = -1; carry = 0;
        }
        double weights[1024], gpu[1024], total = 0;
        for (unsigned j = 0; j < row->frames; j++) {
            weights[j] = exp(sigma * normal() - 0.5 * sigma * sigma);
            total += weights[j];
            gpu[j] = gpu_mean * (0.8 + 0.4 * uniform());
        }
        double observed_period = row->seconds / row->frames;
        double busy = row->frames / row->seconds >= 29.0 ? row->busy_ms * 0.001 : observed_period;
        if (busy > observed_period) busy = observed_period;
        if (busy < 0.001) busy = 0.001;
        for (unsigned j = 0; j < row->frames; j++) {
            double work = busy * weights[j] * row->frames / total;
            /* Native Halo limiter constrains successive simulation samples,
             * not successive presentation timestamps. It remains enabled. */
            double sample = fmax(release, last_sample + (double)0.0333333351f);
            double dt = fmin(sample - last_sample, 1.0 / 15.0);
            unsigned ticks = (unsigned)floor((dt + carry) * 30.0 + 1e-10);
            carry += dt - ticks / 30.0; total_ticks += ticks; last_sample = sample;
            double ready = sample + work;
            HaloFramePacerInput in = input(ready, work, phase, mode);
            /* Unknown tick-cost correlation: avoid inventing an estimator. */
            in.ticks = -1;
            uint64_t was_late = p.late_frames;
            double commit = halo_frame_pacer_slot(&p, &in, NULL);
            assert(isfinite(commit) && commit >= ready - 1e-9);
            assert(commit - ready <= 1.0 / 15.0 + D90);
            paced += p.rung != 0; late += p.late_frames != was_late;
            long latch = (long)ceil((commit + gpu[j] - phase) / D90);
            if (last_latch >= 0 && latch > last_latch) {
                long hold = latch - last_latch;
                if (previous_hold >= 0) changes += previous_hold != hold;
                previous_hold = hold;
                double ms = hold * D90 * 1000;
                sum += ms; square += ms * ms; r.intervals++;
                r.hist[hold < 128 ? hold : 127]++;
            }
            if (latch > last_latch) { last_latch = latch; r.shown++; }
            total_time += commit - release;
            release = commit;
            r.frames++;
        }
    }
    unsigned best = 0;
    for (unsigned j = 1; j < COUNT(r.hist); j++) if (r.hist[j] > best) best = r.hist[j];
    r.fps = r.frames / total_time; r.shown_fps = r.shown / total_time;
    double mean = sum / r.intervals;
    r.spread_ms = sqrt(fmax(0, square / r.intervals - mean * mean));
    r.change_share = (double)changes / r.intervals;
    r.modal_share = (double)best / r.intervals;
    r.game_speed = total_ticks / 30.0 / total_time;
    r.paced_share = (double)paced / r.frames; r.late_share = (double)late / r.frames;
    r.changes_per_min = p.rung_changes / (total_time / 60);
    return r;
}

static void simulation_checks(void) {
    struct { const char *name; const TraceInterval *trace; size_t rows; } cases[] = {
        {"Build75", build75_b30, COUNT(build75_b30)},
        {"Build74", build74_b30, COUNT(build74_b30)}
    };
    for (unsigned c = 0; c < COUNT(cases); c++) {
        double seconds = 0; unsigned frames = 0;
        for (size_t j = 0; j < cases[c].rows; j++) { seconds += cases[c].trace[j].seconds; frames += cases[c].trace[j].frames; }
        printf("%s actual aggregates: %zu intervals, %u frames / %.3f s = %.3f fps\n", cases[c].name, cases[c].rows, frames, seconds, frames / seconds);
        double worst_fps_ratio = 1.0, worst_shown_ratio = 1.0;
        double min_hold_ratio = INFINITY, max_hold_ratio = 0.0, worst_game_speed_delta = 0.0;
        for (unsigned noise = 0; noise < 3; noise++) {
            double sigma = 0.10 * noise;
            for (unsigned seed = 1; seed <= 3; seed++) {
                SimResult off = simulate(cases[c].trace, cases[c].rows, seed, sigma, HALO_PACER_OFF);
                SimResult on = simulate(cases[c].trace, cases[c].rows, seed, sigma, HALO_PACER_EVEN);
                if (seed == 1) printf("  SYNTHETIC sigma=%.2f: fps %.3f -> %.3f; hold SD %.3f -> %.3f ms; adjacent hold changes %.1f%% -> %.1f%%; modal %.1f%% -> %.1f%%; paced %.1f%% late %.1f%%; rung changes/min %.2f\n",
                    sigma, off.fps, on.fps, off.spread_ms, on.spread_ms, 100*off.change_share, 100*on.change_share,
                    100*off.modal_share, 100*on.modal_share, 100*on.paced_share, 100*on.late_share, on.changes_per_min);
                worst_fps_ratio = fmin(worst_fps_ratio, on.fps / off.fps);
                worst_shown_ratio = fmin(worst_shown_ratio, on.shown_fps / off.shown_fps);
                min_hold_ratio = fmin(min_hold_ratio, on.change_share / off.change_share);
                max_hold_ratio = fmax(max_hold_ratio, on.change_share / off.change_share);
                worst_game_speed_delta = fmin(worst_game_speed_delta, on.game_speed - off.game_speed);
                assert(on.frames == off.frames);
                assert(on.fps >= 0.90 * off.fps);
                assert(on.shown_fps >= 0.90 * off.shown_fps);
                /* Every tested Build75 reconstruction improves consecutive
                 * hold consistency, including the zero-jitter sensitivity
                 * case; global SD is deliberately not claimed to improve. */
                if (c == 0) assert(on.change_share <= 0.75 * off.change_share);
                else assert(on.fps >= 0.97 * off.fps);
                assert(on.game_speed >= off.game_speed - 0.03);
                assert(on.spread_ms <= off.spread_ms * 1.10 + 0.5);
                assert(on.changes_per_min <= 12);
            }
        }
        printf("  ALL 9 SYNTHETIC models: minimum fps ratio %.4f; minimum displayed-fps ratio %.4f; adjacent-hold-change ratio %.4f..%.4f; worst game-speed delta %.4f\n",
               worst_fps_ratio, worst_shown_ratio, min_hold_ratio, max_hold_ratio, worst_game_speed_delta);
    }
    puts("PASS trace-derived simulation: Build75 adjacent holds steadier in all 9 models; >=90% throughput, Build74 >=97%; bounded global variation");
}

int main(void) {
    controller_checks();
    budget_checks();
    simulation_checks();
    return 0;
}
