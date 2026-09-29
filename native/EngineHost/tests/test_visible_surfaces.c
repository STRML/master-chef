/* The native 00553920 (visible_surfaces.h) against the translated routine.
 *
 * Compiles the translated sub_00553920.c and sub_0050D5B0.c from the
 * generated engine next to the native version and runs both from the same
 * starting state: randomized visible-cluster records, frustums, subcluster
 * boxes and triangle lists, plus the awkward cases (signed zeros, NaNs,
 * infinities, subnormals, integer boxes touching the planes and the bounds
 * exactly, a limit other than zero, the 0x4000 cap reached at each of its
 * three checks and already passed, int16 counts at their limits, negative
 * subcluster and triangle counts, clusters listed twice, and triangle ids
 * whose bitset writes land in the frustums and counts the routine reads
 * next or in its own stack frame and 0050D5B0's below it). It compares
 * every byte of the guest memory either could reach,
 * all eight registers, the flags, the return, and the x87 registers, stack
 * and status word. Guest memory outside those windows is not mapped, so a
 * stray access by either stops the test. It also checks the native routine
 * declines, touching nothing, in the cases it leaves to the translated one,
 * then times both on a b30-sized view.
 *
 * Needs the generated engine on the include path (not part of the repository) and prints
 * SKIP without it. Built as the headset builds the engine:
 * clang -O2 -DENGINE_FLAT_MEMORY=1 -DHALO_ARM64_FENV_FAST=1 -frounding-math -ffp-contract=off \
 *   -I native/EngineHost -I native/EngineReuse native/EngineHost/tests/test_visible_surfaces.c -lm
 */
#include "visible_surfaces.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#if __has_include("engine_functions.h") && \
    __has_include("sub_00553920.c") && \
    __has_include("sub_0050D5B0.c")
#define HAVE_GENERATED 1
#include "engine_functions.h"
#else
#define HAVE_GENERATED 0
#endif

uint8_t *engine_flat_base;
uint32_t engine_trace_lo, engine_trace_hi;
void engine_pc_trace(EngineCPU *cpu) { (void)cpu; }
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    fprintf(stderr, "FAIL: unexpected dispatch to %08x from %08x\n", address, cpu->pc); exit(1);
}
static void translated_failure(EngineCPU *cpu, const char *reason) {
    fprintf(stderr, "FAIL: translated routine stopped at %08x: %s\n", cpu->pc, reason); exit(1);
}

#if HAVE_GENERATED
/* The only guest memory the routine should reach: the plane limit, the PVS
 * mode byte, the view frustum through the triangle count, and the test's
 * own BSP data and stack, in 16 KiB pages. Everything else is unmapped. */
#define DATA 0x01000000u
#define DATA_SIZE 0x00100000u
#define WIDE 0x02000000u        /* only for the scene with 40,000 subclusters */
#define WIDE_SIZE 0x00400000u
#define RETURN_ADDRESS 0x005538F7u   /* 005537C0, after its call */
typedef struct { uint32_t base, size; } Window;
static const Window windows[] = {
    { 0x00670000u, 0x4000u }, { 0x00724000u, 0x4000u }, { 0x007C0000u, 0x00094000u }, { DATA, DATA_SIZE },
    { WIDE, WIDE_SIZE },
};
enum { ALL_WINDOWS = sizeof windows / sizeof *windows };
static int WINDOWS = ALL_WINDOWS - 1;   /* the wide one joins the snapshots only when in use */
static size_t window_bytes(void) { size_t n = 0; for (int i = 0; i < ALL_WINDOWS; i++) n += windows[i].size; return n; }
static void save(uint8_t *to) { for (int i = 0; i < WINDOWS; i++) { memcpy(to, GPTR(windows[i].base), windows[i].size); to += windows[i].size; } }
static void restore(const uint8_t *from) { for (int i = 0; i < WINDOWS; i++) { memcpy(GPTR(windows[i].base), from, windows[i].size); from += windows[i].size; } }
/* The first guest address whose byte differs from the snapshot, or 0. */
static uint32_t differs(const uint8_t *from) {
    for (int i = 0; i < WINDOWS; i++) {
        const uint8_t *live = GPTR(windows[i].base);
        if (memcmp(live, from, windows[i].size))
            for (uint32_t k = 0; k < windows[i].size; k++) if (live[k] != from[k]) return windows[i].base + k;
        from += windows[i].size;
    }
    return 0;
}

