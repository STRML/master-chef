#ifndef HALO_PANORAMA_BUDGET_H
#define HALO_PANORAMA_BUDGET_H
#include "panorama.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct { int layer; float yaw, pitch; int eye; } HaloPanoramaView;

/* Which bearings to draw this frame, and how many.
 *
 * The bearing nearest the predicted gaze is drawn every frame. The prediction
 * leads the measured turn by a few frames so a turn reaches a fresh picture.
 * Tier zero also refreshes the visible neighbours every frame to limit stale
 * joins. Heavy tiers stagger those neighbours to save engine passes; their
 * joins can therefore show pictures from different engine frames. The centre
 * is always fresh. At tiers zero and one both centre eyes are drawn together;
 * at tiers two and three one centre picture is shared between the eyes.
 *
 * Everything else competes for extra passes, whose budget follows measured
 * busy time (the frame less what its own limiter spent idling). The floor is
 * one extra bearing every other frame until tier three, where it becomes one
 * every eighth frame. Age weighted by nearness prioritises those extras, so
 * distant bearings still rotate while the viewer's gaze stays fresh. */
typedef struct {
    uint64_t last_drawn[HALO_PANORAMA_LAYERS];
    float previous_yaw, yaw_rate, previous_pitch, pitch_rate;
    int have_previous;
    /* Extra passes per two frames: 1 is one distant bearing every other
     * frame, the floor. The ceiling is every remaining bearing every frame. */
    unsigned extra_half;
    float busy_ema;
    /* Time since the last adjustment and since the last tier change, in
     * seconds of measured frame period. Counting frames made every wait
     * three times longer at 11 fps than at 30: Build74 took 24 s to shed the
     * menu's load in Silent Cartographer, a tier every eight seconds. */
    float since_adjust, tier_dwell;
    /* The heavy tiers. A level whose single pass costs a third of the frame
     * cannot reach the target at the floor budget however few extras it
     * gets: on the headset one pass of Silent Cartographer is 15 ms, and
     * the floor's centre pair plus two neighbours plus half an extra is 4.7
     * passes and 9 fps. When the floor still runs over the target the budget
     * steps up a tier, each lighter than the last: 1 draws one neighbour a
     * frame instead of both, 2 also draws the centre once for both eyes,
     * 3 draws a neighbour every other frame and a distant bearing every
     * eighth. The forward view keeps its frame rate; the sides refresh
     * slower. Each step waits ninety frames, and coming back down needs the
     * busy time well under the target, so a scene on the edge does not
     * flicker between tiers. Extras only grow again at tier 0. */
    unsigned tier, tier_pressure, tier_relief;
    /* What one bearing pass costs and how many a frame are drawn, averaged:
     * a lighter tier adds about one pass a frame, and a heavier one's cost
     * can be predicted from its floor. Zero until measured. */
    float pass_ema, ppf_ema;
    /* The tier the last level settled at, where the next one starts. */
    unsigned level_tier; int have_level_tier;
    /* Advances on the existing scene-entry rule (front end closes or a
     * >1 s load gap), so pacing can discard the previous scene's timings. */
    uint64_t scene_epoch;
    /* Choose the cap-free odd slot once per cycle. Latching it prevents a
     * moving head from postponing every distant refresh by switching slots. */
    unsigned distant_phase;
} HaloPanoramaBudget;

enum { HALO_PANORAMA_BUDGET_LOOKAHEAD = 3, HALO_PANORAMA_TIER_MAX = 3,
       HALO_PANORAMA_TIER_UP_PERIODS = 3, HALO_PANORAMA_TIER_DOWN_PERIODS = 6 };
/* Waits in seconds of measured frame period: adjust the extras every quarter
 * second; a heavier tier needs a second at the previous one, a lighter tier
 * three, and the step back to stereo (tier two to one), which is visible, six. */
#define HALO_PANORAMA_BUDGET_ADJUST_SECONDS 0.25f
#define HALO_PANORAMA_TIER_UP_SECONDS 1.0f
#define HALO_PANORAMA_TIER_DOWN_SECONDS 3.0f
#define HALO_PANORAMA_STEREO_DWELL_SECONDS 6.0f
/* Passes a frame each tier's floor draws looking ahead (tier 3: 13 in 8). */
static const float halo_panorama_tier_floor_ppf[HALO_PANORAMA_TIER_MAX + 1] = { 4.5f, 3.5f, 2.6f, 1.625f };

