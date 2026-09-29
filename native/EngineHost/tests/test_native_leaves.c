/* The native leaves in native_leaves.h against the translated engine.
 *
 * Each translated sub_XXXXXXXX.c is compiled here next to its native
 * version. Both start from the same guest memory and registers, on random
 * and awkward inputs (NaNs with payloads, infinities, subnormals, signed
 * zeros, equal edges, aliased and overlapping arguments, short and empty
 * lists, every walk shape), and the test compares every byte of the guest
 * memory either could reach (inputs, outputs, globals, and the stack below
 * and above the call, so the dead frame too), eax..edi, esp, the flags word,
 * the return pc, and the x87 control word, status word, top, tags and every
 * physical register (including empty slots exposed by FLDENV). The rest of the 4 GiB guest space has no access, so a
 * stray read or write faults. When a native version declines (overlaps it
 * cannot reorder, a non-nearest rounding mode, a full x87 stack) it must
 * leave everything untouched, and the translated function then runs.
 * Also times both.
 *
 * Needs the generated engine on the include path (run_source_checks.py
 * finds it); without it this prints SKIP.
 *
 * clang -O2 -DENGINE_FLAT_MEMORY=1 -I native/EngineHost -I native/EngineReuse -I <generated engine> \
 *   native/EngineHost/tests/test_native_leaves.c -lm -o /tmp/leaves && /tmp/leaves
 */
#include "host.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#if __has_include("engine_functions.h") && __has_include("sub_004CC0D0.c") && __has_include("sub_00552DE0.c")
#define HAVE_ENGINE 1
#include "engine_functions.h"
#include "engine_flags.h"
#include "engine_registers.h"
#include "sub_004CC0D0.c"
#include "sub_00553380.c"
#include "sub_00554260.c"
#include "sub_005541B0.c"
#include "sub_00552C20.c"
#include "sub_00552DE0.c"
#include "sub_0044AD80.c"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#undef eflags
#else
#define HAVE_ENGINE 0
#endif
#include "native_leaves.h"

uint8_t *engine_flat_base;

#if HAVE_ENGINE
static jmp_buf failed;
static const char *failure_reason;
static void on_failure(EngineCPU *cpu, const char *reason) { (void)cpu; failure_reason = reason; longjmp(failed, 1); }
/* The walk's callbacks are the only calls any of these make. */
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    cpu->pc = address;
    if (address == 0x0044AD80u) { sub_0044AD80(cpu); return; }
    fprintf(stderr, "unexpected call to %08X\n", address);
    abort();
}

/* ---- guest memory ---- */
#define DATA 0x20000000u
#define DATA_SIZE 0x40000u
#define STACK 0x30000000u
#define STACK_SIZE 0x10000u
typedef struct { uint32_t base, size; } Arena;
static const Arena arenas[] = {
    {0x00670000u, 0x4000u}, {0x0069C000u, 0x4000u}, {0x006B8000u, 0x4000u}, {0x00744000u, 0x4000u},
    {0x00878000u, 0x4000u}, {DATA, DATA_SIZE}, {STACK, STACK_SIZE},
};
#define ARENAS (sizeof arenas / sizeof *arenas)
static size_t arena_total;
static uint8_t *before, *reference;
static void save(uint8_t *to) {
    size_t at = 0;
    for (size_t i = 0; i < ARENAS; i++) { memcpy(to + at, engine_flat_base + arenas[i].base, arenas[i].size); at += arenas[i].size; }
}
static void load(const uint8_t *from) {
    size_t at = 0;
    for (size_t i = 0; i < ARENAS; i++) { memcpy(engine_flat_base + arenas[i].base, from + at, arenas[i].size); at += arenas[i].size; }
}
/* The guest address of the first byte that differs from the snapshot, or 0,
 * and the snapshot's byte there. */
static uint8_t snapshot_byte;
static uint32_t differs(const uint8_t *snapshot) {
    size_t at = 0;
    for (size_t i = 0; i < ARENAS; i++) {
        const uint8_t *live = engine_flat_base + arenas[i].base;
        if (memcmp(live, snapshot + at, arenas[i].size))
            for (uint32_t k = 0; k < arenas[i].size; k++)
                if (live[k] != snapshot[at + k]) { snapshot_byte = snapshot[at + k]; return arenas[i].base + k; }
        at += arenas[i].size;
    }
    return 0;
}