static uint64_t state = 0x243F6A8885A308D3ull;
static uint64_t next(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static uint32_t below(uint32_t n) { return n ? (uint32_t)(next() % n) : 0; }
static double unit(void) { return (double)(next() >> 11) * 0x1p-53; }
static int chance(unsigned percent) { return below(100) < percent; }
static void put_float(uint32_t a, float v) { memcpy(GPTR(a), &v, 4); }
static float float_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static float awkward(void) {
    static const uint32_t bits[] = {
        0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00123u, 0x7F800001u, 0xFFA00000u,
        0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F800000u, 0xBF800000u };
    return float_bits(bits[below(sizeof bits / sizeof *bits)]);
}

/* Frustum styles: 0 a real view frustum, 1 axis planes with small integer
 * coefficients (exact ties), 2 a real one with awkward values mixed in,
 * 3 random bit patterns, 5 coefficients that make a plane's sum cancel, so
 * that adding its three terms in another order changes the sign. */
static double eye[3];
static const float cancelling[] = { 1.f, -1.f, 0.5f, 2.f, 0.f, 0x1p-40f, -0x1p-40f, 3.f };
static const float cancelling_box[] = { 1.f, -1.f, 0x1p-60f, -0x1p-60f, 0x1p-30f, 3.f, -3.f, 0.f, 0x1p60f, -0x1p60f, 0.5f, -2.f };
static int mixed_style(void) { int s = (int)below(5); return s == 4 ? 5 : s; }
static void make_frustum(uint32_t f, int style) {
    if (style == 5) {
        static const float offsets[] = { 0.f, -0.f, 0x1p-40f, -0x1p-40f, 1.f, 0x1p-80f };
        for (uint32_t p = 0; p < 4; p++) {
            for (uint32_t k = 0; k < 3; k++) put_float(f + 0x78u + 16u * p + 4u * k, cancelling[below(sizeof cancelling / sizeof *cancelling)]);
            put_float(f + 0x78u + 16u * p + 12u, offsets[below(sizeof offsets / sizeof *offsets)]);
        }
        for (uint32_t k = 0; k < 3; k++) { put_float(f + 0x128u + 8u * k, -1e30f); put_float(f + 0x12Cu + 8u * k, 1e30f); }
        return;
    }
    if (style == 3) { for (uint32_t k = 0; k < 16; k++) put_float(f + 0x78u + 4u * k, float_bits((uint32_t)next()));
                      for (uint32_t k = 0; k < 6; k++) put_float(f + 0x128u + 4u * k, float_bits((uint32_t)next())); return; }
    if (style == 1) {
        for (uint32_t p = 0; p < 4; p++) {
            int axis = (int)below(3), sign = chance(50) ? 1 : -1;
            for (int k = 0; k < 3; k++) put_float(f + 0x78u + 16u * p + 4u * (uint32_t)k, k == axis ? (float)sign : chance(20) ? (float)((int)below(5) - 2) : 0.f);
            put_float(f + 0x78u + 16u * p + 12u, (float)((int)below(17) - 8));
        }
        for (uint32_t k = 0; k < 3; k++) {
            int lo = (int)below(17) - 8, hi = lo + (int)below(9);
            put_float(f + 0x128u + 8u * k, (float)lo); put_float(f + 0x12Cu + 8u * k, (float)hi);
        }
        return;
    }
    double yaw = unit() * 6.283185307179586, pitch = (unit() - 0.5) * 3.0, roll = (unit() - 0.5) * 0.4;
    double fw[3] = { cos(pitch) * cos(yaw), cos(pitch) * sin(yaw), sin(pitch) };
    double up0[3] = { -sin(pitch) * cos(yaw), -sin(pitch) * sin(yaw), cos(pitch) };
    double rt0[3] = { fw[1] * up0[2] - fw[2] * up0[1], fw[2] * up0[0] - fw[0] * up0[2], fw[0] * up0[1] - fw[1] * up0[0] };
    double rt[3], up[3];
    for (int k = 0; k < 3; k++) { rt[k] = rt0[k] * cos(roll) + up0[k] * sin(roll); up[k] = up0[k] * cos(roll) - rt0[k] * sin(roll); }
    double h = 0.1 + unit() * 1.0, v = 0.1 + unit() * 0.9, far = 20.0 + unit() * 800.0;
    double normals[4][3];
    for (int k = 0; k < 3; k++) {
        normals[0][k] = rt[k] * cos(h) - fw[k] * sin(h);  normals[1][k] = -rt[k] * cos(h) - fw[k] * sin(h);
        normals[2][k] = up[k] * cos(v) - fw[k] * sin(v);  normals[3][k] = -up[k] * cos(v) - fw[k] * sin(v);
    }
    for (uint32_t p = 0; p < 4; p++) {
        float n[3] = { (float)normals[p][0], (float)normals[p][1], (float)normals[p][2] };
        for (uint32_t k = 0; k < 3; k++) put_float(f + 0x78u + 16u * p + 4u * k, n[k]);
        put_float(f + 0x78u + 16u * p + 12u, (float)(n[0] * eye[0] + n[1] * eye[1] + n[2] * eye[2]));
    }
    double lo[3] = { eye[0], eye[1], eye[2] }, hi[3] = { eye[0], eye[1], eye[2] };
    for (int c = 0; c < 4; c++) for (int k = 0; k < 3; k++) {
        double corner = eye[k] + far * (fw[k] + ((c & 1) ? 1 : -1) * tan(h) * rt[k] + ((c & 2) ? 1 : -1) * tan(v) * up[k]);
        if (corner < lo[k]) lo[k] = corner;
        if (corner > hi[k]) hi[k] = corner;
    }
    for (uint32_t k = 0; k < 3; k++) { put_float(f + 0x128u + 8u * k, (float)lo[k]); put_float(f + 0x12Cu + 8u * k, (float)hi[k]); }
    if (style == 2) for (int n = 1 + (int)below(3); n; n--) {
        uint32_t slot = below(22);
        put_float(slot < 16 ? f + 0x78u + 4u * slot : f + 0x128u + 4u * (slot - 16u), awkward());
    }
}
static void make_box(uint32_t b, int style) {
    if (style == 5) { for (uint32_t k = 0; k < 6; k++) put_float(b + 4u * k, cancelling_box[below(sizeof cancelling_box / sizeof *cancelling_box)]); return; }
    if (style == 3) { for (uint32_t k = 0; k < 6; k++) put_float(b + 4u * k, float_bits((uint32_t)next())); return; }
    if (style == 1) {
        for (uint32_t k = 0; k < 3; k++) {
            int lo = (int)below(21) - 10, hi = lo + (int)below(6) - (chance(5) ? 3 : 0);
            put_float(b + 8u * k, (float)lo); put_float(b + 8u * k + 4u, (float)hi);
        }
        return;
    }
    double reach = chance(10) ? 2000.0 : 300.0;
    for (uint32_t k = 0; k < 3; k++) {
        double centre = eye[k] + (unit() * 2 - 1) * reach, half = chance(10) ? 0 : chance(5) ? 5000 * unit() : 12 * unit();
        put_float(b + 8u * k, (float)(centre - half)); put_float(b + 8u * k + 4u, (float)(centre + half));
    }
    if (style == 2) for (int n = 1 + (int)below(2); n; n--) put_float(b + 4u * below(6), awkward());
}

/* Triangle ids. Most fall in the level's range; some are chosen so their
 * bitset word is a float the routine reads later (a record's or the view
 * frustum's planes or bounds), the triangle count, the record-count word
 * (its top half, its sign bit, which ends the record loop early, or, while
 * the count stays within the 128 records laid out, a low bit, which makes
 * the loop visit more records) or a word of the stack below the return
 * address: 00553920's locals and saved registers, the argument and return
 * address it pushes for 0050D5B0, and 0050D5B0's frame. The routine reads
 * the locals back, so their bits are chosen to keep it inside the mapped
 * windows: never the sign of the record or subcluster index, and only low
 * bits of the frustum pointer. */
static uint32_t id_range;
static int record_count_low_bits;
static unsigned stack_percent;
#define STACK_ENTRY (DATA + DATA_SIZE - 0x40u)
static uint32_t stack_word_id(void) {
    const uint32_t offset = 4u + 4u * below(0x94u / 4u);            /* entry-0x04 .. entry-0x94 */
    uint32_t bit = below(32);
    if (offset == 0x0Cu) bit = below(16);                          /* the frustum pointer */
    else if ((offset == 0x04u || offset == 0x08u) && bit == 15u) bit = 14u;   /* the record and subcluster index */
    return (uint32_t)((int32_t)(STACK_ENTRY - offset - VS_BITSET) / 4) * 32u + bit;
}
static uint32_t frustum_word_id(uint32_t frustum) {
    uint32_t slot = below(22);
    uint32_t address = slot < 16 ? frustum + 0x78u + 4u * slot : frustum + 0x128u + 4u * (slot - 16u);
    return (uint32_t)((int32_t)(address - VS_BITSET) / 4) * 32u + below(32);
}
static uint32_t triangle_id(uint32_t records) {
    if (stack_percent && chance(stack_percent)) return stack_word_id();   /* stack_ids scenes: 1-10% more */
    if (chance(98)) return below(id_range);
    switch (below(5)) {
    case 0: return 0x400000u + below(32);                              /* [00850394] and the word after */
    case 1: switch (below(3)) {                                         /* [007D0390] */
            case 0: return (uint32_t)-(int32_t)(1 + below(16));              /* bits 16..31 */
            case 1: return (uint32_t)-17;                                    /* bit 15 */
            default: return record_count_low_bits ? (uint32_t)-(int32_t)(26 + below(7)) : (uint32_t)-17;  /* bits 0..6 */
            }
    case 2: return frustum_word_id(VS_VIEW_FRUSTUM);
    case 3: return frustum_word_id(VS_RECORDS + 0x1A0u * below(records ? records : 1) + 0x14u);
    default: return stack_word_id();
    }
}

/* And in bsp_ids scenes, ids whose bitset word is in the BSP data the
 * routine reads as it goes: a subcluster's bounds (any bit), its triangle
 * count (bits 0-2) or a cluster's subcluster count (bits 0-1). Those
 * scenes lay out eight more triangles past each list and four more
 * subclusters past each cluster's array, so the raised counts stay in them. */
static uint32_t subcluster_at[4096], cluster_at[64];
static unsigned subcluster_n, cluster_n;
static uint32_t bsp_word_id(void) {
    uint32_t address, bit;
    switch (below(3)) {
    case 0: address = subcluster_at[below(subcluster_n)] + 4u * below(6); bit = below(32); break;
    case 1: address = subcluster_at[below(subcluster_n)] + 0x18u; bit = below(3); break;
    default: address = cluster_at[below(cluster_n)] + 0x34u; bit = below(2); break;
    }
    return (uint32_t)((int32_t)(address - VS_BITSET) / 4) * 32u + bit;
}

typedef struct { int frustum_style, box_style, cap_start, deep_x87, rounding, stack_ids, bsp_ids; } Kind;
static uint32_t bump;
static uint32_t take(uint32_t size) { uint32_t at = bump; bump += (size + 15u) & ~15u; assert(bump < DATA + DATA_SIZE - 0x1000u); return at; }

/* Lays out one scene; returns the structure BSP address. */
static uint32_t build_scene(const Kind *kind, unsigned *subclusters_out) {
    for (int i = 0; i < WINDOWS; i++) memset(GPTR(windows[i].base), 0, windows[i].size);
    stack_percent = kind->stack_ids ? 1u + below(10) : 0u;
    for (int k = 0; k < 3; k++) eye[k] = (unit() * 2 - 1) * 1000;
    put_float(VS_PLANE_LIMIT, chance(90) ? 0.f : chance(50) ? awkward() : (float)((unit() - 0.5) * 4));
    S8(VS_PVS_MODE, chance(12) ? (uint8_t)(1 + below(255)) : 0);
    S32(VS_CAMERA_CLUSTER, chance(12) ? 0xFFFFFFFFu : below(64));
    make_frustum(VS_VIEW_FRUSTUM, kind->frustum_style == 4 ? mixed_style() : kind->frustum_style);
    bump = DATA;
    uint32_t bsp = take(0x200);
    uint32_t clusters = 1 + below(48), cluster_array = take(0x68u * clusters);
    S32(bsp + 0x138u, cluster_array);
    uint32_t records = chance(5) ? 0 : chance(5) ? 128 : 1 + below(clusters + 8 < 128 ? clusters + 8 : 128);
    int negative_count = chance(3);
    record_count_low_bits = records < 128 && !negative_count;
    id_range = (uint32_t[]){ 48u, 700u, 20000u, 0x100000u, 0x400000u }[below(5)];
    unsigned subclusters = 0;
    const int32_t spare_subclusters = kind->bsp_ids ? 4 : 0, spare_triangles = kind->bsp_ids ? 8 : 0;
    subcluster_n = cluster_n = 0;
    for (uint32_t c = 0; c < clusters; c++) {
        uint32_t cluster = cluster_array + 0x68u * c;
        int32_t count = chance(8) ? 0 : chance(3) ? -(int32_t)below(3) - 1 : (int32_t)(1 + below(kind->cap_start ? 30 : 16));
        int32_t laid = (count > 0 ? count : 0) + spare_subclusters;
        uint32_t array = take(36u * (laid > 0 ? (uint32_t)laid : 1u));
        S32(cluster + 0x34u, (uint32_t)count); S32(cluster + 0x38u, array);
        cluster_at[cluster_n++] = cluster;
        for (int32_t s = 0; s < laid; s++) {
            uint32_t sub = array + 36u * (uint32_t)s;
            make_box(sub, kind->box_style == 4 ? mixed_style() : kind->box_style);
            int32_t triangles = chance(6) ? 0 : chance(2) ? -1 : (int32_t)(1 + below(kind->cap_start ? 90 : 30));
            int32_t listed = (triangles > 0 ? triangles : 0) + spare_triangles;
            uint32_t list = take(4u * (listed > 0 ? (uint32_t)listed : 1u));
            S32(sub + 0x18u, (uint32_t)triangles); S32(sub + 0x1Cu, list);
            for (int32_t t = 0; t < listed; t++) S32(list + 4u * (uint32_t)t, triangle_id(records > 128 ? 128 : records));
            subcluster_at[subcluster_n++] = sub;
            subclusters++;
        }
    }
    if (kind->bsp_ids && subcluster_n) {
        unsigned percent = 1u + below(5);
        for (unsigned i = 0; i < subcluster_n; i++) {
            int32_t triangles = (int32_t)G32(subcluster_at[i] + 0x18u);
            uint32_t list = G32(subcluster_at[i] + 0x1Cu);
            for (int32_t t = 0; t < (triangles > 0 ? triangles : 0) + spare_triangles; t++)
                if (chance(percent)) S32(list + 4u * (uint32_t)t, bsp_word_id());
        }
    }
    for (uint32_t r = 0; r < 128; r++) {   /* all of them, for counts raised by a bitset write */
        uint32_t record = VS_RECORDS + 0x1A0u * r;
        S16(record, (uint16_t)below(clusters));
        make_frustum(record + 0x14u, kind->frustum_style == 4 ? mixed_style() : kind->frustum_style);
    }
    S16(VS_RECORD_COUNT, (uint16_t)(negative_count ? 0x8000u | below(0x8000) : records));
    /* Bits already set, and a count that may already be near or past the cap. */
    uint32_t words = id_range / 32u + 1u;
    for (unsigned n = below(200); n; n--) { uint32_t w = VS_BITSET + 4u * below(words); S32(w, G32(w) | 1u << below(32)); }
    uint16_t marked = 0;
    switch (kind->cap_start) {
    case 1: marked = (uint16_t)(0x3F00u + below(0x100)); break;
    case 2: marked = (uint16_t)(0x3FF0u + below(0x20)); break;
    case 3: marked = (uint16_t)(chance(50) ? 0x8000u + below(0x8000) : 0xFFF0u + below(0x10)); break;
    default: marked = chance(90) ? 0 : (uint16_t)below(0x4000); break;
    }
    S16(VS_MARKED, marked);
    *subclusters_out = subclusters;
    return bsp;
}

static void random_cpu(EngineCPU *c, uint32_t entry, const Kind *kind) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 8; i++) c->gpr[i] = (uint32_t)next();
    c->gpr[4] = entry;
    c->flags = 0x2u | ((uint32_t)next() & 0x0ED5u);
    c->pc = 0x00553920u;
    c->failure = translated_failure;
    static const uint16_t controls[] = { 0x027F, 0x027F, 0x027F, 0x037F, 0x007F, 0x127F };
    c->fp_control = kind->rounding ? (uint16_t)(0x027F | (1u + below(3)) << 10) : controls[below(6)];
    unsigned depth = kind->deep_x87 ? 7 : below(7);
    c->fp_valid = (uint8_t)((1u << depth) - 1u);
    if (!kind->deep_x87 && chance(20)) c->fp_valid = (uint8_t)(next() & 0x3Fu);   /* with holes */
    c->fp_top = (uint8_t)below(8);
    c->fp_status = (uint16_t)(next() & 0xC7FFu);
    for (int i = 0; i < 8; i++) { uint64_t b = next(); if (chance(20)) b |= UINT64_C(0x7FF0000000000000); memcpy(&c->fp_reg[i], &b, 8); }
}
static int cpu_equal(const EngineCPU *a, const EngineCPU *b, const char **field) {
    if (memcmp(a->gpr, b->gpr, sizeof a->gpr)) { *field = "registers"; return 0; }
    if (a->flags != b->flags) { *field = "flags"; return 0; }
    if (a->pc != b->pc) { *field = "return"; return 0; }
    if (memcmp(a->fp_reg, b->fp_reg, sizeof a->fp_reg)) { *field = "x87 registers"; return 0; }
    if (engine_fp_tag_word((EngineCPU *)a) != engine_fp_tag_word((EngineCPU *)b)) { *field = "x87 tags"; return 0; }
    if (a->fp_valid != b->fp_valid || a->fp_top != b->fp_top) { *field = "x87 stack"; return 0; }
    if (a->fp_control != b->fp_control || a->fp_status != b->fp_status) { *field = "x87 control/status"; return 0; }
    if (a->instruction_count != b->instruction_count) { *field = "instruction count"; return 0; }
    return 1;
}
static void dump_cpu(const char *name, const EngineCPU *c) {
    fprintf(stderr, "  %s: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x flags=%08x pc=%08x fpsw=%04x valid=%02x top=%u\n",
        name, c->gpr[0], c->gpr[1], c->gpr[2], c->gpr[3], c->gpr[4], c->gpr[5], c->gpr[6], c->gpr[7], c->flags, c->pc, c->fp_status, c->fp_valid, c->fp_top);
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }

static uint8_t *initial, *translated;
static unsigned compared, declined, capped, marked_any, corner_frames, saved_hit, bsp_hit;
static unsigned native_top_mask;
static EngineCPU last_translated;
/* Runs the translated routine and then the native one from the same start
 * and compares the results; or, where the native one should decline, checks
 * that it did and changed nothing. Exits on any difference. */
static void check(const char *label, unsigned scene, uint64_t seed, const EngineCPU *start, int run_translated) {
    const uint32_t entry = start->gpr[4];
    int expect_native = engine_fp_nearest(start) && !(start->fp_valid & 0xC0u);
    save(initial);
    uint16_t marked_before = G16(VS_MARKED);
    EngineCPU original = *start;
    if (run_translated) {
        sub_00553920(&original);
        if (original.pc != RETURN_ADDRESS || original.gpr[4] != entry + 4u) {
            fprintf(stderr, "FAIL %s %u: translated routine returned to %08x with esp %08x\n", label, scene, original.pc, original.gpr[4]); exit(1);
        }
        save(translated);
        last_translated = original;
        if ((int16_t)G16(VS_MARKED) >= 0x4000 && (int16_t)marked_before < 0x4000) capped++;
        if (memcmp(GPTR(VS_BITSET), initial + (VS_BITSET - windows[2].base + windows[0].size + windows[1].size), 0x80000u)) marked_any++;
        if (G32(entry - 0x64u - 0x24u) != 0xA5A5A5A5u) corner_frames++;
        /* A bitset write into the saved registers comes back out of the pops. */
        if (original.gpr[3] != start->gpr[3] || original.gpr[5] != start->gpr[5] ||
            original.gpr[6] != start->gpr[6] || original.gpr[7] != start->gpr[7]) saved_hit++;
        /* A bitset write into the BSP data. */
        const size_t data = windows[0].size + windows[1].size + windows[2].size;
        if (memcmp(GPTR(DATA), initial + data, entry - 0x100u - DATA)) bsp_hit++;
        restore(initial);
    }
    EngineCPU native = *start;
    int handled = host_mark_visible_surfaces(&native);
    const char *field = "memory";
    if (handled != expect_native) {
        fprintf(stderr, "FAIL %s %u (seed %016llx): native %s, expected it to %s\n", label, scene, (unsigned long long)seed,
                handled ? "ran" : "declined", expect_native ? "run" : "decline"); exit(1);
    }
    if (!handled) {
        uint32_t at = differs(initial);
        if (at || !cpu_equal(&native, start, &field)) {
            fprintf(stderr, "FAIL %s %u: the declined call changed %s (at %08x)\n", label, scene, field, at); exit(1);
        }
        declined++;
        return;
    }
    assert(run_translated);
    uint32_t at = differs(translated);
    if (at || !cpu_equal(&native, &original, &field)) {
        fprintf(stderr, "FAIL %s %u (seed %016llx): %s differs", label, scene, (unsigned long long)seed, field);
        if (at) {
            size_t offset = 0;
            for (int i = 0; i < WINDOWS; i++) {
                if (at >= windows[i].base && at < windows[i].base + windows[i].size) { offset += at - windows[i].base; break; }
                offset += windows[i].size;
            }
            fprintf(stderr, " at %08x: native %02x translated %02x (entry %08x)", at, G8(at), translated[offset], entry);
        }
        fputc('\n', stderr); dump_cpu("native    ", &native); dump_cpu("translated", &original);
        exit(1);
    }
    compared++;
    native_top_mask |= 1u << start->fp_top;
}

