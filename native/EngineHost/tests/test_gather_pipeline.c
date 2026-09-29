/* Whole-gather differential against generated 00553D80 and all its callees.
 * Randomized BSP trees, leaves, clusters, portals, volumes and per-view bitsets;
 * every writable guest byte (including dead stack) and all GPR/flags/PC and
 * observable x87 state compared. BSP pages are read-only. Includes every BSP
 * in a supplied retail map (b30 by default); never writes or bundles game data.
 * Run through tools/run_source_checks.py; requires generated sources on -I.
 */
#if !__has_include("sub_00553D80.c")
#include <stdio.h>
int main(void) { puts("SKIP: needs the Build75 generated engine (native/build/engine-reuse/desktop-build75-direct)"); return 0; }
#else
#include "../host.h"
#include "engine_flags.h"
#include "engine_hooks.h"
#include <assert.h>
#include <stdarg.h>
#include <time.h>
#include <sys/mman.h>

uint8_t *engine_flat_base;
void host_log(const char *format, ...) {
    va_list args; va_start(args, format);
    fputs("  log: ", stdout); vprintf(format, args); putchar('\n');
    va_end(args);
}
void sub_00553D80(EngineCPU *); void sub_00628240(EngineCPU *); void sub_00553C40(EngineCPU *);
void sub_005541B0(EngineCPU *); void sub_00554260(EngineCPU *); void sub_00553F10(EngineCPU *);
void sub_00553380(EngineCPU *); void sub_005540C0(EngineCPU *); void sub_005013A0(EngineCPU *);
void sub_00554CB0(EngineCPU *); void sub_00554D10(EngineCPU *); void sub_00554B00(EngineCPU *);
void sub_0044D820(EngineCPU *); void sub_004CAD80(EngineCPU *);

#include "../native_leaves.h"
#include "../native_gather.h"
#include "../native_gather_tree.h"
static int reference_run, baseline_native_leaves;
static unsigned native_calls[3], native_declined[3];
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    cpu->pc = address;
    if (!reference_run) {
        int taken = 0, slot = -1;
        if (address == 0x005540C0u) { slot = 0; taken = gather_native_leaf(cpu); }
        if (address == 0x00553C40u) { slot = 1; taken = gather_native_clusters(cpu); }
        if (address == 0x00553F10u) { slot = 2; taken = gather_native_tree(cpu); }
        if (slot >= 0) { if (taken) native_calls[slot]++; else native_declined[slot]++; }
        if (taken) return;
    }
    if (!reference_run || baseline_native_leaves) {
        if (address == 0x00553380u && leaf_bsp_node_bounds(cpu)) return;
        if (address == 0x005541B0u && leaf_bounds_overlap(cpu)) return;
        if (address == 0x00554260u && leaf_bounds_planes(cpu)) return;
    }
    switch (address) {
    case 0x00553D80u: sub_00553D80(cpu); break;
    case 0x00553F10u: sub_00553F10(cpu); break;
    case 0x005540C0u: sub_005540C0(cpu); break;
    case 0x00553C40u: sub_00553C40(cpu); break;
    case 0x00553380u: sub_00553380(cpu); break;
    case 0x005541B0u: sub_005541B0(cpu); break;
    case 0x00554260u: sub_00554260(cpu); break;
    default: fprintf(stderr, "unexpected dispatch %08x\n", address); abort();
    }
}
#define GATHER_MAX 0x1000u
#define GATHER_WORDS_MAX 0x1000u
#define MEMORY      0x01000000u   /* synthetic data; the mapping reaches the map's BSP addresses */
#define MAPPING     0x42000000u
#define HEAP        0x00100000u
#define STACK_TOP   0x003F0000u
#define DEAD        0x00020000u   /* stack below the caller's esp the gather may use */
#define SBSP        0x00870000u   /* level data from here up: read-only while calls run */
#define BSP3D       (SBSP + 0x200u)
#define DATA        (SBSP + 0x1000u)
static uint8_t *before, *reference;

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng >> 12; rng ^= rng << 25; rng ^= rng >> 27; return (uint32_t)((rng * 0x2545F4914F6CDD1Dull) >> 32); }
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFFFF) / 16777216.0f; }
static void SF(uint32_t a, float v) { memcpy(GPTR(a), &v, 4); }
static uint32_t cursor;
static uint32_t take(uint32_t bytes) { uint32_t a = (cursor + 15u) & ~15u; cursor = a + bytes; assert(cursor < MEMORY); return a; }

typedef struct { int triangles, clusters, depth, refs, subclusters, portals, hot, subcluster_ids; } Shape;
static int nodes, leaves, planes_used, refs_used;
static uint32_t nodes_at, fractions_at, leaves_at, refs_at, planes_at;
static const Shape *shape;