/* ---- random inputs ---- */
static uint64_t state = 0x243F6A8885A308D3ull;
static uint32_t rnd(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return (uint32_t)(state >> 11); }
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }
static float uniform(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) / 16777216.0f; }
static float bits_float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static float awkward(void) {
    static const uint32_t special[] = {
        0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u, 0x7FA12345u /* signalling */,
        0x7FFFFFFFu, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F800000u,
        0xBF800000u, 0x33800000u, 0x4B000000u };
    uint32_t r = below(4);
    if (r == 0) return bits_float(special[below(sizeof special / sizeof *special)]);
    if (r == 1) return bits_float(rnd() ^ (rnd() << 21));
    return uniform(-2.0f, 2.0f) * (float)(1u << below(12));
}
static float value(unsigned awkward_percent) { return below(100) < awkward_percent ? awkward() : uniform(-300.0f, 300.0f); }
static void put_float(uint32_t a, float f) { memcpy(GPTR(a), &f, 4); }
static void fill_random(uint32_t a, uint32_t n) { for (uint32_t i = 0; i < n; i++) S8(a + i, (uint8_t)rnd()); }
/* A random start for a block of n bytes in the data arena, 4-aligned mostly. */
static uint32_t place(uint32_t n) {
    uint32_t a = DATA + 0x100u + below(DATA_SIZE - n - 0x200u);
    return below(8) ? a & ~3u : a;
}

static void random_cpu(EngineCPU *c, uint32_t esp) {
    memset(c, 0, sizeof *c);
    for (unsigned i = 0; i < 8; i++) c->gpr[i] = rnd();
    c->gpr[4] = esp;
    c->flags = 0x202u | (rnd() & 0x8D5u);
    c->fp_control = 0x027F;
    c->fp_status = (uint16_t)(rnd() & 0x47FFu);
    c->fp_top = (uint8_t)below(8);
    unsigned depth = below(4);
    c->fp_valid = (uint8_t)((1u << depth) - 1u);
    for (unsigned i = 0; i < 8; i++) c->fp_reg[i] = (double)value(20);
    c->failure = on_failure;
}
static int same_cpu(const EngineCPU *a, const EngineCPU *b, char *why) {
    for (unsigned i = 0; i < 8; i++) if (a->gpr[i] != b->gpr[i]) { sprintf(why, "gpr[%u] %08X vs %08X", i, a->gpr[i], b->gpr[i]); return 0; }
    if (a->flags != b->flags) { sprintf(why, "flags %08X vs %08X", a->flags, b->flags); return 0; }
    if (a->pc != b->pc) { sprintf(why, "pc %08X vs %08X", a->pc, b->pc); return 0; }
    if (a->fp_valid != b->fp_valid || a->fp_top != b->fp_top || a->fp_status != b->fp_status || a->fp_control != b->fp_control) {
        sprintf(why, "x87 valid %02X/%02X top %u/%u status %04X/%04X control %04X/%04X", a->fp_valid, b->fp_valid,
                a->fp_top, b->fp_top, a->fp_status, b->fp_status, a->fp_control, b->fp_control); return 0; }
    if (engine_fp_tag_word((EngineCPU *)a) != engine_fp_tag_word((EngineCPU *)b)) { sprintf(why, "x87 tag word"); return 0; }
    for (unsigned i = 0; i < 8; i++) {
        unsigned slot = engine_fp_physical(a, i);
        if (memcmp(&a->fp_reg[slot], &b->fp_reg[slot], sizeof(double))) {
            sprintf(why, "st(%u)%s %a vs %a", i, (a->fp_valid & (1u << i)) ? "" : " (empty)",
                    a->fp_reg[slot], b->fp_reg[slot]); return 0;
        }
    }
    return 1;
}

/* ---- one comparison ---- */
typedef void (*Translated)(EngineCPU *);
typedef int (*Native)(EngineCPU *);
typedef struct { const char *name; uint32_t entry; Translated translated; Native native; unsigned cases, native_runs, declined, native_top_mask; } Leaf;
static unsigned failures;
/* Runs the original, then the native version from the same start, and
 * compares. reference_may_fail: the original is expected to stop on an x87
 * overflow, which only a declining native version may match. */