/* Off the gaze bearing's middle by more than this, the neighbour on that side
 * is in front of the eyes too: about twelve degrees. */
#define HALO_PANORAMA_GAZE_SEAM_RADIANS 0.21f

/* At tiers two and three the centre is drawn once and shown to both eyes. */
static inline int halo_panorama_budget_mono(const HaloPanoramaBudget *b) { return b->tier >= 2; }

static inline void halo_panorama_budget_init(HaloPanoramaBudget *b) {
    memset(b, 0, sizeof *b);
    b->extra_half = 1;
    b->distant_phase = 1;
}

/* Cosine of the angle between a view's centre and the gaze: 1 dead ahead. */
static inline float halo_panorama_budget_score(const HaloPanoramaView *v, float yaw, float pitch) {
    float delta = remainderf(v->yaw - yaw, 2.f * (float)M_PI);
    return cosf(pitch) * cosf(v->pitch) * cosf(delta) + sinf(pitch) * sinf(v->pitch);
}

/* One frame's bearing passes: their total time and count. */
static inline void halo_panorama_budget_note_passes(HaloPanoramaBudget *b, float seconds, unsigned passes) {
    if (!passes || !(seconds > 0.f) || seconds > 1.f) return;
    float each = seconds / (float)passes;
    b->pass_ema = b->pass_ema > 0.f ? b->pass_ema + 0.1f * (each - b->pass_ema) : each;
    b->ppf_ema = b->ppf_ema > 0.f ? b->ppf_ema + 0.1f * ((float)passes - b->ppf_ema) : (float)passes;
}

/* A level was entered (the front end closed, or a load stalled the frame).
 * The menu's measurements say nothing about the level: Build74 carried the
 * menu's tier 0 and full extras into Silent Cartographer and drew nine
 * passes a frame at 3-11 fps. Start at the tier the last level settled at,
 * or the heaviest, and let relief bring it down once the level is measured. */
static inline void halo_panorama_budget_scene_entry(HaloPanoramaBudget *b) {
    b->scene_epoch++;
    unsigned start = b->have_level_tier ? b->level_tier : HALO_PANORAMA_TIER_MAX;
    if (start < 2) start = 2;
    b->tier = start; b->extra_half = 1;
    b->busy_ema = b->pass_ema = b->ppf_ema = 0.f;
    b->tier_pressure = b->tier_relief = 0;
    b->since_adjust = b->tier_dwell = 0.f;
}
/* Remember where play settles, for the next entry. */
static inline void halo_panorama_budget_note_level(HaloPanoramaBudget *b) {
    b->level_tier = b->tier; b->have_level_tier = 1;
}

/* One frame's busy time. Every few frames the extra budget moves one step
 * toward the target: up when the engine is comfortably inside it, down when
 * it runs over. Anything over a second is a load, not a frame. */
