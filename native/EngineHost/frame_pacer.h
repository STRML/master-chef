#ifndef HALO_FRAME_PACER_H
#define HALO_FRAME_PACER_H
/* Optional publication pacing on a nominal 90 Hz display grid.
 *
 * The host sleeps before committing each completed panorama. A rung is a
 * whole number of display periods (2..6: 45, 30, 22.5, 18, 15 fps). Recent
 * busy times choose a sustainable rung with hysteresis; overload runs free.
 * Missed deadlines slip a display period rather than an entire engine frame.
 * GPU latency and the latest compositor sample establish a best-effort phase;
 * this is not a guarantee about physical scanout or OS wakeup precision.
 *
 * The bearing budget receives the selected period with jitter headroom. Rung
 * selection estimates its lightest existing schedule to avoid slowing down
 * simply because that budget spent spare time on extra bearings. It never
 * changes the contents of a bearing, guest time, or the guest frame limiter.
 *
 * The controller is platform-independent; frame_pacing_hooks.inc owns the
 * clock, blocking wait, idle accounting and synchronized report snapshot.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { HALO_PACER_OFF = 0, HALO_PACER_EVEN = 1 };
/* Rungs counted in the report: 0 free-running; 2..6 allowed. */
enum { HALO_PACER_RUNGS = 7 };

/* What the platform knows about the display when a frame is ready. */
typedef struct {
    uint64_t latch_ns;    /* when the compositor last took the newest frame; 0 unknown */
    uint64_t period_ns;   /* its frame period, from recent latches; 0 unknown (1/90 s) */
    uint64_t lag_ns;      /* commit to GPU completion, typical of recent frames; 0 unknown */
} HaloFramePacerDisplay;

/* For the device report. Cumulative counts; the rest as they stand. */
typedef struct {
    int32_t mode;               /* HALO_PACER_OFF / EVEN */
    uint32_t rung;              /* display frames per engine frame now; 0 free-running */
    uint32_t latch_locked;      /* 1 when the slots follow the compositor's own frames */
    float display_period_ms, observed_display_period_ms, target_period_ms, lead_ms, publish_lag_ms;
    float work_ms, floor_work_ms, budget_target_ms, late_share;
    uint64_t frames, paced_frames, late_frames, free_frames, rung_changes;
    uint64_t rung_frames[HALO_PACER_RUNGS];
    double wait_seconds, requested_wait_seconds;
} HaloFramePacerReport;

/* Implemented by the host (frame_pacing_hooks.inc). present() is called by
 * the platform on the engine thread just before it commits a finished world
 * frame; it waits for the frame's slot and returns when it let the frame go,
 * in CLOCK_UPTIME_RAW nanoseconds. */
uint64_t host_frame_pacer_present(const HaloFramePacerDisplay *display);
/* The busy-time ceiling the bearing budget should keep: the rung's, while
 * pacing, else the fallback (its own target frame rate's period). */
float host_frame_pacer_budget_target(float fallback_seconds);
void host_frame_pacer_report(HaloFramePacerReport *out);

/* ---- the controller ---- */

/* Recent frames a rung is judged by: two to three seconds of play. */
enum { HALO_PACER_WINDOW = 48 };
/* Maximum modeled period overhead, equivalent to 13.05% FPS loss.
 * This is a selection estimate, not a bound on scheduler/GPU stalls. */
#define HALO_PACER_ALLOWANCE 0.15
/* A rung that makes more than this share of frames take longer than Halo's
 * 1/15 s step would slow the game: those steps are clamped. */
#define HALO_PACER_SLOW_SHARE 0.1
/* Waits, in seconds of frames seen: to engage, to leave a rung that costs
 * too much, and to move to a smoother one that has room. */
#define HALO_PACER_ENGAGE_SECONDS 0.5
#define HALO_PACER_LEAVE_SECONDS 0.75
#define HALO_PACER_SMOOTHER_SECONDS 2.0
/* A frame has to reach its slot this much early to make it. */
#define HALO_PACER_MARGIN 0.0005

typedef struct {
    double now;             /* the frame has reached its publish point */
    double work;            /* its busy time since the last publish, less idling */
    double floor_work;      /* the same frame at the budget's lightest schedule */
    int ticks;              /* game ticks it ran; -1 unknown */
    double display_period;  /* compositor frame period; 0 for 1/90 s */
    double latch;           /* a recent compositor latch; 0 unknown or stale */
    double lead;            /* publish this long before a latch */
    double min_period;      /* the fastest supported cadence Halo's limiter allows */
    double max_period;      /* Halo's 1/15 s step, past which the game slows */
    int mode;
} HaloFramePacerInput;