static void compare(Leaf *leaf, const EngineCPU *start, int expect_decline) {
    char why[160];
    leaf->cases++;
    save(before);
    EngineCPU t = *start; int reference_failed = 0;
    if (setjmp(failed) == 0) { t.pc = leaf->entry; leaf->translated(&t); }
    else reference_failed = 1;
    save(reference);
    load(before);
    EngineCPU n = *start;
    int handled = leaf->native(&n);
    if (!handled) {
        leaf->declined++;
        if (!expect_decline) { printf("FAIL %s case %u: declined an ordinary case\n", leaf->name, leaf->cases); failures++; }
        uint32_t d = differs(before);
        if (d || !same_cpu(&n, start, why)) {
            printf("FAIL %s: declined but changed %s\n", leaf->name, d ? "memory" : why); failures++;
        }
        load(reference);
        return;
    }
    leaf->native_runs++;
    leaf->native_top_mask |= 1u << start->fp_top;
    if (expect_decline) { printf("FAIL %s: ran a case it must decline\n", leaf->name); failures++; }
    if (reference_failed) { printf("FAIL %s: the original stopped (%s) but the native version ran\n", leaf->name, failure_reason); failures++; return; }
    uint32_t d = differs(reference);
    if (d) {
        printf("FAIL %s case %u: guest byte %08X differs (native %02X, original %02X)\n", leaf->name, leaf->cases, d,
               engine_flat_base[d], snapshot_byte); failures++;
    } else if (!same_cpu(&n, &t, why)) { printf("FAIL %s case %u: %s\n", leaf->name, leaf->cases, why); failures++; }
    if (failures > 20) exit(1);
}
#define RET_ADDRESS 0x004D7098u
static uint32_t stack_top(void) {   /* esp at entry; the return address is at [esp] */
    uint32_t e = STACK + 0x8000u + 4u * below(0x100);
    S32(e, RET_ADDRESS);
    return e;
}

/* ---- 004CC0D0 ---- */
static void matrix_case(Leaf *leaf) {
    uint32_t e = stack_top(), a = place(52), b = place(52), out = place(52);
    while (leaf_overlap(a, 52, b, 52)) b = place(52);
    while (leaf_overlap(out, 52, a, 52) || leaf_overlap(out, 52, b, 52)) out = place(52);
    unsigned mode = below(10);
    int decline = 0;
    switch (mode) {
    case 1: out = a; break;
    case 2: out = b; break;
    case 3: b = a; out = a; break;
    case 4: b = a; break;
    case 5: out = a + 4u * (1u + below(12)); decline = 1; break;      /* partial overlap */
    case 6: out = e - 0x20u - 4u * below(4); decline = 1; break;       /* out in the frame */
    case 7: a = e - 0x40u; decline = 1; break;                         /* an input in the frame */
    default: break;
    }
    unsigned awk = below(3) ? 3 : 40;
    for (unsigned i = 0; i < 13; i++) { put_float(a + 4u * i, value(awk)); if (b != a) put_float(b + 4u * i, value(awk)); }
    if (mode == 5 || mode == 6) for (unsigned i = 0; i < 13; i++) put_float(out + 4u * i, value(awk));
    if (mode == 7) for (unsigned i = 0; i < 13; i++) put_float(a + 4u * i, value(awk));
    S32(e + 4u, a); S32(e + 8u, b); S32(e + 12u, out);
    EngineCPU c; random_cpu(&c, e);
    if (below(20) == 0) { c.fp_control = 0x0E7F; decline = 1; }           /* rounding toward zero */
    if (below(30) == 0) { c.fp_valid = 0x7F; decline = 1; }               /* the second push would overflow */
    if (below(30) == 0) { c.flags |= 0x400u; decline = 1; }              /* direction flag set */
    compare(leaf, &c, decline);
}

/* ---- 00553380 ---- */
static void node_bounds_case(Leaf *leaf) {
    uint32_t e = stack_top(), src = place(24), bytes = place(6), out = place(24);
    while (leaf_overlap(out, 24, src, 24) || leaf_overlap(out, 24, bytes, 6)) out = place(24);
    int decline = 0;
    unsigned mode = below(12);
    if (mode == 1) { out = src; decline = 1; }
    if (mode == 2) { bytes = out + below(20); decline = 1; }
    if (mode == 3) { out = e - 8u; decline = 1; }
    if (mode == 4) { src = e - 12u; decline = 1; }
    put_float(0x00672AD4u, below(4) ? 1.0f / 254.0f : value(30));
    unsigned awk = below(3) ? 3 : 40;
    for (unsigned i = 0; i < 6; i++) put_float(src + 4u * i, value(awk));
    for (unsigned i = 0; i < 6; i++) S8(bytes + i, below(3) == 0 ? 0xFF : (uint8_t)rnd());
    EngineCPU c; random_cpu(&c, e);
    c.gpr[1] = src; c.gpr[2] = bytes; c.gpr[6] = out;
    if (below(20) == 0) { c.fp_control = 0x067F; decline = 1; }
    if (below(30) == 0) { c.fp_valid = 0x3F; decline = 1; }
    compare(leaf, &c, decline);
}

