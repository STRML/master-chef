#ifndef HALO_CORE_TELEMETRY_H
#define HALO_CORE_TELEMETRY_H
/* Measurement-only arithmetic for engine-thread Present-to-Present samples.
 * Guest state is read on the engine thread. Reports read an atomically
 * published snapshot; neither reader nor writer changes engine behavior. */
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* Frames that ran 0, 1, 2, 3 and 4 or more game ticks. */
enum { HALO_TICK_BUCKETS = 5 };
/* Verified against the generated Build75 sub_00470BF0.c: the pointer at
 * 006F1D6C names game_time_globals. After each returned call to 0045B780 at
 * 00470C54, 00470C68/00470C6E increments the dword at +0x0C. 00470C7A
 * stores the driver's last-call tick count (a word) at +0x10, but the
 * inactive-game branch zeros it at 00470CC6. It can still be stale if the
 * driver is not called between Presents, and only counts the last driver
 * call if several occur. It is a comparison, never a delta replacement.
 * Reading this counter introduces no intercepted address or guest write. */
#define HALO_GAME_TIME_GLOBALS 0x006F1D6Cu
#define HALO_GAME_TIME_TICK 0x0Cu
#define HALO_GAME_TIME_LAST_CALL 0x10u
/* Conservative discontinuity threshold. A reset/restore can change the
 * counter independently of ticks; detected changes are unknown intervals.
 * A small forward restore within the threshold cannot be detected by two
 * counter reads alone. This is a game-time delta, not a hook call counter. */
#define HALO_TICK_JUMP 120u
/* Separate long stalls from the steady-frame means; preserve their wall
 * duration and measured tick count in the stall totals. */
#define HALO_FRAME_STALL_NS 1000000000ull

/* One Present's view of the engine thread. Every field is a running total
 * except the tick counters, which are Halo's own. */
typedef struct {
    uint64_t now_ns;         /* CLOCK_UPTIME_RAW */
    uint64_t pass_ns;        /* inside bearing passes (d3d9_render.inc) */
    uint64_t idle_ns;        /* the frame limiter's idle: Sleep(n) and the Sleep(0) spin */
    uint64_t wait_ns, waits; /* elapsed non-polling wait APIs and critical-section contention */
    uint64_t sleep_ns;       /* idle in blocking sleep/cache-wait calls, including overshoot */
    int ticks_valid;         /* game_time_globals exists */
    uint32_t tick_counter, tick_last_call, tick_globals;
    int counts_valid;        /* THREAD_EXTENDED_INFO answered */
    uint64_t cpu_ns;         /* cumulative engine-thread user + system ns */
} HaloFrameSample;

typedef struct {
    /* Measured Present-to-Present periods. outside_pass_ns is wall minus
     * bearing-pass wall time, clamped per frame. idle/wait may overlap pass
     * time; other_ns is only the clamped residual estimate after subtracting
     * those counters, not an exclusive partition or game-tick CPU time. */
    uint64_t frames, wall_ns, pass_ns, idle_ns, wait_ns, sleep_ns, other_ns, waits;
    uint64_t outside_pass_ns;
    /* The engine thread's CPU time over the same frames, the time it was
     * off a core, and the residual after timed blocking sleep/cache waits:
     * waiting for a core or blocked somewhere unmeasured. Process-wide
     * runnable time cannot assign that cause to this thread or interval. */
    uint64_t cpu_ns, off_core_ns, unaccounted_ns, counts_frames;
    /* Game-time counter deltas and residual estimates by tick bucket.
     * Unknown intervals are excluded from both ticks and tick buckets. */
    uint64_t ticks, tick_frames[HALO_TICK_BUCKETS], tick_other_ns[HALO_TICK_BUCKETS];
    /* Counter discontinuities; unknown normal-frame intervals; and valid
     * deltas differing from the driver's possibly stale last-call count. */
    uint64_t tick_resyncs, tick_mismatch_frames, tick_unknown_frames;
    uint32_t max_ticks, counts_available;
    uint64_t stall_frames, stall_ns, stall_ticks, stall_outside_pass_ns, stall_tick_unknown_frames;
    uint64_t presents;
} HaloFrameSplitTotals;

enum { HALO_FRAME_TOTAL_WORDS = (sizeof(HaloFrameSplitTotals) + sizeof(uint64_t) - 1) / sizeof(uint64_t) };
/* totals, previous and have_previous belong to the single engine writer.
 * Readers access only atomic words. A sequence counter around non-atomic
 * memcpy would still be a C data race even when a changed sequence retries. */
typedef struct {
    _Atomic uint64_t sequence;
    _Atomic uint64_t published[HALO_FRAME_TOTAL_WORDS];
    HaloFrameSplitTotals totals;
    HaloFrameSample previous;
    int have_previous;
} HaloFrameSplit;

static inline uint64_t halo_counter_delta(uint64_t now, uint64_t before) { return now > before ? now - before : 0; }