static inline void halo_panorama_budget_observe_timed(HaloPanoramaBudget *b, float busy_seconds, float period_seconds,
                                                      float target_seconds, unsigned extra_ceiling) {
    if (!(busy_seconds > 0.f) || busy_seconds > 1.f || !(target_seconds > 0.f)) return;
    if (!(period_seconds > 0.f) || period_seconds > 1.f)
        period_seconds = busy_seconds > target_seconds ? busy_seconds : target_seconds;
    b->busy_ema = b->busy_ema > 0.f ? b->busy_ema + 0.2f * (busy_seconds - b->busy_ema)
                                    : busy_seconds;
    if (b->tier_dwell < 1.0e6f) b->tier_dwell += period_seconds;
    b->since_adjust += period_seconds;
    if (b->since_adjust < HALO_PANORAMA_BUDGET_ADJUST_SECONDS) return;
    b->since_adjust = 0.f;
    /* The target is a ceiling on busy time, not a centre: the budget grows
     * only with a fifth of the frame to spare and gives way as soon as the
     * engine runs over, so the frame rate it settles at is the target or
     * better, never a band below it. */
    if (b->tier == 0 && b->busy_ema < target_seconds * 0.80f && b->extra_half < extra_ceiling) b->extra_half++;
    else if (b->busy_ema > target_seconds && b->extra_half > 1) {
        /* Costly rooms can add several milliseconds to every bearing. Price
         * the excess in measured passes instead of removing only half a
         * pass per quarter second. Keep 10% headroom for the game tick and
         * pass-to-pass variation; only optional views are shed here. The
         * mandatory gaze, stereo and seam coverage still follow the same
         * tier rules, and growing back still needs the slow relief path. */
        unsigned drop = 1;
        if (b->pass_ema > 0.f && isfinite(b->pass_ema)) {
            float half_passes = ceilf(2.f * (b->busy_ema - target_seconds * 0.90f) / b->pass_ema);
            /* Bound the float before converting, including tiny measured
             * pass costs which can overflow the quotient to infinity. */
            drop = half_passes >= (float)(b->extra_half - 1) ? b->extra_half - 1
                   : half_passes > 1.f ? (unsigned)half_passes : 1;
        } else if (b->busy_ema > target_seconds * 1.25f && b->extra_half > 2) {
            drop = b->extra_half - b->extra_half / 2;
        }
        b->extra_half -= drop;
    }
    /* The tiers: pressure is the floor still over the target; relief is room
     * for the lighter tier. Once a pass has been timed, relief means one more
     * pass a frame still fits under nine tenths of the target. Before that,
     * or without timing, it is the busy time well under the target. Waiting
     * for 60% alone never came: at Halo's 30 fps cap the busy time read 24-30
     * ms whatever the pass count, so once a heavy scene took the budget to
     * mono it stayed mono for the rest of the session. Either has to persist,
     * and a change has to have had its dwell. */
    int relief = b->pass_ema > 0.f ? b->busy_ema + b->pass_ema < target_seconds * 0.90f
                                   : b->busy_ema < target_seconds * 0.60f;
    if (b->extra_half == 1 && b->busy_ema > target_seconds) { b->tier_pressure++; b->tier_relief = 0; }
    else if (relief) { b->tier_relief++; b->tier_pressure = 0; }
    else { b->tier_pressure = 0; b->tier_relief = 0; }
    /* Far over the target at the floor: jump straight to the lightest tier
     * whose floor is predicted to fit, rather than a step per dwell. */
    if (b->extra_half == 1 && b->busy_ema > 1.5f * target_seconds && b->tier < HALO_PANORAMA_TIER_MAX &&
        b->tier_dwell >= HALO_PANORAMA_BUDGET_ADJUST_SECONDS) {
        unsigned t = b->tier + 1;
        if (b->pass_ema > 0.f && b->ppf_ema > 0.f)
            while (t < HALO_PANORAMA_TIER_MAX &&
                   b->busy_ema - b->pass_ema * (b->ppf_ema - halo_panorama_tier_floor_ppf[t]) > target_seconds) t++;
        else t = HALO_PANORAMA_TIER_MAX;
        b->tier = t; b->tier_dwell = 0.f; b->tier_pressure = 0;
        return;
    }
    if (b->tier_pressure >= HALO_PANORAMA_TIER_UP_PERIODS && b->tier < HALO_PANORAMA_TIER_MAX &&
        b->tier_dwell >= HALO_PANORAMA_TIER_UP_SECONDS) {
        b->tier++; b->tier_dwell = 0.f; b->tier_pressure = 0;
    } else if (b->tier_relief >= HALO_PANORAMA_TIER_DOWN_PERIODS && b->tier > 0 &&
               b->tier_dwell >= (b->tier == 2 ? HALO_PANORAMA_STEREO_DWELL_SECONDS : HALO_PANORAMA_TIER_DOWN_SECONDS)) {
        b->tier--; b->tier_dwell = 0.f; b->tier_relief = 0;
    }
}

/* One frame at the target's period, for callers without a measured one. */
static inline void halo_panorama_budget_observe(HaloPanoramaBudget *b, float busy_seconds,
                                                float target_seconds, unsigned extra_ceiling) {
    halo_panorama_budget_observe_timed(b, busy_seconds, 0.f, target_seconds, extra_ceiling);
}