/* ---- 00554260 ---- */
static void box(float *b, unsigned awk) {
    for (unsigned axis = 0; axis < 3; axis++) {
        float centre = value(awk), half = below(10) ? fabsf(uniform(0.0f, 50.0f)) : uniform(-5.0f, 5.0f);
        b[2 * axis] = centre - half; b[2 * axis + 1] = centre + half;
        if (below(40) == 0) b[2 * axis] = awkward();
        if (below(40) == 0) b[2 * axis + 1] = awkward();
    }
}
static void planes_case(Leaf *leaf) {
    uint32_t e = stack_top();
    int32_t n = below(10) ? (int32_t)below(9) : (below(2) ? -(int32_t)below(4) - 1 : 9 + (int32_t)below(40));
    uint32_t bounds = place(24), planes = place(16u * 64u);
    int decline = 0;
    unsigned mode = below(14);
    if (mode == 1) { bounds = e - 0x20u; decline = 1; }
    if (mode == 2 && n > 0) { planes = e - 0x10u - 16u * (uint32_t)n; decline = 1; }
    float b[6]; box(b, below(4) ? 2 : 30);
    for (unsigned i = 0; i < 6; i++) put_float(bounds + 4u * i, b[i]);
    put_float(0x00672AC0u, below(6) ? 0.0f : (below(2) ? -0.0f : value(20)));
    for (int32_t i = 0; i < (n > 0 ? n : 0); i++) {
        float nx = uniform(-1, 1), ny = uniform(-1, 1), nz = uniform(-1, 1);
        if (below(4) == 0) { nx = ny = nz = 0; if (below(3) == 0) nx = 1; else if (below(2)) ny = -1; else nz = 1; }
        float cx = (b[0] + b[1]) / 2, cy = (b[2] + b[3]) / 2, cz = (b[4] + b[5]) / 2;
        float r = fabsf(nx) * (b[1] - b[0]) / 2 + fabsf(ny) * (b[3] - b[2]) / 2 + fabsf(nz) * (b[5] - b[4]) / 2;
        float d = nx * cx + ny * cy + nz * cz + r * uniform(-2.5f, 2.5f);
        if (below(8) == 0) d = nx * cx + ny * cy + nz * cz;             /* through the centre */
        float p[4] = {nx, ny, nz, d};
        for (unsigned j = 0; j < 4; j++) put_float(planes + 16u * (uint32_t)i + 4u * j, below(60) ? p[j] : awkward());
    }
    EngineCPU c; random_cpu(&c, e);
    c.gpr[0] = bounds; c.gpr[3] = planes; c.gpr[7] = (rnd() & 0xFFFF0000u) | ((uint32_t)n & 0xFFFFu);
    if (below(20) == 0) { c.fp_control = 0x0A7F; decline = 1; }
    if (below(30) == 0) { c.fp_valid = 0x1F; decline = n > 0 ? 1 : decline; if (n <= 0) c.fp_valid = 0x07; }
    compare(leaf, &c, decline);
}

/* ---- 005541B0 ---- */
static void overlap_case(Leaf *leaf) {
    uint32_t e = stack_top(), a = place(24), b = place(24);
    float A[6], B[6]; box(A, below(4) ? 1 : 30);
    unsigned mode = below(6);
    for (unsigned axis = 0; axis < 3; axis++) {
        float lo = A[2 * axis], hi = A[2 * axis + 1], w = hi - lo;
        switch (mode) {
        case 0: B[2 * axis] = lo + w * uniform(0.1f, 0.4f); B[2 * axis + 1] = hi - w * uniform(0.1f, 0.4f); break;  /* inside */
        case 1: B[2 * axis] = lo - w * uniform(0, 1); B[2 * axis + 1] = hi + w * uniform(0, 1); break;              /* around */
        case 2: B[2 * axis] = hi + uniform(0, 5); B[2 * axis + 1] = B[2 * axis] + uniform(0, 5); break;             /* beyond */
        case 3: B[2 * axis] = below(2) ? lo : hi; B[2 * axis + 1] = below(2) ? hi : lo; break;                      /* touching */
        default: B[2 * axis] = value(5); B[2 * axis + 1] = B[2 * axis] + uniform(-10, 60); break;
        }
        if (below(50) == 0) B[2 * axis + below(2)] = awkward();
    }
    for (unsigned i = 0; i < 6; i++) { put_float(a + 4u * i, A[i]); put_float(b + 4u * i, B[i]); }
    EngineCPU c; random_cpu(&c, e);
    c.gpr[1] = a; c.gpr[2] = b;
    int decline = 0;
    if (below(20) == 0) c.fp_control = 0x0E7F;           /* compares do not round: still native */
    if (below(30) == 0) { c.fp_valid = 0xFF; decline = 1; }
    compare(leaf, &c, decline);
}

