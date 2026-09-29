/* Measurement arithmetic and atomic publication, with no scheduler changes. */
#include "core_telemetry.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static HaloFrameSample frame(uint64_t now_ms, uint64_t pass_ms, uint64_t idle_ms, uint64_t wait_ms,
                             uint64_t sleep_ms, uint32_t tick, uint32_t last_call, uint64_t cpu_ms) {
    HaloFrameSample s; memset(&s, 0, sizeof s);
    s.now_ns = now_ms * 1000000ull; s.pass_ns = pass_ms * 1000000ull; s.idle_ns = idle_ms * 1000000ull;
    s.wait_ns = wait_ms * 1000000ull; s.sleep_ns = sleep_ms * 1000000ull;
    s.ticks_valid = 1; s.tick_globals = 0x700000; s.tick_counter = tick; s.tick_last_call = last_call;
    s.counts_valid = 1; s.cpu_ns = cpu_ms * 1000000ull;
    return s;
}

static void split_and_buckets(void) {
    static HaloFrameSplit split;
    HaloFrameSample a = frame(1000, 0, 0, 0, 0, 500, 1, 0);
    halo_frame_split_add(&split, &a);
    assert(split.totals.presents == 1 && split.totals.frames == 0);
    /* 75 ms: 30 in passes, 5 idle (2 asleep), 4 waiting, so 36 elsewhere;
     * two ticks. The thread ran 60 ms, so it was off the core 15 ms, of which
     * 2 asleep and 4 in a wait: 9 ms unaccounted. */
    HaloFrameSample b = frame(1075, 30, 5, 4, 2, 502, 2, 60);
    halo_frame_split_add(&split, &b);
    HaloFrameSplitTotals t; halo_frame_split_read(&split, &t);
    assert(t.frames == 1 && t.wall_ns == 75000000ull && t.pass_ns == 30000000ull && t.idle_ns == 5000000ull);
    assert(t.wait_ns == 4000000ull && t.sleep_ns == 2000000ull && t.other_ns == 36000000ull && t.outside_pass_ns == 45000000ull);
    assert(t.cpu_ns == 60000000ull && t.off_core_ns == 15000000ull && t.unaccounted_ns == 9000000ull);
    assert(t.ticks == 2 && t.tick_frames[2] == 1 && t.tick_other_ns[2] == 36000000ull && t.max_ticks == 2);
    assert(t.tick_mismatch_frames == 0 && t.tick_resyncs == 0);
    assert(t.counts_frames == 1 && t.counts_available);
    /* Six ticks in one frame land in the 4+ bucket, and the driver having
     * reported only 3 for its last call means two driver calls ran between
     * these Presents. */
    HaloFrameSample c = frame(1175, 60, 5, 4, 2, 508, 3, 160);
    halo_frame_split_add(&split, &c);
    halo_frame_split_read(&split, &t);
    assert(t.frames == 2 && t.ticks == 8 && t.tick_frames[4] == 1 && t.max_ticks == 6);
    assert(t.tick_other_ns[4] == 70000000ull && t.tick_mismatch_frames == 1);
    /* A frame with passes and idle past its period (a boundary effect) has
     * nothing left for the rest, not a wrapped remainder. */
    HaloFrameSample d = frame(1185, 70, 10, 4, 2, 508, 0, 170);
    halo_frame_split_add(&split, &d);
    halo_frame_split_read(&split, &t);
    assert(t.frames == 3 && t.tick_frames[0] == 1 && t.tick_other_ns[0] == 0 && t.other_ns == 106000000ull);
}