static void random_box(float *b, float span) {
    for (int k = 0; k < 3; k++) { float c = frand(-40, 40), h = frand(0.2f, span); b[2 * k] = c - h; b[2 * k + 1] = c + h; }
}
static uint32_t triangle_id(void) {
    /* A small hot set makes the walk meet the same triangle again. */
    return (rnd() % 3 == 0) ? rnd() % (uint32_t)shape->hot : rnd() % (uint32_t)shape->triangles;
}
/* A child's box as fractions of its parent's, low then high per axis (0xFF
 * is the parent's high edge); now and then inverted, which the walk must
 * treat the same way both times. */
static void random_fractions(uint32_t at) {
    for (int k = 0; k < 3; k++) {
        uint8_t lo = (uint8_t)(rnd() % 3 ? rnd() % 60u : 0u), hi = (uint8_t)(rnd() % 3 ? 0xFFu : 180u + rnd() % 76u);
        if (rnd() % 40 == 0) { uint8_t t = lo; lo = hi; hi = t; }
        S8(at + 2u * (uint32_t)k, lo); S8(at + 2u * (uint32_t)k + 1u, hi);
    }
}
static int32_t build_node(int depth) {
    if (depth >= shape->depth || (depth > 2 && rnd() % 5 == 0)) {
        if (rnd() % 25 == 0) return -1;
        int leaf = leaves++;
        uint32_t at = leaves_at + 16u * (uint32_t)leaf;
        random_fractions(at);
        S16(at + 8, (uint16_t)(rnd() % 30 == 0 ? 0xFFFF : rnd() % (uint32_t)shape->clusters));
        int count = (int)(rnd() % (uint32_t)(shape->refs + 1));
        S16(at + 10, (uint16_t)count); S32(at + 12, (uint32_t)refs_used);
        assert(refs_used + count <= 262144);
        for (int i = 0; i < count; i++) { S32(refs_at + 8u * (uint32_t)refs_used, triangle_id()); S32(refs_at + 8u * (uint32_t)refs_used + 4u, rnd()); refs_used++; }
        return (int32_t)(0x80000000u | (uint32_t)leaf);
    }
    int node = nodes++;
    uint32_t at = nodes_at + 12u * (uint32_t)node;
    S32(at, rnd() % (uint32_t)planes_used);
    random_fractions(fractions_at + 6u * (uint32_t)node);
    int32_t a = build_node(depth + 1), b = build_node(depth + 1);
    S32(at + 4, (uint32_t)a); S32(at + 8, (uint32_t)b);
    return node;
}
/* Level data is read-only while calls run, so a stray write faults. */
static void protect_level(int read_only) {
    assert(!mprotect(engine_flat_base + SBSP, MEMORY - SBSP, read_only ? PROT_READ : PROT_READ | PROT_WRITE));
}
static void build_bsp(const Shape *s) {
    shape = s;
    protect_level(0);
    memset(GPTR(SBSP), 0, MEMORY - SBSP);
    cursor = DATA;
    nodes = leaves = refs_used = 0;
    planes_used = 256;
    planes_at = take(16u * 256u); nodes_at = take(12u * 4096u); fractions_at = take(6u * 4096u);
    leaves_at = take(16u * 4096u); refs_at = take(8u * 262144u);
    for (int p = 0; p < planes_used; p++) {
        float n[3];
        if (p % 2) { n[0] = n[1] = n[2] = 0; n[rnd() % 3] = rnd() & 1 ? 1.f : -1.f; }
        else { float l; do { for (int k = 0; k < 3; k++) n[k] = frand(-1, 1); l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); } while (l < 0.1f); for (int k = 0; k < 3; k++) n[k] /= l; }
        for (int k = 0; k < 3; k++) SF(planes_at + 16u * (uint32_t)p + 4u * (uint32_t)k, n[k]);
        SF(planes_at + 16u * (uint32_t)p + 12u, frand(-30, 30));
    }
    S32(BSP3D + 4, nodes_at); S32(BSP3D + 16, planes_at);
    int32_t root = build_node(0);
    assert(root == 0);
    S32(SBSP + 0xB4, BSP3D); S32(SBSP + 0xC0, fractions_at);
    float world[6] = { -45, 45, -45, 45, -45, 45 };
    memcpy(GPTR(SBSP + 0xC8), world, sizeof world);
    S32(SBSP + 0xE4, leaves_at); S32(SBSP + 0xF0, refs_at);
    S32(SBSP + 0xF8, (uint32_t)s->triangles); S32(SBSP + 0xFC, take(6u * (uint32_t)s->triangles));
    uint32_t clusters = take(0x68u * (uint32_t)s->clusters), portals = take(64u * (uint32_t)s->portals);
    S32(SBSP + 0x134, (uint32_t)s->clusters); S32(SBSP + 0x138, clusters); S32(SBSP + 0x158, portals);
    for (int p = 0; p < s->portals; p++) {
        uint32_t at = portals + 64u * (uint32_t)p;
        uint16_t front = (uint16_t)(rnd() % (uint32_t)s->clusters), back = (uint16_t)((front + 1u + rnd() % (uint32_t)(s->clusters - 1)) % (uint32_t)s->clusters);
        S16(at, front); S16(at + 2, back); S32(at + 4, rnd() % (uint32_t)planes_used);
        float c[3] = { frand(-40, 40), frand(-40, 40), frand(-40, 40) };
        for (int k = 0; k < 3; k++) SF(at + 8u + 4u * (uint32_t)k, c[k]);
        SF(at + 0x14, frand(1, 25));
        uint32_t count = 3u + rnd() % 6u, vertices = take(12u * count);
        S32(at + 0x34, count); S32(at + 0x38, vertices);
        for (uint32_t v = 0; v < count; v++) for (int k = 0; k < 3; k++) SF(vertices + 12u * v + 4u * (uint32_t)k, c[k] + frand(-12, 12));
    }
    for (int c = 0; c < s->clusters; c++) {
        uint32_t at = clusters + 0x68u * (uint32_t)c;
        uint32_t subs = rnd() % (uint32_t)(s->subclusters + 1), sub = take(36u * subs + 4u);
        S32(at + 0x34, subs); S32(at + 0x38, sub);
        for (uint32_t i = 0; i < subs; i++) {
            float b[6]; random_box(b, 20); memcpy(GPTR(sub + 36u * i), b, 24);
            uint32_t n = rnd() % (uint32_t)s->subcluster_ids, ids = take(4u * n + 4u);
            S32(sub + 36u * i + 0x18, n); S32(sub + 36u * i + 0x1C, ids);
            for (uint32_t k = 0; k < n; k++) S32(ids + 4u * k, triangle_id());
        }
        uint32_t links = 0, list = take(2u * (uint32_t)s->portals + 4u);
        for (int p = 0; p < s->portals; p++) {
            uint32_t pa = portals + 64u * (uint32_t)p;
            if (G16(pa) == c || G16(pa + 2) == c || rnd() % 16 == 0) S16(list + 2u * links++, (uint16_t)p);
        }
        S32(at + 0x5C, links); S32(at + 0x60, list);
    }
    protect_level(1);
}
static void reset_globals(void) {
    memset(GPTR(0x00600000u), 0, SBSP - 0x00600000u);
    /* The dominant-axis table 00554BE5 reads, constants and threshold. */
    static const int16_t axes[12] = { 2, 1, 1, 2, 0, 2, 2, 0, 1, 0, 0, 1 };   /* halo.exe's values */
    memcpy(GPTR(0x0065C29Cu), axes, sizeof axes);
    SF(0x00672AC0u, 0.0f); SF(0x00672AD4u, 1.0f / 255.0f); SF(0x0069FA4Cu, 2.0f);
    S32(0x00746F9Cu, SBSP); S32(0x00746F90u, BSP3D);
    S32(0x006E3F04u, 1000u + rnd() % 1000u);
    for (uint32_t c = 0; c < 0x400u; c++) S32(0x006E3F08u + 4u * c, rnd() % 1000u);
}
static void random_bitset(unsigned percent) {
    uint32_t words = ((uint32_t)shape->triangles + 31u) >> 5;
    for (uint32_t w = 0; w < words; w++) {
        uint32_t v = 0;
        for (int b = 0; b < 32; b++) v |= (uint32_t)(rnd() % 100u < percent) << b;
        S32(0x007D0394u + 4u * w, v);
    }
}