/* ---- 00552C20 ---- */
static uint32_t visible_count(uint32_t bitset, int32_t count) {
    uint32_t found = 0;
    for (uint32_t w = 0; count > 0 && w < ((uint32_t)count + 31u) / 32u; w++) {
        uint32_t v = G32(bitset + 4u * w), left = (uint32_t)count - 32u * w;
        if (left < 32u) v &= (1u << left) - 1u;
        found += (uint32_t)__builtin_popcount(v);
    }
    return found;
}
static void surface_list_case(Leaf *leaf) {
    uint32_t e = stack_top();
    static const int32_t fixed[] = {0, -3, 1, 31, 32, 33, 63, 64, 65, 96};
    int32_t count = below(3) ? (int32_t)below(3000) + 1 : fixed[below(10)];
    uint32_t words = count > 0 ? ((uint32_t)count + 31u) / 32u : 1u;
    /* Every block sized for the largest case, and inputs and outputs apart
     * unless the case is about overlap. */
    const uint32_t list_bytes = 4u * 3000u, index_bytes = 6u * 3000u, tri_bytes = 6u * 3000u + 8u;
    uint32_t bsp, bitset, triangles, list, indices;
    for (;;) {
        bsp = place(0x100); bitset = place(list_bytes); triangles = place(tri_bytes); list = place(list_bytes); indices = place(index_bytes);
        const uint32_t at[5] = {bsp, bitset, triangles, list, indices}, size[5] = {0x100, list_bytes, tri_bytes, list_bytes, index_bytes};
        int apart = 1;
        for (unsigned i = 0; i < 5; i++) for (unsigned j = i + 1; j < 5; j++) if (leaf_overlap(at[i], size[i], at[j], size[j])) apart = 0;
        if (apart) break;
    }
    fill_random(bsp, 0x100);
    fill_random(triangles, tri_bytes);
    unsigned density = below(5);
    for (uint32_t w = 0; w < words; w++) {
        uint32_t v = density == 0 ? 0 : density == 1 ? rnd() & rnd() & rnd() : density == 2 ? 0xFFFFFFFFu : rnd();
        if (below(4) == 0) v = 0;
        S32(bitset + 4u * w, v);
    }
    S32(0x00746F9Cu, bsp);
    S32(bsp + 0xF8u, (uint32_t)count);
    S32(bsp + 0xFCu, triangles);
    uint32_t found = visible_count(bitset, count);
    int decline = 0;
    unsigned mode = below(16);
    if (mode == 1 && found) { list = bitset; decline = 1; }
    if (mode == 2 && found) { indices = triangles + 2u; decline = 1; }
    if (mode == 3 && found) { indices = e - 0x14u - 6u * found + 2u; decline = 1; }   /* the last entry reaches the saved esi */
    S32(e + 4u, list); S32(e + 8u, bitset); S32(e + 12u, indices);
    EngineCPU c; random_cpu(&c, e);
    compare(leaf, &c, decline);
}

