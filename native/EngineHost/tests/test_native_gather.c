/* Differential tests of the exact leaf/cluster gathers against the actual
 * translated sub_*.c files. Compare all mapped guest bytes, including dead
 * stack frames, every GPR/flags/return PC and observable x87 state. Guard pages
 * cover the rest of the 4 GiB address space. Random data includes duplicate
 * surfaces, visibility/visited masks, clipping, capacity exits, aliases, NaNs,
 * signed counts, unusual rounding modes and pre-existing x87 registers.
 */
#include "host.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#if __has_include("engine_functions.h") && __has_include("sub_005540C0.c") && __has_include("sub_00553C40.c")
#define HAVE_ENGINE 1
#include "engine_functions.h"
#include "engine_flags.h"
#include "engine_registers.h"
#include "sub_00553380.c"
#include "sub_00554260.c"
#include "sub_005541B0.c"
#include "sub_005540C0.c"
#include "sub_00553C40.c"
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
#include "native_gather.h"

uint8_t *engine_flat_base;

#if HAVE_ENGINE
static jmp_buf failed;
static const char *failure_reason;
static void on_failure(EngineCPU *cpu, const char *reason) { (void)cpu; failure_reason = reason; longjmp(failed, 1); }
/* mode 0 is wholly translated; mode 1 is the existing native-leaf baseline. */
static int native_children;
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    cpu->pc = address;
    if (native_children && host_native_leaf_dispatch(cpu, address)) return;
    switch (address) {
    case 0x00553380u: sub_00553380(cpu); return;
    case 0x005541B0u: sub_005541B0(cpu); return;
    case 0x00554260u: sub_00554260(cpu); return;
    default: fprintf(stderr, "unexpected call to %08X\n", address); abort();
    }
}

/* ---- guest memory ---- */
#define DATA 0x20000000u
#define DATA_SIZE 0x40000u
#define STACK 0x30000000u
#define STACK_SIZE 0x10000u
typedef struct { uint32_t base, size; } Arena;
static const Arena arenas[] = {
    {0x00670000u, 0x4000u}, {0x0069C000u, 0x4000u}, {0x006B8000u, 0x4000u}, {0x00744000u, 0x4000u},
    {0x007D0000u, 0x4000u}, {DATA, DATA_SIZE}, {STACK, STACK_SIZE},
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
    for (unsigned i = 0; i < 8; i++) c->fp_reg[i] = (double)value(20);   /* every physical register, empty ones too */
    c->failure = on_failure;
}
static int same_cpu(const EngineCPU *a, const EngineCPU *b, char *why) {
    for (unsigned i = 0; i < 8; i++) if (a->gpr[i] != b->gpr[i]) { sprintf(why, "gpr[%u] %08X vs %08X", i, a->gpr[i], b->gpr[i]); return 0; }
    if (a->flags != b->flags) { sprintf(why, "flags %08X vs %08X", a->flags, b->flags); return 0; }
    if (a->pc != b->pc) { sprintf(why, "pc %08X vs %08X", a->pc, b->pc); return 0; }
    if (a->fp_valid != b->fp_valid || a->fp_top != b->fp_top || a->fp_status != b->fp_status || a->fp_control != b->fp_control) {
        sprintf(why, "x87 valid %02X/%02X top %u/%u status %04X/%04X control %04X/%04X", a->fp_valid, b->fp_valid,
                a->fp_top, b->fp_top, a->fp_status, b->fp_status, a->fp_control, b->fp_control); return 0; }
    for (unsigned i = 0; i < 8; i++)
        if ((a->fp_valid >> i & 1) && memcmp(&a->fp_reg[engine_fp_physical(a, i)], &b->fp_reg[engine_fp_physical(b, i)], 8)) {
            sprintf(why, "st(%u) %a vs %a", i, a->fp_reg[engine_fp_physical(a, i)], b->fp_reg[engine_fp_physical(b, i)]); return 0; }
    /* Empty registers too: the rotating stack keeps what a pop leaves behind,
     * and the native gather drives the same helpers, so every slot matches. */
    if (memcmp(a->fp_reg, b->fp_reg, sizeof a->fp_reg)) { sprintf(why, "x87 register file differs"); return 0; }
    return 1;
}

static unsigned cases, failures;
typedef void (*Translated)(EngineCPU *);
typedef int (*Native)(EngineCPU *);
#define RET_ADDRESS 0x004D7098u
#define BSP (DATA + 0x100u)
#define LEAVES (DATA + 0x1000u)
#define SURFACES (DATA + 0x2000u)
#define PARENT (DATA + 0x5000u)
#define VOLUME (DATA + 0x5100u)
#define PLANES (DATA + 0x6000u)
#define BITS (DATA + 0x8000u)
#define OUT (DATA + 0x9000u)
#define CLUSTERS (DATA + 0xA000u)
#define GROUPS (DATA + 0xC000u)
#define CLUSTER_IDS (DATA + 0xD000u)
#define CLUSTER_SURFACES (DATA + 0x10000u)