/* One cluster of 40,000 subclusters. 00553920 indexes them with a 16-bit
 * register and compares that, sign-extended, with the 32-bit count, so past
 * 32,767 the index wraps to -32,768 and the loop runs until the triangle cap
 * stops it. The first 32,768 are outside the frustum's bounds; the wrapped
 * ones below the array are inside it with one new triangle each. */
static void check_wrapped_subclusters(void) {
    WINDOWS = ALL_WINDOWS;
    for (int i = 0; i < WINDOWS; i++) memset(GPTR(windows[i].base), 0, windows[i].size);
    const uint32_t entry = DATA + DATA_SIZE - 0x40u, bsp = DATA, cluster = DATA + 0x200u;
    const uint32_t lowest = WIDE, array = WIDE + 36u * 32768u, lists = WIDE + 36u * 65536u;
    S32(bsp + 0x138u, cluster);
    S32(cluster + 0x34u, 40000u); S32(cluster + 0x38u, array);
    S16(VS_RECORD_COUNT, 1); S16(VS_RECORDS, 0);
    const uint32_t f = VS_RECORDS + 0x14u;
    for (uint32_t p = 0; p < 4; p++) { put_float(f + 0x78u + 16u * p + 4u * (p & 1u ? 1u : 0u), 1.f); put_float(f + 0x78u + 16u * p + 12u, 1e6f); }
    for (uint32_t k = 0; k < 3; k++) { put_float(f + 0x128u + 8u * k, -10.f); put_float(f + 0x12Cu + 8u * k, 10.f); }
    for (uint32_t s = 0; s < 65536u; s++) {
        uint32_t sub = lowest + 36u * s;
        float lo = s < 32768u ? 1.f : 100.f;   /* below the array: inside; from it on: past the bounds */
        for (uint32_t k = 0; k < 3; k++) { put_float(sub + 8u * k, lo); put_float(sub + 8u * k + 4u, lo + 1.f); }
        S32(sub + 0x18u, 1); S32(sub + 0x1Cu, lists + 4u * s); S32(lists + 4u * s, s);
    }
    memset(GPTR(entry - 0x200u), 0xA5, 0x200u);
    S32(entry, RETURN_ADDRESS); S32(entry + 4u, bsp);
    Kind kind = { 0, 0, 0, 0, 0, 0, 0 };
    EngineCPU start; random_cpu(&start, entry, &kind);
    start.fp_control = 0x027F;
    unsigned capped_before = capped;
    check("wrapped subclusters", 0, 0, &start, 1);
    if (capped != capped_before + 1 || last_translated.gpr[2] != 0xFFFFC000u) {
        fprintf(stderr, "FAIL: the wrapped-subcluster scene did not wrap to the cap (edx %08x)\n", last_translated.gpr[2]); exit(1);
    }
    WINDOWS = ALL_WINDOWS - 1;
    printf("40,000 subclusters in one cluster: the index wrapped and the cap stopped it; identical\n");
}