/* ---- 00552DE0 ---- */
typedef struct { uint32_t list; int16_t count; uint32_t args[5]; } Walk;
static Walk build_walk(void) {
    Walk w;
    uint32_t bsp = DATA + 0x100u;               /* a fixed layout keeps every structure inside the arena */
    uint32_t lightmaps = DATA + 0x400u, materials = DATA + 0x1000u, tags = DATA + 0x20000u;
    uint32_t shaders = DATA + 0x21000u, bitmaps = DATA + 0x22000u, breakable = DATA + 0x23000u;
    uint32_t list = DATA + 0x30000u;
    fill_random(DATA, 0x100u);
    fill_random(bsp, 0x200u);
    S32(0x00746F9Cu, bsp);
    S32(0x0087BC14u, tags);
    S16(0x0069E8D8u, (uint16_t)below(3));
    S32(0x006B8D78u, breakable);
    fill_random(breakable, 0x400u);
    int32_t lightmap_count = below(12) ? (int32_t)below(7) : -(int32_t)below(3);
    S32(bsp + 0x104u, (uint32_t)lightmap_count);
    S32(bsp + 0x108u, lightmaps);
    S32(bsp + 0x0Cu, below(3) ? below(4) : 0xFFFFFFFFu);
    /* Tags: 0..3 bitmaps (some without data), 4..19 shaders of random types. */
    for (uint32_t t = 0; t < 20; t++) {
        uint32_t data = t < 4 ? (below(4) ? bitmaps + 0x100u * t : 0u) : shaders + 0x40u * (t - 4u);
        fill_random(tags + 32u * t, 32u);
        S32(tags + 32u * t + 0x14u, data);
        if (t < 4 && data) { S32(data + 0x60u, below(6)); S32(data + 0x64u, DATA + 0x24000u + 0x400u * t); }
        if (t >= 4) { fill_random(data, 0x40u); S16(data + 0x24u, (uint16_t)(int16_t)((int32_t)below(17) - 2)); }
    }
    int32_t cursor = 0;
    uint32_t next_material = materials;
    for (int32_t l = 0; l < 7; l++) {
        uint32_t lm = lightmaps + 32u * (uint32_t)l;
        fill_random(lm, 32u);
        S16(lm, (uint16_t)(int16_t)((int32_t)below(6) - 1));
        int32_t count = below(8) ? (int32_t)below(6) : 0;
        S32(lm + 0x14u, (uint32_t)count);
        next_material += 0x100u;                 /* the original reads the entry before an empty array */
        S32(lm + 0x18u, next_material);
        for (int32_t m = 0; m < count; m++) {
            uint32_t mat = next_material + 0x100u * (uint32_t)m;
            fill_random(mat, 0x100u);
            S32(mat + 0x0Cu, (rnd() & 0xFFFF0000u) | (4u + below(16)));
            int32_t n = below(6) ? (int32_t)below(20) : 0;
            if (below(10) == 0) cursor -= (int32_t)below(10);   /* overlapping or out-of-order ranges */
            S32(mat + 0x18u, (uint32_t)cursor);
            S32(mat + 0x14u, (uint32_t)n);
            cursor += n + (below(6) == 0 ? (int32_t)below(5) : 0);
            S16(mat + 0xACu, below(3) ? 0xFFFFu : (uint16_t)(int16_t)((int32_t)below(120) - 8));
        }
        next_material += 0x100u * (uint32_t)(count > 0 ? count : 0);
    }
    /* The surface list: ascending mostly, with strays. */
    uint32_t n = below(8) ? below(200) : 0;
    int32_t v = (int32_t)below(4) - 2;
    for (uint32_t i = 0; i < n; i++) {
        v += below(3) == 0 ? (int32_t)below(6) : 1;
        S32(list + 4u * i, below(80) ? (uint32_t)v : rnd());
    }
    w.list = list; w.count = (int16_t)n;
    w.args[0] = rnd(); w.args[1] = 0; w.args[2] = below(8) ? 0x0044AD80u : 0; w.args[3] = 0; w.args[4] = 0;
    return w;
}
static void walk_case(Leaf *leaf) {
    uint32_t e = STACK + 0x8000u + 4u * below(0x100);
    Walk w = build_walk();
    S32(e, 0x0050C528u);
    for (unsigned i = 0; i < 5; i++) S32(e + 4u + 4u * i, w.args[i]);
    fill_random(e - 0x60u, 0x60u);   /* whatever the stack held before */
    EngineCPU c; random_cpu(&c, e);
    c.gpr[0] = (rnd() & 0xFFFF0000u) | (uint16_t)w.count;
    c.gpr[1] = w.list;
    int decline = 0;
    if (below(25) == 0 && w.count > 4) {   /* the list inside the frame: declined once the walk reads it */
        c.gpr[1] = e - 0x40u;
        decline = (int32_t)G32(DATA + 0x100u + 0x104u) > 0;
    }
    compare(leaf, &c, decline);
}
/* Callbacks other than 0x44AD80 run the original; the native version must decline. */
static void walk_decline_case(Leaf *leaf) {
    uint32_t e = STACK + 0x8000u;
    Walk w = build_walk();
    S32(e, 0x0050C300u);
    w.args[1 + below(4)] = 0x00512020u;
    for (unsigned i = 0; i < 5; i++) S32(e + 4u + 4u * i, w.args[i]);
    EngineCPU c; random_cpu(&c, e);
    c.gpr[0] = (uint16_t)w.count; c.gpr[1] = w.list;
    save(before);
    EngineCPU n = c;
    char why[160];
    if (leaf_bsp_walk_dead(&n) || differs(before) || !same_cpu(&n, &c, why)) { printf("FAIL %s: ran with a live callback\n", leaf->name); failures++; }
}