static void compare(const char *name, Translated translated, Native native, const EngineCPU *start) {
    cases++;
    save(before);
    EngineCPU t = *start;
    native_children = (int)(cases & 1);
    if (setjmp(failed)) { fprintf(stderr, "reference failure %s case %u: %s\n", name, cases, failure_reason); exit(1); }
    translated(&t);
    save(reference);
    load(before);
    EngineCPU n = *start;
    if (!native(&n)) { fprintf(stderr, "native declined ordinary %s case %u\n", name, cases); exit(1); }
    char why[160];
    uint32_t d = differs(reference);
    if (d) {
        printf("FAIL %s case %u byte %08X native %02X translated %02X\n", name, cases, d, G8(d), snapshot_byte);
        failures++;
    } else if (!same_cpu(&n, &t, why)) {
        printf("FAIL %s case %u: %s\n", name, cases, why); failures++;
    }
    if (failures > 10) exit(1);
}

static void common(EngineCPU *c, int ordinary) {
    uint32_t e = STACK + 0x8000u + below(128) * 4u;
    fill_random(e - 0x100u, 0x180u);
    S32(e, RET_ADDRESS);
    random_cpu(c, e);
    c->pc = 0x005540C0u;
    if (!ordinary && below(8) == 0) c->fp_control = (uint16_t)(0x027Fu | (below(4) << 10));
    S32(0x00746F9Cu, BSP);
    S32(BSP + 0xE4u, LEAVES); S32(BSP + 0xF0u, SURFACES);
    S32(BSP + 0x138u, CLUSTERS);
    put_float(0x00672AD4u, 1.0f / 254.0f); put_float(0x00672AC0u, 0.0f);
    for (unsigned i = 0; i < 6; i++) {
        put_float(PARENT + 4u * i, i & 1 ? 100.f : -100.f);
        put_float(VOLUME + 4u * i, ordinary ? (i & 1 ? 200.f : -200.f) : value(15));
    }
    for (unsigned i = 0; i < 32; i++) {
        S32(0x007D0394u + i * 4u, ordinary ? ~0u : rnd());
        S32(BITS + i * 4u, ordinary ? 0 : (rnd() & rnd()));
    }
    for (unsigned i = 0; i < 32; i++) put_float(PLANES + i * 4u, ordinary ? 0.f : value(10));
}

static EngineCPU leaf_case(int ordinary, unsigned variant) {
    EngineCPU c; common(&c, ordinary);
    uint32_t e = c.gpr[4], index = below(16), node = LEAVES + index * 16u;
    c.gpr[0] = index | (rnd() & 0x80000000u);
    c.gpr[1] = (rnd() & 0xFFFF0000u) | (ordinary ? 2u : below(4));
    for (unsigned i = 0; i < 6; i++) S8(node + i, (uint8_t)rnd());
    unsigned count = ordinary ? 128 : below(256);
    S16(node + 0xAu, (uint16_t)count);
    uint32_t start = below(8); S32(node + 0xCu, start);
    for (unsigned i = 0; i < count + start; i++) {
        S32(SURFACES + i * 8u, ordinary ? i : below(1024));
        S32(SURFACES + i * 8u + 4u, rnd());
    }
    uint32_t out = OUT, bits = BITS, parent = PARENT, planes = PLANES, cap = ordinary ? 256 : below(130);
    if (!ordinary) switch (variant % 14) {
    case 1: S16(node + 0xAu, 0); break;
    case 2: S16(node + 0xAu, 0xFFFFu); break;
    case 3: cap = 0xFFFFu; break;
    case 4: cap = 0x7FFFu; break;
    case 5: out = SURFACES; break;
    case 6: out = BITS; break;
    case 7: out = 0x007D0394u; break;
    case 8: bits = 0x007D0394u; break;
    case 9: out = e - 0x2Cu; cap = 1; break;
    case 10: out = PARENT; break;
    case 11: c.gpr[1] = 0xFFFFu; break;
    case 12: parent = e - 0x18u; break; /* bounds decode must use translated overlap path */
    case 13: planes = e - 0x60u; c.gpr[1] = 1; break; /* bounds-plane scratch alias */
    }
    S32(e + 4u, parent); S32(e + 8u, bits); S32(e + 12u, out); S32(e + 16u, cap);
    S32(e + 20u, VOLUME); S32(e + 24u, ordinary ? 0 : below(9)); S32(e + 28u, planes);
    return c;
}