typedef struct {
    float center[3], radius, box[6], planes[32][4];
    int has_box, has_list;
    int32_t plane_count, cluster_count;
    uint16_t list[64];
} Query;
static void random_query(Query *q, int kind) {
    memset(q, 0, sizeof *q);
    for (int k = 0; k < 3; k++) q->center[k] = frand(-42, 42);
    static const float radii[] = { 0.3f, 1.2f, 1.999f, 2.0f, 2.5f, 4.f, 7.f, 15.f };
    q->radius = radii[rnd() % 8];
    if (kind == 0) {           /* a light: a cluster list, no planes */
        q->has_list = rnd() % 12 != 0;
        q->cluster_count = (int32_t)(rnd() % 6u);
        for (int i = 0; i < 64; i++) q->list[i] = (uint16_t)(rnd() % (uint32_t)shape->clusters);
    } else {                   /* a shadow: a box and clip planes, no list */
        q->has_box = 1;
        for (int k = 0; k < 3; k++) { q->box[2 * k] = q->center[k] - frand(0.1f, 3.f); q->box[2 * k + 1] = q->center[k] + frand(0.1f, 3.f); }
        q->plane_count = rnd() % 5 ? 6 : (int32_t)(rnd() % 4u);
        for (int p = 0; p < 32; p++) {
            float n[3] = { frand(-1, 1), frand(-1, 1), frand(-1, 1) };
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-3f;
            for (int k = 0; k < 3; k++) q->planes[p][k] = n[k] / l;
            q->planes[p][3] = q->planes[p][0] * q->center[0] + q->planes[p][1] * q->center[1] + q->planes[p][2] * q->center[2] - frand(-1, 4);
        }
        if (rnd() % 5 == 0) {   /* a box and a list together */
            q->has_list = 1; q->cluster_count = 1 + (int32_t)(rnd() % 3u);
            for (int i = 0; i < 64; i++) q->list[i] = (uint16_t)(rnd() % (uint32_t)shape->clusters);
        }
    }
    switch (rnd() % 24) {      /* edge cases */
    case 0: q->radius = NAN; break;
    case 1: q->radius = 0.f; break;
    case 2: q->radius = -1.f; break;
    case 3: q->radius = INFINITY; break;
    case 4: q->radius = 60.f; break;                         /* reaches the maximum */
    case 5: q->center[0] = 400.f; break;                     /* outside the BSP */
    case 6: q->planes[1][2] = NAN; break;
    case 7: q->cluster_count = -3; break;
    case 8: q->plane_count = -2; break;
    case 9: q->cluster_count = 64; q->has_list = 1; break;
    default: break;
    }
}
static uint32_t place_query(const Query *q, uint32_t *box, uint32_t *center, uint32_t *planes, uint32_t *list) {
    uint32_t at = HEAP + (rnd() % 64u) * 0x400u;   /* by value: the address moves every call */
    *center = at; memcpy(GPTR(at), q->center, 12);
    *box = q->has_box ? at + 0x10u : 0; memcpy(GPTR(at + 0x10u), q->box, 24);
    *planes = q->plane_count != 0 ? at + 0x40u : 0; memcpy(GPTR(at + 0x40u), q->planes, sizeof q->planes);
    *list = q->has_list ? at + 0x300u : 0; memcpy(GPTR(at + 0x300u), q->list, sizeof q->list);
    return at;
}
static const uint32_t callers[3] = { 0x00552AA7u, 0x005529C7u, 0x00552B7Du };
static uint32_t current_esp, current_out;
static void setup_call(EngineCPU *cpu, const Query *q, uint32_t caller, uint16_t control) {
    uint32_t box, center, planes, list;
    place_query(q, &box, &center, &planes, &list);
    uint32_t esp = STACK_TOP - 0x4000u - (rnd() % 256u) * 4u;
    uint32_t out = esp + 0x40u + (rnd() % 16u) * 4u;
    for (uint32_t a = esp - DEAD; a < esp; a += 4) S32(a, rnd());
    for (uint32_t i = 0; i < GATHER_MAX; i++) S32(out + 4u * i, rnd());
    uint32_t args[8] = { caller, out, GATHER_MAX, 0, (uint32_t)q->plane_count, planes, (uint32_t)q->cluster_count, list };
    memcpy(&args[3], &q->radius, 4);
    memcpy(GPTR(esp), args, sizeof args);
    memset(cpu, 0, sizeof *cpu);
    for (int r = 0; r < 8; r++) cpu->gpr[r] = rnd();
    cpu->gpr[1] = box; cpu->gpr[2] = center; cpu->gpr[4] = esp;
    cpu->flags = 0x202u | (rnd() & ENGINE_EFLAGS_STATUS_MASK);
    for (int i = 0; i < 8; i++) cpu->fp_reg[i] = frand(-1e6f, 1e6f);   /* every physical register */
    cpu->fp_top = (uint8_t)(rnd() & 7u); cpu->fp_valid = 0;
    cpu->fp_control = control; cpu->fp_status = (uint16_t)(rnd() & 0xC7FFu);
    cpu->pc = 0x00553D80u;
    current_esp = esp; current_out = out;
}
static struct { unsigned calls, saturated, live_x87; uint64_t ids; } tally;
static void compare(const EngineCPU *a, const EngineCPU *b) {
    int same_memory = !memcmp(reference, engine_flat_base, SBSP);
    int same = a->pc == b->pc && !memcmp(a->gpr, b->gpr, sizeof a->gpr) &&
               a->flags == b->flags && a->fp_status == b->fp_status && a->fp_top == b->fp_top &&
               a->fp_valid == b->fp_valid && a->fp_control == b->fp_control;
    for (int i = 0; i < 8; i++) if (a->fp_valid & (1u << i)) same = same && !memcmp(&a->fp_reg[engine_fp_physical(a, (unsigned)i)], &b->fp_reg[engine_fp_physical(b, (unsigned)i)], 8);
    same = same && a->fp_top == b->fp_top && !memcmp(a->fp_reg, b->fp_reg, sizeof a->fp_reg);
    if (!same_memory || !same) {
        printf("FAIL call=%u: memory %s pc %08x/%08x flags %x/%x fpsw %x/%x\n",
               tally.calls, same_memory ? "same" : "differs", a->pc, b->pc, a->flags, b->flags, a->fp_status, b->fp_status);
        for (int i = 0; i < 8; i++) if (a->gpr[i] != b->gpr[i]) printf("  gpr[%d] %08x/%08x\n", i, a->gpr[i], b->gpr[i]);
        for (uint32_t i = 0; i < SBSP && !same_memory; i++)
            if (reference[i] != engine_flat_base[i]) { printf("  first byte differs at %08x (esp=%08x)\n", i, current_esp); break; }
        exit(1);
    }
}
/* Every writable byte, including the entire dead stack, starts identically.
 * The BSP is protected read-only and everything outside the arena inaccessible. */