static inline uint32_t halo_panorama_budget_plan(HaloPanoramaBudget *b, const HaloPanoramaView *views,
                                                 unsigned count, int stereo, uint64_t frame,
                                                 float yaw, float pitch) {
    /* Head turn per frame from the last two gazes, and where it will be. */
    float rate = 0.f;
    float pitch_rate = 0.f;
    if (b->have_previous) rate = remainderf(yaw - b->previous_yaw, 2.f * (float)M_PI);
    if (b->have_previous) pitch_rate = pitch - b->previous_pitch;
    b->previous_yaw = yaw; b->have_previous = 1;
    b->previous_pitch = pitch;
    if (rate > 0.35f) rate = 0.35f;
    if (rate < -0.35f) rate = -0.35f;
    b->yaw_rate = rate;
    b->pitch_rate = fminf(0.35f, fmaxf(-0.35f, pitch_rate));
    float ahead = yaw + rate * (float)HALO_PANORAMA_BUDGET_LOOKAHEAD;

    float score[HALO_PANORAMA_LAYERS], best = -2.f;
    uint32_t active = 0;
    for (unsigned n = 0; n < count && n < HALO_PANORAMA_LAYERS; n++) {
        int layer = views[n].layer;
        score[n] = -2.f;
        if (!stereo && layer == HALO_PANORAMA_CENTRE_RIGHT) continue;
        active |= 1u << layer;
        score[n] = halo_panorama_budget_score(&views[n], ahead, pitch);
        if (score[n] > best) best = score[n];
    }
    if (count > HALO_PANORAMA_LAYERS) count = HALO_PANORAMA_LAYERS;
    /* The ring bearing nearest the predicted gaze, and how far off its
     * middle the gaze is: past about twelve degrees a join is in view. */
    int gaze = -1; float gaze_score = -2.f, gaze_off = 0.f;
    for (unsigned n = 0; n < count; n++)
        if ((active & (1u << views[n].layer)) && views[n].pitch == 0.f && score[n] > gaze_score) { gaze = (int)n; gaze_score = score[n]; }
    if (gaze >= 0) gaze_off = remainderf(ahead - views[gaze].yaw, 2.f * (float)M_PI);
    int seam_in_view = gaze >= 0 && fabsf(gaze_off) > HALO_PANORAMA_GAZE_SEAM_RADIANS;

    /* Mandatory: the predicted gaze bearing, the ring bearings within
     * ninety degrees of it (the caps only when the head is pitched toward
     * them), and the centre pair. */
    uint32_t draw = 1u << HALO_PANORAMA_CENTRE_LEFT;
    if (stereo) draw |= 1u << HALO_PANORAMA_CENTRE_RIGHT;
    for (unsigned n = 0; n < count; n++) {
        int layer = views[n].layer;
        uint32_t bit = 1u << layer;
        if (!(active & bit)) continue;
        if (score[n] >= best - 0.00001f) { draw |= bit; continue; }
        float delta = remainderf(views[n].yaw - ahead, 2.f * (float)M_PI);
        int cap = layer == HALO_PANORAMA_UP || layer == HALO_PANORAMA_DOWN;
        int beside = layer == HALO_PANORAMA_UP ? pitch > 0.5f
                   : layer == HALO_PANORAMA_DOWN ? pitch < -0.5f
                   : fabsf(delta) < 1.5707963268f;
        /* The heavy tiers take the neighbours in turns: one a frame at tiers
         * one and two, one every other frame at tier three, the caps in the
         * odd slots when the head is pitched toward them. */
        if (beside && b->tier >= 1) {
            unsigned phase = (unsigned)(frame & 3u);
            if (b->tier < 3) beside = cap ? (frame & 1u) : ((delta > 0.f) == ((frame & 1u) == 0u));
            else beside = cap ? (layer == HALO_PANORAMA_UP ? phase == 1u : phase == 3u)
                              : (delta > 0.f ? phase == 0u : phase == 2u);
            /* At tier three with a join in view, the neighbour on that side is
             * drawn on even frames below, and the other one, mostly out of view,
             * waits for the distant slot: never more than two passes a frame. */
            if (b->tier >= 3 && seam_in_view && !cap) beside = 0;
        }
        if (beside) draw |= bit;
    }

    /* The join in front of the eyes. Off the gaze bearing's middle, the ring
     * neighbour on that side is in view too, so tiers one and two draw it
     * every frame instead of in turns, and tier three every other frame: the
     * seam the viewer is looking at pairs pictures at most a frame apart. In
     * turns it was up to three frames behind at tier three, which at a brisk
     * stick turn is several degrees of misalignment and a flash of stale
     * lighting across the join. Looking straight ahead adds nothing. */
    if (b->tier >= 1 && seam_in_view && !(b->tier >= 3 && (frame & 1u))) {
        float want = views[gaze].yaw + (gaze_off > 0.f ? 1.0471975512f : -1.0471975512f);
        for (unsigned n = 0; n < count; n++)
            if ((active & (1u << views[n].layer)) && views[n].pitch == 0.f &&
                fabsf(remainderf(views[n].yaw - want, 2.f * (float)M_PI)) < 0.1f)
                draw |= 1u << views[n].layer;
    }

    /* The rest, stalest and nearest first. */
    unsigned order[HALO_PANORAMA_LAYERS], rest = 0;
    float priority[HALO_PANORAMA_LAYERS];
    for (unsigned n = 0; n < count; n++) {
        int layer = views[n].layer;
        uint32_t bit = 1u << layer;
        if (!(active & bit) || (draw & bit)) continue;
        uint64_t since = frame > b->last_drawn[layer] ? frame - b->last_drawn[layer] : 1u;
        if (since > 64u) since = 64u;
        /* Age dominates over time, with a bounded nearness weight, so the bearing
         * behind the head still comes round within about seventeen frames
         * at the tier-zero floor budget rather than waiting on the ones beside it. */
        float near = 0.5f * (score[n] + 1.f);   /* 0 behind the head, 1 ahead */
        float p = (float)since * (0.7f + 0.3f * near);
        unsigned j = rest;
        while (j && priority[j - 1] < p) { order[j] = order[j - 1]; priority[j] = priority[j - 1]; j--; }
        order[j] = n; priority[j] = p; rest++;
    }
    /* Anti-windup: a full sphere can spend only the optional views left
     * after this frame's mandatory coverage. Keeping credits above that
     * count made overload adjustments do no real work for up to seconds.
     * Retain the one-half-pass floor even when every view is mandatory;
     * tier pressure and starvation guarantees use that sentinel. */
    unsigned useful_half = rest ? 2u * rest : 1u;
    if (b->extra_half > useful_half) b->extra_half = useful_half;
    unsigned take = b->extra_half / 2u + (((b->extra_half & 1u) && (frame & 1u)) ? 1u : 0u);
    /* Odd slots avoid the ring neighbours. Of those, choose the one without
     * the cap toward the current gaze, so the distant pass fills a lighter
     * frame instead of stacking on a neighbour/cap. Keep this choice for the
     * whole eight-frame cycle: following live pitch every frame could skip
     * both slots on a head turn and starve the distant bearings. */
    if (b->tier >= 3) {
        if ((frame & 7u) == 0u) b->distant_phase = pitch > 0.5f ? 3u : 1u;
        take = (frame & 7u) == b->distant_phase ? 1u : 0u;
    }
    if (take > rest) take = rest;
    for (unsigned i = 0; i < take; i++) draw |= 1u << views[order[i]].layer;
    return draw;
}