static EngineCPU clusters_case(int ordinary, unsigned variant) {
    EngineCPU c; common(&c, ordinary);
    c.pc = 0x00553C40u;
    uint32_t e = c.gpr[4], count = ordinary ? 4 : below(7), cap = ordinary ? 1024 : below(120);
    uint32_t out = OUT, bits = BITS;
    S32(e + 12u, VOLUME); S32(e + 16u, ordinary ? 0 : below(9)); S32(e + 20u, PLANES);
    for (unsigned i = 0; i < count; i++) {
        S16(CLUSTER_IDS + 2u * i, (uint16_t)i);
        uint32_t cluster = CLUSTERS + 0x68u * i, groups = GROUPS + 0x100u * i;
        unsigned group_count = ordinary ? 4 : below(6);
        S32(cluster + 0x34u, group_count); S32(cluster + 0x38u, groups);
        for (unsigned j = 0; j < group_count; j++) {
            uint32_t group = groups + 36u * j, surfaces = CLUSTER_SURFACES + 0x2000u * i + 0x400u * j;
            for (unsigned k = 0; k < 6; k++) put_float(group + k * 4u, ordinary ? (k & 1 ? 1.f : -1.f) : value(10));
            unsigned n = ordinary ? 64 : below(100);
            S32(group + 24u, n); S32(group + 28u, surfaces);
            for (unsigned k = 0; k < n; k++) S32(surfaces + 4u * k, ordinary ? k + j * 64u + i * 256u : below(1024));
        }
    }
    if (!ordinary) switch (variant % 12) {
    case 1: count = 0; break;
    case 2: count = 0xFFFFu; break;
    case 3: cap = 0xFFFFu; break;
    case 4: out = CLUSTER_SURFACES; break;
    case 5: out = BITS; break;
    case 6: out = 0x007D0394u; break;
    case 7: bits = 0x007D0394u; break;
    case 8: out = e - 0x20u; cap = 1; break;
    case 9: out = VOLUME; cap = 3; break;
    case 10: S32(e + 20u, e - 0x58u); break; /* bounds-plane scratch alias */
    case 11: S32(e + 12u, e - 0x58u); break;
    }
    S32(e + 4u, out); S32(e + 8u, cap); S32(e + 24u, bits); S32(e + 28u, count); S32(e + 32u, CLUSTER_IDS);
    return c;
}

static void decline_cases(void) {
    unsigned declined = 0;
    for (unsigned k = 0; k < 2; k++) for (unsigned i = 0; i < 160; i++) {
        EngineCPU c = k ? clusters_case(1, 0) : leaf_case(1, 0);
        Native native = k ? gather_native_clusters : gather_native_leaf;
        if (i < 96) c.fp_valid = (uint8_t)(0x10u | rnd());
        else if (i < 128) c.gpr[4] = i & 1 ? 0xFFFFFFF0u : 0x10u;
        else { c.instruction_limit = 1u + rnd(); c.instruction_count = below(2) ? 0 : c.instruction_limit; }
        EngineCPU n = c;
        save(before);
        if (native(&n) || memcmp(&n, &c, sizeof c) || differs(before)) {
            fprintf(stderr, "FAIL: gather declined case changed state\n"); exit(1);
        }
        declined++;
    }
    printf("PASS: %u x87-capacity/stack-range/instruction-limit fallbacks decline without changing memory or any CPU byte\n", declined);
}

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static volatile uint32_t checksum;
static void timing(const char *name, Translated translated, Native native, EngineCPU c, unsigned iterations) {
    uint32_t e = c.gpr[4];
    double elapsed[3];
    for (unsigned mode = 0; mode < 3; mode++) {
        native_children = mode > 0;
        double begin = now();
        for (unsigned i = 0; i < iterations; i++) {
            EngineCPU run = c;
            memset(GPTR(BITS), 0, 128u);
            S32(e, RET_ADDRESS);
            if (mode == 2) { if (!native(&run)) abort(); } else translated(&run);
            checksum += run.gpr[0];
        }
        elapsed[mode] = (now() - begin) * 1e9 / iterations;
    }
    printf("timing %s: translated %.1f ns; existing native leaves %.1f ns; native gather %.1f ns; %.2fx vs existing\n",
           name, elapsed[0], elapsed[1], elapsed[2], elapsed[1] / elapsed[2]);
}
#endif

int main(void) {
#if !HAVE_ENGINE
    puts("SKIP: generated gather source is not on include path"); return 0;
#else
    void *space = mmap(NULL, UINT64_C(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(space != MAP_FAILED); engine_flat_base = space;
    for (size_t i = 0; i < ARENAS; i++) {
        assert(!mprotect(engine_flat_base + arenas[i].base, arenas[i].size, PROT_READ | PROT_WRITE));
        arena_total += arenas[i].size;
    }
    before = malloc(arena_total); reference = malloc(arena_total); assert(before && reference);
    for (size_t i = 0; i < ARENAS; i++) fill_random(arenas[i].base, arenas[i].size);
    for (unsigned i = 0; i < 12000; i++) {
        EngineCPU c = leaf_case(0, i); compare("005540C0", sub_005540C0, gather_native_leaf, &c);
        c = clusters_case(0, i); compare("00553C40", sub_00553C40, gather_native_clusters, &c);
    }
    if (failures) { printf("FAIL: %u mismatches\n", failures); return 1; }
    printf("PASS: %u randomized gathers; all mapped guest bytes, GPRs, flags, return PC and observable x87 identical\n", cases);
    decline_cases();
    EngineCPU c = leaf_case(1, 0); timing("005540C0 / 128 surfaces", sub_005540C0, gather_native_leaf, c, 100000);
    c = clusters_case(1, 0); timing("00553C40 / 1024 surfaces", sub_00553C40, gather_native_clusters, c, 20000);
    return 0;
#endif
}