static void check_call(const Query *q, uint32_t caller, uint16_t control) {
    EngineCPU cpu;
    setup_call(&cpu, q, caller, control);
    /* The unrelated portal flood can use the whole x87 stack. Test live
     * values on the tree/explicit-cluster routes that our helpers replace. */
    unsigned depth = (q->has_list || q->radius < 2.f) ? tally.calls % 5u : 0u;
    cpu.fp_valid = (uint8_t)((1u << depth) - 1u);
    tally.live_x87 += depth != 0;
    memcpy(before, engine_flat_base, SBSP);
    EngineCPU start = cpu;
    reference_run = 1; sub_00553D80(&cpu); reference_run = 0;
    assert(cpu.pc == caller && cpu.gpr[4] == start.gpr[4] + 4u);
    EngineCPU original = cpu;
    memcpy(reference, engine_flat_base, SBSP);
    memcpy(engine_flat_base, before, SBSP);
    cpu = start;
    sub_00553D80(&cpu);
    compare(&original, &cpu);
    tally.calls++; tally.ids += cpu.gpr[0] & 0xFFFFu;
    tally.saturated += (cpu.gpr[0] & 0xFFFFu) == GATHER_MAX;
}

static const Shape shapes[] = {
    { 3000, 12, 9, 30, 5, 40, 200, 90 },
    { 900, 24, 7, 12, 3, 60, 50, 40 },
    { 12000, 8, 11, 60, 6, 24, 800, 1500 },   /* long lists: the 0x1000 maximum is reachable */
};
static void differential(void) {
    static const uint16_t controls[] = { 0x027Fu, 0x037Fu, 0x007Fu, 0x0E7Fu };
    static const unsigned densities[] = { 0, 3, 40, 85, 100 };
    for (unsigned s = 0; s < sizeof shapes / sizeof *shapes; s++) {
        build_bsp(&shapes[s]); reset_globals();
        Query queries[24];
        for (int i = 0; i < 24; i++) random_query(&queries[i], i % 2);
        for (int frame = 0; frame < 4; frame++) {
            S32(0x007C3100u, 5000u + (uint32_t)frame);
            if (frame == 2) {  /* a stale stamp at the next generation: the flood must run */
                uint32_t g = G32(0x006E3F04u);
                S32(0x006E3F08u + 4u * (rnd() % (uint32_t)shapes[s].clusters), g + 1u);
            }
            if (frame == 3) S32(0x006E3F04u, G32(0x006E3F04u) + 7u);
            for (int i = frame; i < 24; i += 3) random_query(&queries[i], i % 2);   /* some change each frame */
            for (int bearing = 0; bearing < 5; bearing++) {
                random_bitset(densities[(bearing + frame) % 5]);
                for (int i = 0; i < 24; i++) {
                    uint16_t control = controls[(i / 6 + (frame == 1)) % 4];
                    for (unsigned c = 0; c < 3; c++) if ((i + c + (unsigned)bearing) % 2 == 0 || c == 2 - (unsigned)(i % 3))
                        check_call(&queries[i], callers[c], control);
                }
            }
        }
    }
    /* Dense visible clusters force the exact original cap/early exit. */
    random_bitset(100);
    Query cap = {0}; cap.radius = 1000.f; cap.has_list = 1;
    cap.cluster_count = shapes[2].clusters;
    for (int i = 0; i < cap.cluster_count; i++) cap.list[i] = (uint16_t)i;
    for (int i = 0; i < 5; i++) check_call(&cap, callers[i % 3], controls[i % 4]);
    assert(tally.saturated > 0);
    printf("random BSP differential: %u calls, %.0f ids/list, all writable memory + CPU/x87: PASS\n",
           tally.calls, (double)tally.ids / tally.calls);
}