/* During game-camera or head motion, spend measured spare time on visible stale
 * neighbours before leaving it idle in a heavy tier's relief dwell. This
 * does not enable geometric warping or lower the frame-rate target. Account
 * for the passes already planned (including optional distant refreshes),
 * and retain 10% headroom. At most two additional views can be admitted. */
static inline uint32_t halo_panorama_budget_motion_fill(const HaloPanoramaBudget *b,
        const HaloPanoramaView *views, unsigned count, int stereo, uint32_t draw,
        float yaw, float pitch, float target_seconds) {
    if (!(b->pass_ema>0.f) || !(b->busy_ema>0.f) || !(b->ppf_ema>0.f) ||
        !isfinite(b->pass_ema) || !isfinite(b->busy_ema) || !isfinite(b->ppf_ema) ||
        !(target_seconds>0.f) || !isfinite(target_seconds)) return draw;
    float other=b->busy_ema-b->pass_ema*b->ppf_ema;
    if(other<0.f)other=0.f;
    unsigned passes=(unsigned)__builtin_popcount(draw);
    for(unsigned extra=0;extra<2;extra++) {
        if(other+b->pass_ema*(float)(passes+1)>target_seconds*0.90f)break;
        int best=-1;float score=0.35f;
        for(unsigned n=0;n<count && n<HALO_PANORAMA_LAYERS;n++) {
            int layer=views[n].layer;
            if(layer<0 || layer>=HALO_PANORAMA_LAYERS || (draw&(1u<<layer)) ||
                (!stereo && layer==HALO_PANORAMA_CENTRE_RIGHT))continue;
            float near=halo_panorama_budget_score(&views[n],yaw,pitch);
            /* The cap centres are 90 degrees away when looking ahead, but
             * their edges overlap the visible ring. A centre-only cosine
             * threshold excluded both caps and left the floor/ceiling on an
             * old camera while the walls caught up. Admit that overlap with
             * lower priority than the front neighbours, and alternate equal
             * priorities by age. Do not spend time on the opposite cap when
             * looking steeply up/down. This is conservative visibility, not
             * a promise to draw every panel regardless of cost. */
            if(fabsf(views[n].pitch)>1.5f && near>-.15f && near<.36f)near=.36f;
            if(near>score || (best>=0 && near==score && b->last_drawn[layer]<b->last_drawn[best])){
                score=near;best=layer;
            }
        }
        if(best<0)break;
        draw|=1u<<best;passes++;
    }
    return draw;
}