int main(void) {
    engine_flat_base = mmap(NULL, UINT64_C(1) << 32, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    if (engine_flat_base == MAP_FAILED) { perror("mmap"); return 1; }
    for (int i = 0; i < ALL_WINDOWS; i++)
        if (mprotect(GPTR(windows[i].base), windows[i].size, PROT_READ | PROT_WRITE)) { perror("mprotect"); return 1; }
    size_t bytes = window_bytes();
    initial = malloc(bytes); translated = malloc(bytes);
    assert(initial && translated);
    const uint32_t entry = DATA + DATA_SIZE - 0x40u;

    static const Kind kinds[] = {
        { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0 }, { 1, 1, 0, 0, 0, 0, 0 },
        { 2, 2, 0, 0, 0, 0, 0 }, { 3, 3, 0, 0, 0, 0, 0 }, { 4, 4, 0, 0, 0, 0, 0 }, { 0, 0, 1, 0, 0, 0, 0 }, { 0, 0, 2, 0, 0, 0, 0 },
        { 1, 1, 2, 0, 0, 0, 0 }, { 0, 0, 3, 0, 0, 0, 0 }, { 0, 0, 0, 0, 1, 0, 0 }, { 0, 0, 0, 1, 0, 0, 0 }, { 5, 5, 0, 0, 0, 0, 0 },
        { 5, 5, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 1, 0 }, { 1, 1, 0, 0, 0, 1, 0 }, { 4, 4, 1, 0, 0, 1, 0 },
        { 0, 0, 0, 0, 0, 0, 1 }, { 1, 1, 0, 0, 0, 0, 1 }, { 4, 4, 0, 0, 0, 1, 1 },
    };
    enum { KINDS = sizeof kinds / sizeof *kinds, SCENES = 6000 };
    unsigned tested_subclusters = 0;
    for (unsigned scene = 0; scene < SCENES; scene++) {
        const Kind *kind = &kinds[scene % KINDS];
        uint64_t seed = state;
        unsigned subclusters = 0;
        uint32_t bsp = build_scene(kind, &subclusters);
        memset(GPTR(entry - 0x200u), 0xA5, 0x200u);
        S32(entry, RETURN_ADDRESS); S32(entry + 4u, bsp);
        EngineCPU start; random_cpu(&start, entry, kind);
        /* Seven deep, the translated routine itself would overflow the x87 stack. */
        check("scene", scene, seed, &start, !kind->deep_x87);
        tested_subclusters += subclusters;
    }
    printf("compared %u scenes byte for byte (%u subclusters laid out; %u marked triangles, %u reached the 0x4000 cap, "
           "%u left a corner frame); %u declined as expected\n",
           compared, tested_subclusters, marked_any, capped, corner_frames, declined);
    printf("%u scenes marked a triangle whose bitset word is a saved register in the routine's frame, %u one in the BSP data\n",
           saved_hit, bsp_hit);
    if (capped < 50 || marked_any < 1000 || corner_frames < 1000 || declined < 400 || saved_hit < 50 || bsp_hit < 100) { fprintf(stderr, "FAIL: coverage too thin\n"); return 1; }
    if (native_top_mask != 0xFFu) { fprintf(stderr, "FAIL: not every x87 TOP was compared\n"); return 1; }
    puts("all eight initial x87 TOP values: identical physical registers (including empty slots), tags and status");
    check_wrapped_subclusters();

    /* Timing: one b30-sized view. 30 visible clusters of 14 subclusters,
     * 8-30 triangles each out of 24,000, boxes within 150 units of the eye,
     * every cluster seen through the whole view frustum. The bitset and
     * count are cleared before each call. */
    state = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < WINDOWS; i++) memset(GPTR(windows[i].base), 0, windows[i].size);
    eye[0] = 10; eye[1] = -20; eye[2] = 5;
    make_frustum(VS_VIEW_FRUSTUM, 0);
    bump = DATA;
    uint32_t bsp = take(0x200), cluster_array = take(0x68u * 30u);
    S32(bsp + 0x138u, cluster_array);
    unsigned subclusters = 0;
    for (uint32_t c = 0; c < 30; c++) {
        uint32_t cluster = cluster_array + 0x68u * c, array = take(36u * 14u);
        S32(cluster + 0x34u, 14); S32(cluster + 0x38u, array);
        for (uint32_t s = 0; s < 14; s++) {
            uint32_t sub = array + 36u * s;
            for (uint32_t k = 0; k < 3; k++) {
                double centre = eye[k] + (unit() * 2 - 1) * 150, half = 1 + 9 * unit();
                put_float(sub + 8u * k, (float)(centre - half)); put_float(sub + 8u * k + 4u, (float)(centre + half));
            }
            uint32_t triangles = 8 + below(23), list = take(4u * triangles);
            S32(sub + 0x18u, triangles); S32(sub + 0x1Cu, list);
            for (uint32_t t = 0; t < triangles; t++) S32(list + 4u * t, below(24000));
            subclusters++;
        }
        S16(VS_RECORDS + 0x1A0u * c, (uint16_t)c);
        memcpy(GPTR(VS_RECORDS + 0x1A0u * c + 0x14u), GPTR(VS_VIEW_FRUSTUM), 0x18Cu);
    }
    S16(VS_RECORD_COUNT, 30);
    S32(entry, RETURN_ADDRESS); S32(entry + 4u, bsp);
    EngineCPU base; memset(&base, 0, sizeof base); base.gpr[4] = entry; base.pc = 0x00553920u; base.fp_control = 0x027F; base.failure = translated_failure;
    enum { ROUNDS = 3000 };
    const size_t clear = 24000 / 8 + 4;
    unsigned visible_marks = 0;
    double t0 = now();
    for (int r = 0; r < ROUNDS; r++) { memset(GPTR(VS_BITSET), 0, clear); S16(VS_MARKED, 0); }
    double t1 = now();
    for (int r = 0; r < ROUNDS; r++) { memset(GPTR(VS_BITSET), 0, clear); S16(VS_MARKED, 0); EngineCPU c = base; sub_00553920(&c); visible_marks += G16(VS_MARKED); }
    double t2 = now();
    for (int r = 0; r < ROUNDS; r++) { memset(GPTR(VS_BITSET), 0, clear); S16(VS_MARKED, 0); EngineCPU c = base; host_mark_visible_surfaces(&c); visible_marks -= G16(VS_MARKED); }
    double t3 = now();
    double translated_us = ((t2 - t1) - (t1 - t0)) / ROUNDS * 1e6, native_us = ((t3 - t2) - (t1 - t0)) / ROUNDS * 1e6;
    printf("timing, %u subclusters, %u triangles marked a call: translated %.1f us (%.0f ns a subcluster), native %.2f us (%.1f ns a subcluster), %.0fx\n",
           subclusters, G16(VS_MARKED), translated_us, translated_us * 1e3 / subclusters, native_us, native_us * 1e3 / subclusters,
           translated_us / native_us);
    if (visible_marks != 0) { fprintf(stderr, "FAIL: the timed runs marked different counts\n"); return 1; }
    puts("PASS: native 00553920 matches the translated routine in memory, registers, flags and x87 state");
    return 0;
}

/* The translated routines, compiled here as the generated chunks compile them. */
#include "engine_registers.h"
#include "sub_0050D5B0.c"
#include "sub_00553920.c"
#else
int main(void) { puts("SKIP: generated engine not found on the include path"); return 0; }
#endif