/* ---- timing ---- */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
/* Through a pointer, as engine_dispatch reaches it, so nothing is inlined into the loop. */
static int (*volatile native_call)(EngineCPU *);
static void empty(EngineCPU *c) { c->gpr[4] += 4u; }
static void (*volatile empty_call)(EngineCPU *) = empty;
static void (*volatile translated_call)(EngineCPU *);
static void time_leaf(const char *name, uint32_t entry, Translated t, Native n, const EngineCPU *start, uint32_t restore_at, uint32_t restore, unsigned iterations) {
    EngineCPU c;
    uint32_t ret = G32(start->gpr[4]);
    translated_call = t; native_call = n;
    double t0 = now();
    for (unsigned i = 0; i < iterations; i++) {
        c = *start; S32(start->gpr[4], ret); if (restore_at) S32(restore_at, restore);
        c.pc = entry; translated_call(&c);
    }
    double t1 = now();
    for (unsigned i = 0; i < iterations; i++) {
        c = *start; S32(start->gpr[4], ret); if (restore_at) S32(restore_at, restore);
        if (!native_call(&c)) { printf("FAIL %s: timing input declined\n", name); failures++; return; }
    }
    double t2 = now();
    for (unsigned i = 0; i < iterations; i++) {   /* the loop alone: state copy, stores, the call */
        c = *start; S32(start->gpr[4], ret); if (restore_at) S32(restore_at, restore);
        c.pc = entry; empty_call(&c);
    }
    double t3 = now();
    double loop = (t3 - t2) / iterations * 1e9, tn = (t1 - t0) / iterations * 1e9 - loop, nn = (t2 - t1) / iterations * 1e9 - loop;
    printf("timing %-28s translated %9.1f ns/call  native %8.1f ns/call  (%.0fx; loop %.1f ns removed)\n", name, tn, nn, tn / nn, loop);
}
static void timings(void) {
    EngineCPU c;
    /* 004CC0D0 on ordinary matrices, out distinct. */
    uint32_t e = STACK + 0x8000u, a = DATA + 0x1000u, b = DATA + 0x1100u, out = DATA + 0x1200u;
    for (unsigned i = 0; i < 13; i++) { put_float(a + 4u * i, uniform(-1, 1)); put_float(b + 4u * i, uniform(-1, 1)); }
    S32(e, RET_ADDRESS); S32(e + 4u, a); S32(e + 8u, b); S32(e + 12u, out);
    random_cpu(&c, e); c.fp_valid = 0; c.flags = 0x202;
    time_leaf("004CC0D0 matrix multiply", 0x004CC0D0u, sub_004CC0D0, leaf_matrix4x3_multiply, &c, 0, 0, 2000000);
    /* 00553380 with a mix of bytes. */
    uint32_t src = DATA + 0x1300u, bytes = DATA + 0x1400u;
    for (unsigned i = 0; i < 6; i++) { put_float(src + 4u * i, uniform(-100, 100)); S8(bytes + i, (uint8_t)(i == 3 ? 0xFF : rnd() % 255)); }
    put_float(0x00672AD4u, 1.0f / 254.0f);
    random_cpu(&c, e); c.fp_valid = 0; c.gpr[1] = src; c.gpr[2] = bytes; c.gpr[6] = out;
    time_leaf("00553380 node bounds", 0x00553380u, sub_00553380, leaf_bsp_node_bounds, &c, 0, 0, 2000000);
    /* 00554260 against six frustum-like planes, straddling. */
    uint32_t bounds = DATA + 0x1500u, planes = DATA + 0x1600u;
    float bx[6] = {-10, 10, -10, 10, -10, 10};
    for (unsigned i = 0; i < 6; i++) put_float(bounds + 4u * i, bx[i]);
    for (unsigned i = 0; i < 6; i++) {
        float p[4] = {i / 2 == 0 ? (i & 1 ? -1.f : 1.f) : 0.f, i / 2 == 1 ? (i & 1 ? -1.f : 1.f) : 0.f, i / 2 == 2 ? (i & 1 ? -1.f : 1.f) : 0.f, -5.f};
        for (unsigned j = 0; j < 4; j++) put_float(planes + 16u * i + 4u * j, p[j]);
    }
    put_float(0x00672AC0u, 0.0f);
    random_cpu(&c, e); c.fp_valid = 0; c.gpr[0] = bounds; c.gpr[3] = planes; c.gpr[7] = 6;
    time_leaf("00554260 bounds vs 6 planes", 0x00554260u, sub_00554260, leaf_bounds_planes, &c, 0, 0, 1000000);
    /* 005541B0 on overlapping boxes (all twelve compares until the last). */
    uint32_t ba = DATA + 0x1700u, bb = DATA + 0x1800u;
    float by[6] = {-5, 5, -5, 5, -5, 5};
    for (unsigned i = 0; i < 6; i++) { put_float(ba + 4u * i, bx[i]); put_float(bb + 4u * i, by[i]); }
    random_cpu(&c, e); c.fp_valid = 0; c.gpr[1] = ba; c.gpr[2] = bb;
    time_leaf("005541B0 box overlap", 0x005541B0u, sub_005541B0, leaf_bounds_overlap, &c, 0, 0, 2000000);
    /* 00552C20 over 8000 surfaces with a quarter visible, in runs. */
    uint32_t bsp = DATA + 0x2000u, bitset = DATA + 0x2100u, tris = DATA + 0x3000u, list = DATA + 0x10000u, idx = DATA + 0x20000u;
    S32(0x00746F9Cu, bsp); S32(bsp + 0xF8u, 8000u); S32(bsp + 0xFCu, tris);
    for (uint32_t w = 0; w < 250; w++) S32(bitset + 4u * w, (w % 4 == 0) ? rnd() : (w % 4 == 1 ? 0xFFFF0000u : 0));
    random_cpu(&c, e); c.fp_valid = 0; S32(e + 4u, list); S32(e + 8u, bitset); S32(e + 12u, idx);
    time_leaf("00552C20 surface list (8000)", 0x00552C20u, sub_00552C20, leaf_surface_list, &c, e + 8u, bitset, 3000);
    /* 00552DE0 dead walk over a built BSP. */
    Walk w;
    do w = build_walk(); while (w.count < 150 || (int32_t)G32(DATA + 0x100u + 0x104u) < 5 || w.args[2] != 0x0044AD80u);
    S32(e, 0x0050C528u); for (unsigned i = 0; i < 5; i++) S32(e + 4u + 4u * i, w.args[i]);
    random_cpu(&c, e); c.fp_valid = 0; c.gpr[0] = (uint16_t)w.count; c.gpr[1] = w.list;
    time_leaf("00552DE0 walk, ret callback", 0x00552DE0u, sub_00552DE0, leaf_bsp_walk_dead, &c, 0, 0, 50000);
}
#endif