static inline void halo_game_time_read(const uint8_t *flat, HaloFrameSample *sample) {
    sample->ticks_valid = 0; sample->tick_counter = sample->tick_last_call = sample->tick_globals = 0;
    if (!flat) return;
    uint32_t globals; memcpy(&globals, flat + HALO_GAME_TIME_GLOBALS, 4);
    /* Zero until the game allocates them. The first 64 KiB of guest space is
     * the host's null guard (host.c) and faults on any read. */
    if (globals < 0x10000u || globals > 0xFFFFFF00u) return;
    uint16_t last_call;
    memcpy(&sample->tick_counter, flat + globals + HALO_GAME_TIME_TICK, 4);
    memcpy(&last_call, flat + globals + HALO_GAME_TIME_LAST_CALL, 2);
    sample->tick_last_call = last_call;
    sample->tick_globals = globals;
    sample->ticks_valid = 1;
}

/* Returns a continuous game-time delta. resync is set for every unknown
 * interval, including unavailable samples. Never guess using last_call. */
static inline uint32_t halo_frame_ticks(const HaloFrameSample *before, const HaloFrameSample *now, int *resync) {
    *resync = !before->ticks_valid || !now->ticks_valid ||
        now->tick_globals != before->tick_globals || now->tick_counter < before->tick_counter ||
        now->tick_counter - before->tick_counter > HALO_TICK_JUMP;
    return *resync ? 0 : now->tick_counter - before->tick_counter;
}

static inline void halo_frame_split_add(HaloFrameSplit *split, const HaloFrameSample *now) {
    HaloFrameSplitTotals *t = &split->totals;
    t->presents++;
    t->counts_available = (uint32_t)(now->counts_valid != 0);
    if (split->have_previous) {
        const HaloFrameSample *before = &split->previous;
        uint64_t period = halo_counter_delta(now->now_ns, before->now_ns);
        int resync = 0;
        uint32_t ticks = halo_frame_ticks(before, now, &resync);
        if (resync && now->ticks_valid) t->tick_resyncs++;
        if (period > HALO_FRAME_STALL_NS) {
            uint64_t pass = halo_counter_delta(now->pass_ns, before->pass_ns);
            t->stall_frames++; t->stall_ns += period; t->stall_ticks += ticks;
            t->stall_outside_pass_ns += halo_counter_delta(period, pass);
            t->stall_tick_unknown_frames += (uint64_t)resync;
        } else {
            uint64_t pass = halo_counter_delta(now->pass_ns, before->pass_ns);
            uint64_t idle = halo_counter_delta(now->idle_ns, before->idle_ns);
            uint64_t wait = halo_counter_delta(now->wait_ns, before->wait_ns);
            uint64_t sleep = halo_counter_delta(now->sleep_ns, before->sleep_ns);
            uint64_t outside = halo_counter_delta(period, pass);
            uint64_t other = halo_counter_delta(halo_counter_delta(outside, idle), wait);
            t->frames++; t->wall_ns += period; t->pass_ns += pass; t->idle_ns += idle;
            t->wait_ns += wait; t->sleep_ns += sleep; t->other_ns += other; t->outside_pass_ns += outside;
            t->waits += halo_counter_delta(now->waits, before->waits);
            if (now->counts_valid && before->counts_valid && now->cpu_ns >= before->cpu_ns) {
                uint64_t cpu = now->cpu_ns - before->cpu_ns;
                t->counts_frames++;
                uint64_t off_core = period > cpu ? period - cpu : 0;
                t->cpu_ns += cpu; t->off_core_ns += off_core;
                t->unaccounted_ns += halo_counter_delta(halo_counter_delta(off_core, sleep), wait);
            }
            if (resync) {
                t->tick_unknown_frames++;
            } else {
                unsigned bucket = ticks < HALO_TICK_BUCKETS - 1 ? ticks : HALO_TICK_BUCKETS - 1;
                t->ticks += ticks; t->tick_frames[bucket]++; t->tick_other_ns[bucket] += other;
                if (ticks > t->max_ticks) t->max_ticks = ticks;
                if (ticks != now->tick_last_call) t->tick_mismatch_frames++;
            }
        }
    }
    split->previous = *now; split->have_previous = 1;
    uint64_t words[HALO_FRAME_TOTAL_WORDS] = {0};
    memcpy(words, &split->totals, sizeof split->totals);
    uint64_t sequence = atomic_load_explicit(&split->sequence, memory_order_relaxed);
    atomic_store_explicit(&split->sequence, sequence + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (unsigned i = 0; i < HALO_FRAME_TOTAL_WORDS; i++)
        atomic_store_explicit(&split->published[i], words[i], memory_order_relaxed);
    atomic_store_explicit(&split->sequence, sequence + 2u, memory_order_release);
}

/* Returns 1 for a coherent snapshot. A preempted writer can leave its
 * publication in progress; bounded retries return 0 and leave out unchanged.
 * The caller must report availability or retain its previous good sample. */
static inline int halo_frame_split_read(HaloFrameSplit *split, HaloFrameSplitTotals *out) {
    for (int attempt = 0; attempt < 1000; attempt++) {
        uint64_t before = atomic_load_explicit(&split->sequence, memory_order_acquire);
        if (before & 1u) continue;
        uint64_t words[HALO_FRAME_TOTAL_WORDS];
        for (unsigned i = 0; i < HALO_FRAME_TOTAL_WORDS; i++)
            words[i] = atomic_load_explicit(&split->published[i], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&split->sequence, memory_order_relaxed) == before) {
            memcpy(out, words, sizeof *out);
            return 1;
        }
    }
    return 0;
}
#endif