static void resyncs_and_stalls(void) {
    static HaloFrameSplit split;
    HaloFrameSample a = frame(0, 0, 0, 0, 0, 9000, 1, 0);
    halo_frame_split_add(&split, &a);
    /* A new map restarts the counter: its interval is unknown, never guessed. */
    HaloFrameSample b = frame(40, 10, 0, 0, 0, 3, 3, 30);
    halo_frame_split_add(&split, &b);
    HaloFrameSplitTotals t; halo_frame_split_read(&split, &t);
    assert(t.tick_resyncs == 1 && t.ticks == 0 && t.tick_unknown_frames == 1 && t.tick_frames[3] == 0 && t.tick_mismatch_frames == 0);
    /* A checkpoint revert can jump far forward too. */
    HaloFrameSample c = frame(80, 20, 0, 0, 0, 5000, 1, 60);
    halo_frame_split_add(&split, &c);
    halo_frame_split_read(&split, &t);
    assert(t.tick_resyncs == 2 && t.ticks == 0 && t.tick_unknown_frames == 2 && t.tick_frames[1] == 0);
    /* A five-second load is counted apart, ticks and all, and leaves the
     * split and its buckets alone. */
    HaloFrameSample d = frame(5080, 25, 0, 0, 0, 5030, 30, 4000);
    halo_frame_split_add(&split, &d);
    halo_frame_split_read(&split, &t);
    assert(t.stall_frames == 1 && t.stall_ns == 5000000000ull && t.stall_ticks == 30);
    assert(t.stall_outside_pass_ns == 4995000000ull && t.stall_tick_unknown_frames == 0);
    assert(t.frames == 2 && t.wall_ns == 80000000ull && t.ticks == 0 && t.cpu_ns == 60000000ull);
    /* No globals: unknown ticks, not a measured zero-tick frame. */
    HaloFrameSample e = frame(5110, 30, 0, 0, 0, 0, 0, 4020); e.ticks_valid = 0;
    halo_frame_split_add(&split, &e);
    halo_frame_split_read(&split, &t);
    assert(t.frames == 3 && t.ticks == 0 && t.tick_unknown_frames == 3 && t.tick_frames[0] == 0 && t.tick_mismatch_frames == 0);
    /* Without thread counters, the CPU split is left alone. */
    HaloFrameSample f = frame(5140, 35, 0, 0, 0, 0, 0, 0); f.ticks_valid = 0; f.counts_valid = 0;
    halo_frame_split_add(&split, &f);
    halo_frame_split_read(&split, &t);
    assert(t.frames == 4 && t.cpu_ns == 80000000ull && t.counts_available == 0);
}

static void game_time_from_guest_memory(void) {
    uint8_t *flat = calloc(1, 0x800000); assert(flat);
    HaloFrameSample s; memset(&s, 0, sizeof s);
    halo_game_time_read(flat, &s);
    assert(!s.ticks_valid);
    uint32_t globals = 0x700000, tick = 123456; uint16_t last_call = 2;
    memcpy(flat + HALO_GAME_TIME_GLOBALS, &globals, 4);
    memcpy(flat + globals + 0x0C, &tick, 4); memcpy(flat + globals + 0x10, &last_call, 2);
    halo_game_time_read(flat, &s);
    assert(s.ticks_valid && s.tick_counter == 123456 && s.tick_last_call == 2);
    halo_game_time_read(NULL, &s);
    assert(!s.ticks_valid);
    globals = 0x8000; memcpy(flat + HALO_GAME_TIME_GLOBALS, &globals, 4);   /* inside the null guard */
    halo_game_time_read(flat, &s);
    assert(!s.ticks_valid);
    globals = UINT32_MAX; memcpy(flat + HALO_GAME_TIME_GLOBALS, &globals, 4);
    halo_game_time_read(flat, &s);
    assert(!s.ticks_valid); /* reject wraparound at the end of guest memory */
    free(flat);
}

/* Unknown and reset counters never masquerade as zero-tick or zero-CPU
 * samples; valid intervals resume only after a fresh baseline. */