typedef struct {
    unsigned rung;
    double slot, last_now;
    /* Recent frames: busy time, the same at the lightest schedule, ticks. */
    double win_work[HALO_PACER_WINDOW], win_floor[HALO_PACER_WINDOW];
    int win_ticks[HALO_PACER_WINDOW];
    unsigned win_count, win_next;
    /* What one game tick costs, from frames that ran different counts. */
    double tick_cost;
    /* Averages, for the report. */
    double work_ema, floor_ema;
    int have_work;
    /* Share of recent frames that missed their slot. */
    double late_ema;
    /* The rung the recent frames call for, and for how long they have. */
    unsigned want;
    double want_seconds, rung_seconds;
    uint64_t frames, paced_frames, late_frames, free_frames, rung_changes;
    uint64_t rung_frames[HALO_PACER_RUNGS];
    double wait_seconds;
} HaloFramePacer;

static inline void halo_frame_pacer_init(HaloFramePacer *p) { memset(p, 0, sizeof *p); }

/* Forget the timing, keep the counts: a load, a menu, pacing switched off. */
static inline void halo_frame_pacer_reset(HaloFramePacer *p) {
    p->rung = p->want = 0; p->slot = p->last_now = 0.0;
    p->win_count = p->win_next = 0; p->have_work = 0;
    p->work_ema = p->floor_ema = p->late_ema = 0.0;
    p->want_seconds = p->rung_seconds = p->tick_cost = 0.0;
}

static inline double halo_frame_pacer_display_period(const HaloFramePacerInput *in) {
    return in->display_period > 0.004 && in->display_period < 0.05 ? in->display_period : 1.0 / 90.0;
}

/* Only the requested 90 Hz divisors, respecting Halo's own limiter. */
static inline int halo_frame_pacer_allowed(unsigned n, const HaloFramePacerInput *in) {
    double period = (double)n * halo_frame_pacer_display_period(in);
    return n >= 2 && n < HALO_PACER_RUNGS && period >= in->min_period * 0.99 &&
           period <= in->max_period * 1.00001;
}

static inline void halo_frame_pacer_note(HaloFramePacer *p, double work, double floor_work, int ticks) {
    unsigned i = p->win_next++ % HALO_PACER_WINDOW;
    p->win_work[i] = work; p->win_floor[i] = floor_work; p->win_ticks[i] = ticks;
    if (p->win_count < HALO_PACER_WINDOW) p->win_count++;
    /* The tick cost from the two commonest tick counts in the window. A rung
     * that runs one count only (30 or 15 fps) keeps the last estimate. */
    double sum[4] = { 0 }; unsigned count[4] = { 0 };
    for (unsigned j = 0; j < p->win_count; j++)
        if (p->win_ticks[j] >= 0 && p->win_ticks[j] < 4) { sum[p->win_ticks[j]] += p->win_floor[j]; count[p->win_ticks[j]]++; }
    int a = -1, b = -1;
    for (int k = 0; k < 4; k++) {
        if (count[k] < 6) continue;
        if (a < 0 || count[k] > count[a]) { b = a; a = k; }
        else if (b < 0 || count[k] > count[b]) b = k;
    }
    if (a >= 0 && b >= 0) {
        double cost = (sum[a] / count[a] - sum[b] / count[b]) / (double)(a - b);
        if (cost < 0.0) cost = 0.0;
        p->tick_cost = p->tick_cost > 0.0 ? p->tick_cost + 0.1 * (cost - p->tick_cost) : cost;
    }
}

/* What pacing at rung n would cost the recent frames at the lightest
 * schedule. A rung sets how many ticks a frame runs, the floor or ceiling of
 * its period in ticks (22.5 fps runs 1, 1, 2), so each frame is first moved
 * to those counts at the measured tick cost: a two-tick frame seen at 15 fps
 * says little about 30 fps, where it would have run one. Then each takes the
 * rung's period, or as many display frames as it needs if it runs over.
 * *loss is the period overhead against the same frames run free
 * (never faster than Halo's cap); *slow the share that would take longer than
 * Halo's 1/15 s step; *late the share that would miss the rung's period. */