/* Retail map geometry, read only, at its original BSP addresses. */
static uint8_t *map_data; static size_t map_size;
static const char *map_label;
static uint32_t map_u32(size_t at) { uint32_t v = 0; if (at + 4 <= map_size) memcpy(&v, map_data + at, 4); return v; }
static int map_open(void) {
    const char *path = getenv("HALO_TEST_GATHER_MAP");
    if (!path) path = getenv("HALO_TEST_B30_MAP");
    int required = path != NULL;
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) { assert(!required && "requested gather map is missing"); return 0; }
    map_label = strrchr(path, '/'); map_label = map_label ? map_label + 1 : path;
    fseek(f, 0, SEEK_END); map_size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    map_data = malloc(map_size);
    int ok = map_data && fread(map_data, 1, map_size, f) == map_size && !memcmp(map_data, "daeh", 4);
    fclose(f);
    assert(ok && "invalid retail map");
    return ok;
}
static uint32_t map_bsp_count(void) {
    uint32_t base = 0x40440000u, tags = map_u32(0x10);
    uint32_t tag_array = map_u32(tags), scenario = map_u32(tags + 4);
    uint32_t data = map_u32((size_t)tags + tag_array - base + 0x20u * (scenario & 0xFFFFu) + 0x14);
    return map_u32((size_t)tags + data - base + 0x5A4u);
}
static uint32_t level_va, level_size;
static int map_load_bsp(uint32_t index, uint32_t *sbsp) {
    const uint32_t base = 0x40440000u, tags = map_u32(0x10);
#define MAP_AT(va) ((size_t)tags + ((va) - base))
    uint32_t tag_array = map_u32(tags), scenario = map_u32(tags + 4);
    uint32_t data = map_u32(MAP_AT(tag_array + 0x20u * (scenario & 0xFFFFu)) + 0x14);
    uint32_t count = map_u32(MAP_AT(data + 0x5A4u)), entries = map_u32(MAP_AT(data + 0x5A4u) + 4);
    if (index >= count) return 0;
    size_t entry = MAP_AT(entries + 32u * index);
#undef MAP_AT
    uint32_t file_offset = map_u32(entry), size = map_u32(entry + 4), va = map_u32(entry + 8);
    if (!size || (size_t)file_offset + size > map_size || va < SBSP + MEMORY || (uint64_t)va + size > MAPPING) return 0;
    if (level_size) assert(!mprotect(engine_flat_base + (level_va & ~0x3FFFu), ((level_va + level_size + 0x3FFFu) & ~0x3FFFu) - (level_va & ~0x3FFFu), PROT_READ | PROT_WRITE));
    assert(!mprotect(engine_flat_base + (va & ~0x3FFFu), ((va + size + 0x3FFFu) & ~0x3FFFu) - (va & ~0x3FFFu), PROT_READ | PROT_WRITE));
    memcpy(engine_flat_base + va, map_data + file_offset, size);
    level_va = va; level_size = size;
    assert(!mprotect(engine_flat_base + (va & ~0x3FFFu), ((va + size + 0x3FFFu) & ~0x3FFFu) - (va & ~0x3FFFu), PROT_READ));
    *sbsp = G32(va);
    return 1;
}
static uint32_t level_sbsp, level_clusters, level_triangles;
static void level_globals(uint32_t sbsp) {
    reset_globals();
    level_sbsp = sbsp; level_clusters = G32(sbsp + 0x134); level_triangles = G32(sbsp + 0xF8);
    S32(0x00746F9Cu, sbsp); S32(0x00746F90u, G32(sbsp + 0xB4));
}
/* A pass's bitset: the camera cluster's PVS, with a third of its
 * subclusters culled, as the frustum would. */