int main(void) {
#if !HAVE_ENGINE
    puts("SKIP: the generated engine (sub_*.c, engine_functions.h) is not on the include path");
    return 0;
#else
    void *space = mmap(NULL, UINT64_C(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(space != MAP_FAILED);
    engine_flat_base = space;
    for (size_t i = 0; i < ARENAS; i++) {
        assert(!mprotect(engine_flat_base + arenas[i].base, arenas[i].size, PROT_READ | PROT_WRITE));
        arena_total += arenas[i].size;
    }
    before = malloc(arena_total); reference = malloc(arena_total);
    assert(before && reference);
    for (size_t i = 0; i < ARENAS; i++) fill_random(arenas[i].base, arenas[i].size);
    Leaf leaves[] = {
        {"004CC0D0 matrix multiply", 0x004CC0D0u, sub_004CC0D0, leaf_matrix4x3_multiply},
        {"00553380 node bounds", 0x00553380u, sub_00553380, leaf_bsp_node_bounds},
        {"00554260 bounds vs planes", 0x00554260u, sub_00554260, leaf_bounds_planes},
        {"005541B0 box overlap", 0x005541B0u, sub_005541B0, leaf_bounds_overlap},
        {"00552C20 surface list", 0x00552C20u, sub_00552C20, leaf_surface_list},
        {"00552DE0 dead walk", 0x00552DE0u, sub_00552DE0, leaf_bsp_walk_dead},
    };
    for (unsigned i = 0; i < 12000; i++) matrix_case(&leaves[0]);
    for (unsigned i = 0; i < 12000; i++) node_bounds_case(&leaves[1]);
    for (unsigned i = 0; i < 12000; i++) planes_case(&leaves[2]);
    for (unsigned i = 0; i < 12000; i++) overlap_case(&leaves[3]);
    for (unsigned i = 0; i < 3000; i++) surface_list_case(&leaves[4]);
    for (unsigned i = 0; i < 6000; i++) walk_case(&leaves[5]);
    for (unsigned i = 0; i < 300; i++) walk_decline_case(&leaves[5]);
    for (unsigned i = 0; i < sizeof leaves / sizeof *leaves; i++)
        printf("%-28s %6u cases: %6u native, identical; %5u declined, untouched\n", leaves[i].name, leaves[i].cases,
               leaves[i].native_runs, leaves[i].declined);
    for (unsigned i = 0; i < sizeof leaves / sizeof *leaves; i++) {
        if (leaves[i].native_top_mask != 0xFFu) {
            printf("FAIL: %s did not compare every x87 TOP\n", leaves[i].name); return 1;
        }
    }
    puts("all six leaves cover all eight initial x87 TOP values, including every empty register");
    if (failures) { printf("FAIL: %u mismatches\n", failures); return 1; }
    timings();
    if (failures) return 1;
    puts("PASS: native leaves leave the same memory, registers, flags and x87 state as the translated functions");
    return 0;
#endif
}