static inline void halo_frame_pacer_cost(const HaloFramePacer *p, unsigned n, const HaloFramePacerInput *in,
                                         double *loss, double *slow, double *late) {
    *loss = 1.0; *slow = 1.0; if (late) *late = 1.0;
    if (!p->win_count) return;
    double D = halo_frame_pacer_display_period(in), period = (double)n * D, ticks = period * 30.0;
    int low = (int)floor(ticks + 0.02);
    double high_share = ticks - (double)low;
    if (high_share < 0.02) high_share = 0.0;
    double mean_floor = 0.0;
    for (unsigned j = 0; j < p->win_count; j++) mean_floor += p->win_floor[j];
    mean_floor /= (double)p->win_count;
    double cost = p->tick_cost > 0.0 ? p->tick_cost : 0.15 * mean_floor;
    double paced = 0.0, free_run = 0.0, slow_share = 0.0, late_share = 0.0;
    for (unsigned j = 0; j < p->win_count; j++) {
        for (int high = 0; high <= 1; high++) {
            double weight = high ? high_share : 1.0 - high_share;
            if (weight <= 0.0) continue;
            int k = p->win_ticks[j];
            double w = p->win_floor[j] + (k >= 0 ? (double)(low + high - k) * cost : 0.0) + HALO_PACER_MARGIN;
            double frames = ceil(w / D - 1e-9);
            if (frames < (double)n) frames = (double)n;
            paced += weight * frames * D;
            free_run += weight * (w > in->min_period ? w : in->min_period);
            if (w > in->max_period) slow_share += weight;
            if (w > period) late_share += weight;
        }
    }
    *loss = paced / free_run - 1.0;
    *slow = slow_share / (double)p->win_count;
    if (late) *late = late_share / (double)p->win_count;
}

/* The smoothest rung the recent frames can afford: the slowest allowed one
 * whose cost is within the allowance and which does not slow the game. 0,
 * free-running, when none is. */
static inline unsigned halo_frame_pacer_choose(const HaloFramePacer *p, const HaloFramePacerInput *in, double allowance) {
    for (unsigned n = HALO_PACER_RUNGS - 1u; n >= 1u; n--) {
        if (!halo_frame_pacer_allowed(n, in)) continue;
        double loss, slow;
        halo_frame_pacer_cost(p, n, in, &loss, &slow, NULL);
        if (loss <= allowance && slow <= HALO_PACER_SLOW_SHARE) return n;
    }
    return 0;
}

/* The bearing budget's busy-time ceiling at the current rung. The budget
 * steers the average frame, so the ceiling leaves the rung's period less the
 * margin and less how far the heavier frames run over the average (the 88th
 * percentile of recent ones: the tick that every third frame runs at 22.5 fps,
 * the extra pass of the heavy tiers' alternate frames). While frames are
 * missing their slots it comes down further. Between six and nine tenths of
 * the period; the plain target when not pacing. */
static inline double halo_frame_pacer_budget_target(const HaloFramePacer *p, double display_period, double fallback) {
    if (!p->rung) return fallback;
    if (!(display_period > 0.004 && display_period < 0.05)) display_period = 1.0 / 90.0;
    double period = (double)p->rung * display_period, excess = 0.0;
    if (p->win_count >= 8) {
        double sorted[HALO_PACER_WINDOW], mean = 0.0;
        unsigned n = p->win_count;
        for (unsigned i = 0; i < n; i++) {
            double x = p->win_work[i]; unsigned j = i;
            mean += x;
            while (j && sorted[j - 1] > x) { sorted[j] = sorted[j - 1]; j--; }
            sorted[j] = x;
        }
        mean /= (double)n;
        excess = sorted[(unsigned)(0.88 * (double)(n - 1))] - mean;
        if (excess < 0.0) excess = 0.0;
    }
    double target = period - 2.0 * HALO_PACER_MARGIN - excess;
    if (p->late_ema > 0.1) target -= 0.5 * period * (p->late_ema - 0.1);
    if (target > 0.9 * period) target = 0.9 * period;
    if (target < 0.6 * period) target = 0.6 * period;
    return target;
}

static inline void halo_frame_pacer_set_rung(HaloFramePacer *p, unsigned rung) {
    if (rung == p->rung) return;
    p->rung = rung; p->rung_changes++;
    p->rung_seconds = 0.0; p->late_ema = 0.0;
}

/* One frame at its publish point: returns when to publish it, which is now
 * when free-running or when it has missed its slot and the display phase is
 * unknown. *shortened is set when the frame period got shorter this frame (a
 * faster rung, or free-running again), so the caller can bring the budget
 * down to it at once. */