static void level_bitset(uint32_t camera, unsigned cull) {
    uint32_t words = (level_triangles + 31u) >> 5, row = (level_clusters + 31u) >> 5;
    memset(GPTR(0x007D0394u), 0, 4u * words);
    uint32_t pvs = G32(level_sbsp + 0x14C) + 4u * row * camera, clusters = G32(level_sbsp + 0x138);
    for (uint32_t c = 0; c < level_clusters; c++) {
        if (!((G32(pvs + 4u * (c >> 5)) >> (c & 31u)) & 1u)) continue;
        uint32_t cluster = clusters + 0x68u * c, subs = G32(cluster + 0x34), sub = G32(cluster + 0x38);
        for (uint32_t i = 0; i < subs; i++) {
            if (rnd() % 100u < cull) continue;
            uint32_t n = G32(sub + 36u * i + 0x18), ids = G32(sub + 36u * i + 0x1C);
            for (uint32_t k = 0; k < n; k++) {
                uint32_t t = G32(ids + 4u * k);
                if (t < level_triangles) S32(0x007D0394u + 4u * (t >> 5), G32(0x007D0394u + 4u * (t >> 5)) | (1u << (t & 31u)));
            }
        }
    }
}
/* A point in the level: inside a random subcluster's box. */
static uint32_t level_point(float *p) {
    assert(level_clusters > 0);
    uint32_t first = rnd() % level_clusters;
    for (uint32_t visited = 0; visited < level_clusters; visited++) {
        uint32_t c = (first + visited) % level_clusters;
        uint32_t cluster = G32(level_sbsp + 0x138) + 0x68u * c;
        uint32_t subs = G32(cluster + 0x34);
        if (!subs) continue;
        float box[6]; memcpy(box, GPTR(G32(cluster + 0x38) + 36u * (rnd() % subs)), sizeof box);
        for (int k = 0; k < 3; k++) p[k] = box[2 * k] + frand(0, 1) * (box[2 * k + 1] - box[2 * k]);
        return c;
    }
    /* The final Maw BSP has one empty cluster and no render surfaces.
     * Exercise empty results from a valid cluster instead of spinning forever. */
    p[0] = p[1] = p[2] = 0.f;
    return 0;
}
static void level_query(Query *q, int kind) {
    memset(q, 0, sizeof *q);
    uint32_t c = level_point(q->center);
    if (kind == 0) {       /* a light: its cluster and the ones through its portals */
        static const float radii[] = { 1.0f, 1.5f, 2.5f, 3.0f, 4.0f, 6.0f };
        q->radius = radii[rnd() % 6];
        q->has_list = 1; q->list[0] = (uint16_t)c; q->cluster_count = 1;
        uint32_t cluster = G32(level_sbsp + 0x138) + 0x68u * c, links = G32(cluster + 0x5C), list = G32(cluster + 0x60);
        for (uint32_t i = 0; i < links && q->cluster_count < 4; i++) {
            uint32_t portal = G32(level_sbsp + 0x158) + 64u * G16(list + 2u * i);
            uint16_t other = G16(portal) == c ? G16(portal + 2) : G16(portal);
            if (other < level_clusters) q->list[q->cluster_count++] = other;
        }
    } else {               /* an object's shadow: its box, and a volume cast away from the light */
        float r = rnd() % 6 ? frand(0.25f, 1.4f) : frand(2.0f, 3.0f);
        q->radius = r; q->has_box = 1;
        for (int k = 0; k < 3; k++) { q->box[2 * k] = q->center[k] - r; q->box[2 * k + 1] = q->center[k] + r; }
        float d[3] = { frand(-0.5f, 0.5f), frand(-0.5f, 0.5f), -1.f }, l = sqrtf(d[0] * d[0] + d[1] * d[1] + 1.f);
        for (int k = 0; k < 3; k++) d[k] /= l;
        float u[3] = { 1, 0, 0 }, v[3];
        v[0] = d[1] * u[2] - d[2] * u[1]; v[1] = d[2] * u[0] - d[0] * u[2]; v[2] = d[0] * u[1] - d[1] * u[0];
        float lv = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); for (int k = 0; k < 3; k++) v[k] /= lv;
        u[0] = v[1] * d[2] - v[2] * d[1]; u[1] = v[2] * d[0] - v[0] * d[2]; u[2] = v[0] * d[1] - v[1] * d[0];
        const float *axes[3] = { u, v, d };
        q->plane_count = 6;
        for (int p = 0; p < 6; p++) {
            const float *a = axes[p / 2]; float sign = p % 2 ? -1.f : 1.f;
            float reach = p / 2 == 2 ? (p % 2 ? 4.f * r : r) : r;
            for (int k = 0; k < 3; k++) q->planes[p][k] = sign * a[k];
            q->planes[p][3] = sign * (a[0] * q->center[0] + a[1] * q->center[1] + a[2] * q->center[2]) - reach;
        }
    }
}
static void real_level(void) {
    if (!map_open()) { puts("real level: SKIP (no default b30.map; set HALO_TEST_GATHER_MAP)"); return; }
    uint64_t ids = tally.ids;
    unsigned calls = tally.calls;
    uint32_t bsp_count = map_bsp_count();
    assert(bsp_count > 0 && bsp_count <= 64);
    for (uint32_t bsp = 0; bsp < bsp_count; bsp++) {
        uint32_t sbsp; assert(map_load_bsp(bsp, &sbsp));
        level_globals(sbsp);
        printf("checking %s BSP %u: %u clusters, %u surfaces\n", map_label, bsp, level_clusters, level_triangles);
        assert(((level_triangles + 31u) >> 5) <= GATHER_WORDS_MAX);
        for (int frame = 0; frame < 3; frame++) {
            S32(0x007C3100u, 7000u + (uint32_t)frame);
            Query queries[32];
            for (int i = 0; i < 32; i++) level_query(&queries[i], i % 2);
            uint32_t camera = level_point(queries[0].center);
            for (int bearing = 0; bearing < 4; bearing++) {
                level_bitset(bearing < 2 ? camera : rnd() % level_clusters, 33);   /* 0 and 1: the centre pair */
                for (int i = 0; i < 32; i++) {
                    uint32_t caller = i % 2 ? callers[2] : callers[0];
                    check_call(&queries[i], caller, 0x027F);
                    if (i % 2 == 0 && !(frame == 2 && bearing == 3)) check_call(&queries[i], callers[1], 0x027F);   /* the specular pass */
                }
            }
        }
    }
    printf("real level %s (%u BSPs): %u calls, %.0f ids/list, all writable memory + CPU/x87: PASS\n",
           map_label, bsp_count, tally.calls - calls, (double)(tally.ids - ids) / (tally.calls - calls));
}
static double seconds(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }
/* Includes the entire original gather, with the existing Build78 native leaves
 * enabled for both baseline and candidate. No rendering or device FPS is measured. */
