/* hsc_trace.inc -- read-only, default-off HSC (Halo script) thread tracing.
 *
 * Hooked at the ORIGINAL scheduler boundary: entry of FUN_0048a1a0 (the per-tick
 * script scheduler, called from the main loop, never from inside Present).
 * The hook is reached through engine_dispatch_override(cpu, address), the same
 * call-target boundary a10_control.inc already uses.
 *
 * Guarantees:
 *   - never writes guest memory (only G8/G16/G32 reads through the flat map)
 *   - every guest pointer is bounds-checked with hsc_ok() before dereference
 *   - bounded output: at most HSC_TRACE_MAX_EVENTS thread events per snapshot
 *   - default off; enabled only when HALO_HSC_TRACE is set
 *
 * Environment:
 *   HALO_HSC_TRACE=<interval>   positive tick interval between snapshots (default 120;
 *                               "0", "" or non-numeric -> default; "1" samples each tick). Set to a
 *                               number to enable; unset -> completely inert.
 *   HALO_HSC_TRACE_MAX=<n>      max thread events per snapshot (default 128, cap 512)
 *   HALO_HSC_TRACE_ALL=1        also emit wake -1 threads (default: skipped,
 *                               but still counted in "total")
 *
 * Log contract (schema 1). Each line: "[hsc-trace] " + exactly one JSON object.
 *   {"schema":1,"event":"scheduler","tick":T,"enabled":0|1,"total":N,"emitted":M,"truncated":0|1}
 *   {"schema":1,"event":"thread","tick":T,"thread":i,"script":s,"kind":k,"wake":w,
 *    "state":"runnable|sleeping|dormant|indefinite","node":n|null,"opcode":o|null,"name":"..."|null}
 * state: runnable  (wake>=0 && wake<=tick), sleeping (wake>tick), dormant (wake==-2),
 *        indefinite (all other negative wakes, including -1).
 *   Negative wake values alone do not prove completion or a deadlock.
 *
 * Layouts (validated against decompilation/c9acf0c46954/functions/):
 *   DAT_0087a470  script-thread datum array: +0x2e last index (s16), +0x34 element base
 *                 (0048a1a0.c:12-18, 0048a370.c:15-16). Element stride 0x218.
 *   thread elem   +0 salt (u16, 0 = free), +2 thread type (0048a2f0.c:17), +3 flags,
 *                 +4 script index (0048a2f0.c:18), +8 wake tick (0048a2f0.c:22,25),
 *                 +0x10 current frame ptr, +0x18 stack start (0048a2f0.c:13-14).
 *   frame         +4 node datum (0048a2f0.c:16, 0048a370.c:33), +0xc stack offset (s16).
 *   DAT_0087a474  node datum array, stride 0x14; node +2 = function index used as
 *                 (&PTR_DAT_00688b58)[sVar2] in 0048a370.c:34-40 ("opcode").
 *   DAT_006f1d6c  game time: +0xc current tick (0048a1a0.c:13).
 *   DAT_006b15e8  scheduler enabled byte (0048a1a0.c:12).
 *   DAT_00746f8c  scenario tag: +0x49c script count, +0x4a0 script table,
 *                 entry stride 0x5c, +0x20 script kind (00489ef0.c:79-83, 0048a2f0.c:20).
 *                 Name at +0 (32 bytes) matches tools/export_campaign_scripts.py;
 *                 it is emitted only if the 32 bytes
 *                 are printable ASCII and NUL-terminated, otherwise null.
 *
 * Old scanner (d3d9.c HALO_SCAN_THREADS) labelled wake==-2 as "runnable" and also read
 * "type" from thread+2 (thread type, always 0 for script threads) while calling it
 * the script kind. This trace fixes both.
 */