/* Record what was actually drawn, so staleness counts real pictures. */
static inline void halo_panorama_budget_drawn(HaloPanoramaBudget *b, uint32_t mask, uint64_t frame) {
    for (int layer = 0; layer < HALO_PANORAMA_LAYERS; layer++)
        if (mask & (1u << layer)) b->last_drawn[layer] = frame;
}

/* What a frame that was busy this long would cost at the lightest tier's
 * floor: the passes it drew over that floor are what the budget could still
 * shed. The frame pacer (frame_pacer.h) chooses its cadence from this, so it
 * never settles on a slower cadence only because the budget filled a faster
 * one with extra bearings. Unchanged until a pass has been timed. */
static inline float halo_panorama_budget_floor_busy(const HaloPanoramaBudget *b, float busy_seconds) {
    if (!(b->pass_ema > 0.f) || !(b->ppf_ema > 0.f)) return busy_seconds;
    float over = b->ppf_ema - halo_panorama_tier_floor_ppf[HALO_PANORAMA_TIER_MAX];
    float floor_busy = over > 0.f ? busy_seconds - b->pass_ema * over : busy_seconds;
    return floor_busy > 0.f ? floor_busy : 0.f;
}

/* The frame period just got shorter (the pacer stepped to a faster cadence):
 * go straight to the lightest tier predicted to fit under the new target,
 * with the extras at their floor, rather than a tier per dwell while every
 * frame misses. Only ever heavier; relief brings it back as usual. */
static inline void halo_panorama_budget_retarget(HaloPanoramaBudget *b, float target_seconds) {
    if (!(target_seconds > 0.f) || !(b->busy_ema > target_seconds)) return;
    unsigned t = b->tier;
    if (b->pass_ema > 0.f && b->ppf_ema > 0.f)
        while (t < HALO_PANORAMA_TIER_MAX &&
               b->busy_ema - b->pass_ema * (b->ppf_ema - halo_panorama_tier_floor_ppf[t]) > target_seconds) t++;
    else t = HALO_PANORAMA_TIER_MAX;
    b->extra_half = 1;
    if (t != b->tier) { b->tier = t; b->tier_dwell = 0.f; }
    b->tier_pressure = b->tier_relief = 0;
    b->since_adjust = 0.f;
}
#endif