static inline double halo_frame_pacer_slot(HaloFramePacer *p, const HaloFramePacerInput *in, int *shortened) {
    if (shortened) *shortened = 0;
    double D = halo_frame_pacer_display_period(in);
    double elapsed = p->last_now > 0.0 && in->now > p->last_now ? in->now - p->last_now : 0.0;
    /* A load, severe hitch, disabled mode or invalid sample must not keep
     * pacing from an old low-cost window. Re-engagement starts fresh. */
    if (elapsed > 1.0 || in->work >= 0.25 || in->work < 0.0 || !isfinite(in->work) ||
        in->mode == HALO_PACER_OFF) {
        if (p->rung) { p->rung_changes++; if (shortened) *shortened = 1; }
        halo_frame_pacer_reset(p);
        elapsed = 0.0;
    }
    p->last_now = in->now;
    p->frames++;
    /* Measure ordinary frames; hitch samples above reset the controller. */
    double floor_work = in->floor_work > 0.0 && in->floor_work < in->work ? in->floor_work : in->work;
    if (in->mode != HALO_PACER_OFF && in->work > 0.0 && in->work < 0.25) {
        halo_frame_pacer_note(p, in->work, floor_work, in->ticks);
        if (!p->have_work) { p->work_ema = in->work; p->floor_ema = floor_work; p->have_work = 1; }
        else { p->work_ema += 0.1 * (in->work - p->work_ema); p->floor_ema += 0.1 * (floor_work - p->floor_ema); }
    }
    if (!p->have_work || in->mode == HALO_PACER_OFF) {
        if (p->rung) { halo_frame_pacer_set_rung(p, 0); if (shortened) *shortened = 1; }
        p->slot = 0.0; p->free_frames++; p->rung_frames[0]++;
        return in->now;
    }

    /* Choose the rung: what the recent frames call for, once they have
     * called for it long enough. Leaving a rung that costs clearly too much
     * frame rate, or slows the game, is quick; moving to a smoother one with
     * room to spare waits, and the margins either side keep a scene on the
     * edge where it is. */
    unsigned before = p->rung;
    p->rung_seconds += elapsed;
    unsigned want = halo_frame_pacer_choose(p, in, HALO_PACER_ALLOWANCE);
    if (want == p->want) p->want_seconds += elapsed; else { p->want = want; p->want_seconds = 0.0; }
    if (!p->rung) {
        if (want && p->want_seconds >= HALO_PACER_ENGAGE_SECONDS) halo_frame_pacer_set_rung(p, want);
    } else if (!halo_frame_pacer_allowed(p->rung, in)) {
        /* The display period or Halo's limiter moved under it. */
        halo_frame_pacer_set_rung(p, want);
    } else if (want != p->rung) {
        double loss, slow;
        halo_frame_pacer_cost(p, p->rung, in, &loss, &slow, NULL);
        if (loss > HALO_PACER_ALLOWANCE + 0.05 || slow > 2.0 * HALO_PACER_SLOW_SHARE) {
            if (p->want_seconds >= HALO_PACER_LEAVE_SECONDS) halo_frame_pacer_set_rung(p, want);
        } else if (want > p->rung && p->want_seconds >= HALO_PACER_SMOOTHER_SECONDS &&
                   p->rung_seconds >= HALO_PACER_SMOOTHER_SECONDS) {
            halo_frame_pacer_cost(p, want, in, &loss, &slow, NULL);
            if (loss <= HALO_PACER_ALLOWANCE - 0.03) halo_frame_pacer_set_rung(p, want);
        }
    }
    if (shortened && p->rung != before && (!p->rung || p->rung < before)) *shortened = 1;
    if (!p->rung) {
        p->slot = 0.0; p->free_frames++; p->rung_frames[0]++;
        return in->now;
    }

    /* The slot: a rung's period after the last one, on the compositor's grid
     * when its phase is known. A frame that has missed it goes at the next
     * grid point, a compositor frame late rather than a whole period, and the
     * cadence carries on from there. */
    const double guard = 0.0002;   /* waking takes about this long */
    double period = (double)p->rung * D;
    int locked = in->latch > 0.0;
    double phase = locked ? in->latch - in->lead : 0.0;
    double target = p->slot > 0.0 ? p->slot + period : 0.0;
    if (target > 0.0 && locked) target = phase + floor((target - phase) / D + 0.5) * D;
    int late = 0;
    if (target < in->now + guard) {
        late = target > 0.0;
        target = locked ? phase + ceil((in->now + guard - phase) / D) * D : in->now;
    }
    /* Never hold a frame for longer than its own period: that is a clock
     * that moved, not a frame that is early. */
    if (target > in->now + period + D) target = in->now;
    p->late_ema += 0.05 * ((late ? 1.0 : 0.0) - p->late_ema);
    if (late) p->late_frames++;
    p->slot = target; p->paced_frames++; p->rung_frames[p->rung]++;
    p->wait_seconds += target - in->now;
    return target;
}

/* The median of the non-zero values, in place; 0 when there are none. */
static inline uint64_t halo_frame_pacer_median(uint64_t *v, unsigned n) {
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) if (v[i]) v[k++] = v[i];
    for (unsigned i = 1; i < k; i++) {
        uint64_t x = v[i]; unsigned j = i;
        while (j && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
    return k ? v[k / 2] : 0;
}

#ifdef __cplusplus
}
#endif
#endif