#ifndef HSC_TRACE_MAX_EVENTS_CAP
#define HSC_TRACE_MAX_EVENTS_CAP 512
#endif
#define HSC_SCHEDULER_ENTRY   0x0048a1a0u
#define HSC_THREAD_ARRAY_PTR  0x0087a470u
#define HSC_NODE_ARRAY_PTR    0x0087a474u
#define HSC_GAME_TIME_PTR     0x006f1d6cu
#define HSC_SCHED_ENABLED     0x006b15e8u
#define HSC_SCENARIO_PTR      0x00746f8cu
#define HSC_THREAD_STRIDE     0x218u
#define HSC_NODE_STRIDE       0x14u
#define HSC_SCRIPT_STRIDE     0x5cu

/* Host memory validation: the host maps one flat 4 GiB guest space (host.c: mmap GUEST_SIZE)
 * and makes [0,0x10000) PROT_NONE. There is no finer-grained validity API in host.h, so a
 * guest range is dereferenceable iff it is above the guard page and does not wrap. Callers
 * require metadata and per-thread stack bounds in addition to this range check. */
static int hsc_ok(uint32_t addr, uint32_t len) {
    if (len == 0 || len > 0x01000000u) return 0;
    if (addr < 0x10000u) return 0;
    if (!engine_flat_base) return 0;
    if ((uint64_t)addr + len > 0x100000000ull) return 0;
    return 1;
}

static const char *hsc_state(int32_t wake, int32_t tick) {
    if (wake == -2) return "dormant";
    if (wake < 0)   return "indefinite";
    if (wake <= tick) return "runnable";
    return "sleeping";
}

/* Copy a 32-byte script name into out (>= 33 bytes). Returns 0 if not printable/terminated. */
static int hsc_script_name(uint32_t entry, char *out) {
    if (!hsc_ok(entry, 32)) return 0;
    for (int i = 0; i < 32; i++) {
        uint8_t c = G8(entry + (uint32_t)i);
        if (c == 0) { out[i] = 0; return i > 0; }
        if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return 0;
        out[i] = (char)c;
    }
    return 0; /* no NUL within 32 bytes */
}