static void counter_availability(void) {
    HaloFrameSplit split = {0};
    HaloFrameSample a = frame(0, 0, 0, 0, 0, 20, 2, 0);
    halo_frame_split_add(&split, &a);
    HaloFrameSample b = frame(20, 5, 0, 0, 0, 20, 2, 10);
    halo_frame_split_add(&split, &b);
    HaloFrameSplitTotals t;
    assert(halo_frame_split_read(&split, &t));
    /* The paused driver's stale last-call value does not invent ticks. */
    assert(t.ticks == 0 && t.tick_frames[0] == 1 && t.tick_mismatch_frames == 1);
    HaloFrameSample c = frame(40, 10, 0, 0, 0, 21, 1, 20);
    c.tick_globals += 0x100; c.counts_valid = 0;
    halo_frame_split_add(&split, &c);
    assert(halo_frame_split_read(&split, &t));
    assert(t.tick_unknown_frames == 1 && t.tick_resyncs == 1 && t.ticks == 0);
    assert(t.counts_frames == 1 && t.counts_available == 0 && t.cpu_ns == 10000000ull);
    HaloFrameSample d = frame(60, 15, 0, 0, 0, 22, 1, 30);
    d.tick_globals = c.tick_globals;
    halo_frame_split_add(&split, &d);
    assert(halo_frame_split_read(&split, &t));
    assert(t.tick_frames[1] == 1 && t.ticks == 1);
    assert(t.counts_frames == 1 && t.counts_available == 1); /* gap cannot be bridged */
    HaloFrameSample e = frame(80, 20, 0, 0, 0, 23, 1, 5);
    e.tick_globals = c.tick_globals;
    halo_frame_split_add(&split, &e);
    assert(halo_frame_split_read(&split, &t));
    assert(t.counts_frames == 1); /* a reset also starts a new baseline */
    HaloFrameSample f = frame(100, 25, 0, 0, 0, 24, 1, 15);
    f.tick_globals = c.tick_globals;
    halo_frame_split_add(&split, &f);
    assert(halo_frame_split_read(&split, &t));
    assert(t.counts_frames == 2 && t.cpu_ns == 20000000ull);
    f.now_ns += 2000000000ull; f.tick_counter = 0;
    halo_frame_split_add(&split, &f);
    assert(halo_frame_split_read(&split, &t));
    assert(t.stall_tick_unknown_frames == 1 && t.stall_ticks == 0);
    assert(t.stall_outside_pass_ns == 2000000000ull);
}

static void unavailable_snapshot_preserves_output(void) {
    HaloFrameSplit split = {0};
    HaloFrameSplitTotals t, before;
    memset(&t, 0xA5, sizeof t); before = t;
    atomic_store(&split.sequence, 1); /* writer preempted mid-publication */
    assert(!halo_frame_split_read(&split, &t));
    assert(!memcmp(&before, &t, sizeof t));
    atomic_store(&split.sequence, 0);
    assert(halo_frame_split_read(&split, &t));
    assert(t.frames == 0 && t.presents == 0);
}

/* One writer adding frames whose parts always sum to the period, one reader
 * checking every snapshot it takes sums too. */
static HaloFrameSplit shared;
static _Atomic int writer_done;
static void *writer(void *unused) {
    (void)unused;
    for (uint64_t i = 0; i < 400000; i++) {
        HaloFrameSample s; memset(&s, 0, sizeof s);
        s.now_ns = i * 1000; s.pass_ns = i * 400; s.idle_ns = i * 100; s.wait_ns = i * 50;
        s.ticks_valid = 1; s.tick_counter = (uint32_t)(i * 2); s.tick_last_call = 2;
        halo_frame_split_add(&shared, &s);
        for (volatile int pause = 0; pause < 32; pause++) {}
    }
    atomic_store(&writer_done, 1);
    return NULL;
}
static void reader_sees_whole_updates(void) {
    pthread_t thread; assert(!pthread_create(&thread, NULL, writer, NULL));
    unsigned checked = 0, attempts = 0;
    while (!atomic_load(&writer_done)) {
        HaloFrameSplitTotals t; attempts++;
        if (!halo_frame_split_read(&shared, &t)) continue;
        assert(t.wall_ns == t.pass_ns + t.idle_ns + t.wait_ns + t.other_ns);
        assert(t.ticks == 2 * t.frames && t.tick_frames[2] == t.frames && t.presents == t.frames + (t.presents ? 1 : 0));
        checked++;
    }
    pthread_join(thread, NULL);
    assert(checked > 0);
    HaloFrameSplitTotals t; int whole = halo_frame_split_read(&shared, &t);
    assert(whole);
    assert(t.frames == 399999 && t.wall_ns == 399999000ull && t.other_ns == 399999000ull * 45 / 100);
    printf("  %u of %u snapshots whole under a concurrent writer, every one consistent\n", checked, attempts);
}

int main(void) {
    split_and_buckets();
    resyncs_and_stalls();
    game_time_from_guest_memory();
    counter_availability();
    unavailable_snapshot_preserves_output();
    reader_sees_whole_updates();
    puts("PASS: frame split, measured ticks and resyncs, stalls apart, whole-update reads, CPU coverage, unknown ticks excluded");
    return 0;
}