static void timing(void) {
    static const Shape big = { 20000, 32, 11, 40, 6, 80, 2000, 400 };
    uint32_t sbsp = 0;
    int level = map_data && map_load_bsp(0, &sbsp);
    if (level) level_globals(sbsp); else { build_bsp(&big); reset_globals(); }
    enum { N = 32, ROUNDS = 60 };
    Query queries[N];
    double original = 0, native = 0;
    unsigned calls = 0; uint64_t ids = 0;
    baseline_native_leaves = 1;
    for (int round = 0; round < ROUNDS; round++) {
        S32(0x007C3100u, 9000u + (uint32_t)round);
        for (int i = 0; i < N; i++) {
            if (level) level_query(&queries[i], i % 2);
            else { random_query(&queries[i], i % 2); if (!isfinite(queries[i].radius) || queries[i].radius <= 0.f || queries[i].radius > 15.f) queries[i].radius = 3.f; }
        }
        uint32_t camera = level ? level_point(queries[0].center) : 0;
        for (int bearing = 0; bearing < 3; bearing++) {
            if (level) level_bitset(bearing < 2 ? camera : rnd() % level_clusters, 33); else random_bitset(40);
            for (int i = 0; i < N; i++) for (int pass = 0; pass < (i % 2 ? 1 : 2); pass++) {
                uint32_t caller = i % 2 ? callers[2] : callers[pass];
                EngineCPU start;
                setup_call(&start, &queries[i], caller, 0x027F);
                /* Alternate order to reduce frequency/thermal bias; timings exclude
                 * snapshots. Both versions receive identical guest inputs. */
                memcpy(before, engine_flat_base, SBSP);
                for (int order = 0; order < 2; order++) {
                    memcpy(engine_flat_base, before, SBSP);
                    EngineCPU cpu = start;
                    reference_run = (order + round) % 2;
                    double t0 = seconds(); sub_00553D80(&cpu); double elapsed = seconds() - t0;
                    if (reference_run) original += elapsed; else native += elapsed;
                    if (!order) ids += cpu.gpr[0] & 0xFFFFu;
                }
                reference_run = 0; calls++;
            }
        }
    }
    printf("timing %s (%u calls, %.0f ids/list): Build78 gather %.2f us, native gather %.2f us, %.2fx; %.1f%% less gather time\n",
           level ? map_label : "synthetic", calls, (double)ids / calls,
           1e6 * original / calls, 1e6 * native / calls, original / native, 100.0 * (1.0 - native / original));
}
static void test_tree_fallback(void) {
    EngineCPU cpu = {0};
    cpu.gpr[4] = STACK_TOP;
    cpu.pc = 0x00553F10u;
    const uint8_t crowded[] = {0x10u, 0x20u, 0x40u, 0x80u, 0x1Fu, 0x3Fu, 0x7Fu, 0xFFu};
    memcpy(before, engine_flat_base, SBSP);
    for (unsigned i = 0; i < sizeof crowded; i++) {
        cpu.fp_valid = crowded[i];
        const EngineCPU entry = cpu;
        assert(!gather_native_tree(&cpu));
        assert(!memcmp(&entry, &cpu, sizeof cpu));
        assert(!memcmp(before, engine_flat_base, SBSP));
    }
    cpu.fp_valid = 0;
    cpu.instruction_limit = 1;
    EngineCPU entry = cpu;
    assert(!gather_native_tree(&cpu));
    assert(!memcmp(&entry, &cpu, sizeof cpu));
    assert(!memcmp(before, engine_flat_base, SBSP));
    cpu.instruction_limit = 0;
    const uint32_t bad_stacks[] = {0u, 0xFFu, 0xFFFFFFF0u};
    for (unsigned i = 0; i < sizeof bad_stacks / sizeof *bad_stacks; i++) {
        cpu.gpr[4] = bad_stacks[i];
        entry = cpu;
        assert(!gather_native_tree(&cpu));
        assert(!memcmp(&entry, &cpu, sizeof cpu));
        assert(!memcmp(before, engine_flat_base, SBSP));
    }
    puts("tree fallback: 8 crowded x87 states, bounded execution and 3 invalid stack spans decline without writes");
}
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    engine_flat_base = mmap(NULL, MAPPING, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    assert(engine_flat_base != MAP_FAILED);
    assert(!mprotect(engine_flat_base, MEMORY, PROT_READ | PROT_WRITE));
    before = malloc(SBSP); reference = malloc(SBSP);
    assert(before && reference);
    test_tree_fallback();
    differential();
    real_level();
    printf("coverage: %u capped results, %u calls with live x87 values\n", tally.saturated, tally.live_x87);
    timing();
    printf("native entry calls leaf=%u clusters=%u tree=%u; declined=%u/%u/%u\n",
           native_calls[0], native_calls[1], native_calls[2], native_declined[0], native_declined[1], native_declined[2]);
    assert(native_calls[1] && native_calls[2]);
    munmap(engine_flat_base, MAPPING); free(before); free(reference); free(map_data);
    puts("PASS: exact native light/shadow gather pipeline matches translated 00553D80");
    return 0;
}
#include "engine_registers.h"
#include "sub_00553D80.c"
#include "sub_00628240.c"
#include "sub_00553C40.c"
#include "sub_005541B0.c"
#include "sub_00554260.c"
#include "sub_00553F10.c"
#include "sub_00553380.c"
#include "sub_005540C0.c"
#include "sub_005013A0.c"
#include "sub_00554CB0.c"
#include "sub_00554D10.c"
#include "sub_00554B00.c"
#include "sub_0044D820.c"
#include "sub_004CAD80.c"
#endif