static void host_hsc_trace_boundary(EngineCPU *cpu, uint32_t address) {
    static int interval = -1;        /* -1 = not yet read, 0 = disabled */
    static int max_events = 128, emit_all = 0;
    static int32_t last_tick;
    static int sampled;
    (void)cpu;
    if (address != HSC_SCHEDULER_ENTRY) return;
    if (interval < 0) {
        const char *e = getenv("HALO_HSC_TRACE");
        if (!e) { interval = 0; return; }
        long v = strtol(e, NULL, 10); interval = v > 0 && v <= 2147483647L ? (int)v : 120;
        const char *m = getenv("HALO_HSC_TRACE_MAX");
        if (m) { long mv = strtol(m, NULL, 10); if (mv > 0) max_events = mv < HSC_TRACE_MAX_EVENTS_CAP ? mv : HSC_TRACE_MAX_EVENTS_CAP; }
        emit_all = getenv("HALO_HSC_TRACE_ALL") != NULL;
        host_log("[hsc-trace] {\"schema\":1,\"event\":\"config\",\"interval\":%d,\"max\":%d,\"all\":%d}", interval, max_events, emit_all);
    }
    if (!interval) return;

    uint32_t tm = G32(HSC_GAME_TIME_PTR);
    int32_t tick = hsc_ok(tm, 0x10) ? (int32_t)G32(tm + 0xcu) : -1;
    /* Sample every `interval` ticks; tick can reset on map reload, so sample every rollback immediately, with 64-bit delta arithmetic. */
    int64_t delta = (int64_t)tick - (int64_t)last_tick;
    if (sampled && delta >= 0 && delta < interval) return;
    sampled = 1;
    last_tick = tick;

    int enabled = G8(HSC_SCHED_ENABLED) != 0;
    uint32_t ta = G32(HSC_THREAD_ARRAY_PTR);
    uint32_t tbase = 0; int last = 0;
    if (hsc_ok(ta, 0x38)) { tbase = G32(ta + 0x34u); last = (int)(int16_t)G16(ta + 0x2eu); }
    if (!tbase || last <= 0 || last > 2048 || !hsc_ok(tbase, (uint32_t)last * HSC_THREAD_STRIDE)) {
        host_log("[hsc-trace] {\"schema\":1,\"event\":\"scheduler\",\"tick\":%d,\"enabled\":%d,\"total\":0,\"emitted\":0,\"truncated\":0}", tick, enabled);
        return;
    }
    uint32_t na = G32(HSC_NODE_ARRAY_PTR), nbase = 0; int nlast = 0;
    if (hsc_ok(na, 0x38)) { nbase = G32(na + 0x34u); nlast = (int)G16(na + 0x2eu); }
    if (!hsc_ok(nbase, (uint32_t)(nlast > 0 ? nlast : 1) * HSC_NODE_STRIDE)) { nbase = 0; nlast = 0; }
    uint32_t sc = G32(HSC_SCENARIO_PTR), stab = 0; int scount = 0;
    if (hsc_ok(sc, 0x4a4)) { scount = (int)G32(sc + 0x49cu); stab = G32(sc + 0x4a0u); }
    if (scount <= 0 || scount > 4096 || !hsc_ok(stab, (uint32_t)scount * HSC_SCRIPT_STRIDE)) { stab = 0; scount = 0; }

    int total = 0, emitted = 0, truncated = 0;
    for (int t = 0; t < last; t++) {
        uint32_t e = tbase + (uint32_t)t * HSC_THREAD_STRIDE;
        if (G16(e) == 0) continue;               /* free slot */
        total++;
        int32_t script = (int32_t)G32(e + 4u);
        int32_t wake   = (int32_t)G32(e + 8u);
        if (wake == -1 && !emit_all) continue; /* completion or indefinite sleep */
        if (emitted >= max_events) { truncated = 1; continue; }

        int kind = -1; char name[33]; int have_name = 0;
        if (stab && script >= 0 && script < scount) {
            uint32_t se = stab + (uint32_t)script * HSC_SCRIPT_STRIDE;
            kind = (int)(int16_t)G16(se + 0x20u);
            have_name = hsc_script_name(se, name);
        }
        /* Current frame -> node datum -> opcode. Frame must lie inside this thread's own stack. */
        long node = -1, opcode = -1;
        uint32_t frame = G32(e + 0x10u);
        if (frame >= e + 0x18u && (uint64_t)frame + 0x10u <= (uint64_t)e + HSC_THREAD_STRIDE) {
            uint32_t nd = G32(frame + 4u);
            if (nd != 0xffffffffu && nbase) {
                uint32_t ni = nd & 0xffffu;
                if ((int)ni < nlast) { node = (long)nd; opcode = (long)(int16_t)G16(nbase + ni * HSC_NODE_STRIDE + 2u); }
            }
        }
        char nodebuf[16], opbuf[16], namebuf[40];
        if (node >= 0) snprintf(nodebuf, sizeof nodebuf, "%ld", node); else snprintf(nodebuf, sizeof nodebuf, "null");
        if (node >= 0) snprintf(opbuf, sizeof opbuf, "%ld", opcode); else snprintf(opbuf, sizeof opbuf, "null");
        if (have_name) snprintf(namebuf, sizeof namebuf, "\"%s\"", name); else snprintf(namebuf, sizeof namebuf, "null");
        host_log("[hsc-trace] {\"schema\":1,\"event\":\"thread\",\"tick\":%d,\"thread\":%d,\"script\":%d,\"kind\":%d,\"wake\":%d,\"state\":\"%s\",\"node\":%s,\"opcode\":%s,\"name\":%s}",
                 tick, t, script, kind, wake, hsc_state(wake, tick), nodebuf, opbuf, namebuf);
        emitted++;
    }
    host_log("[hsc-trace] {\"schema\":1,\"event\":\"scheduler\",\"tick\":%d,\"enabled\":%d,\"total\":%d,\"emitted\":%d,\"truncated\":%d}",
             tick, enabled, total, emitted, truncated);
}
