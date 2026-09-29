/* Real translated leaves, frozen shifting layout versus rotating layout.
 * Every writable guest byte is checked after EACH call; unmapped guest
 * addresses are protected. This is host evidence, not headset frame timing. */
#include "x87_translated.h"
#include <assert.h>
#include <fenv.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

uint8_t *engine_flat_base;
static uint8_t *shifting_memory, *rotating_memory;
static const char *names[] = { "sub_0050D5B0", "sub_004CC0D0", "sub_00554260", "sub_00553380" };
typedef struct { uint32_t address, size; } Region;
/* 16 KiB aligned for both Apple arm64 and ordinary 4 KiB hosts. The entire
 * rest of each reserved 4 GiB guest arena remains PROT_NONE. */
static const Region regions[] = {
    { X87T_STACK - 0x4000, 0x4000 }, { 0x670000, 0x4000 },
    { X87T_OBJECT(0), 0x10000 }, { X87T_BOX(0), 0x4000 },
    { X87T_MATRIX(0), 0x4000 }, { X87T_OUTPUT(0), 0x4000 },
    { X87T_PLANES(0), 0x4000 }, { X87T_BYTES(0), 0x4000 }
};
static uint64_t rng = UINT64_C(0xD1B54A32D192ED03);
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void write_f32(uint8_t *m, uint32_t address, float v) { memcpy(m + address, &v, sizeof v); }
static float input_float(float lo, float hi, int awkward) {
    if (awkward && next() % 13 == 0) {
        static const uint32_t bits[] = { 0, 0x80000000, 0x7F800000, 0xFF800000,
            0x7FC12345, 0xFFC54321, 0x7F812345, 1, 0x80000001, 0x7F7FFFFF };
        uint32_t b = bits[next() % (sizeof bits / sizeof *bits)]; float v; memcpy(&v, &b, 4); return v;
    }
    return lo + (hi - lo) * (float)(next() & 0xFFFFFF) / 16777216.0f;
}
static void fill_inputs(uint8_t *m, int awkward) {
    for (unsigned r = 0; r < sizeof regions / sizeof *regions; r++)
        memset(m + regions[r].address, 0xA5, regions[r].size);
    write_f32(m, 0x672AC0, 0.0f); write_f32(m, 0x672AD4, 1.0f / 255.0f);
    for (unsigned k = 0; k < X87T_RECORDS; k++) {
        for (unsigned i = 0; i < 6; i++) {
            write_f32(m, X87T_BOX(k) + 4 * i, input_float(i & 1 ? 0.5f : -2.0f, i & 1 ? 2.0f : 0.4f, awkward));
            write_f32(m, X87T_OBJECT(k) + 0x128 + 4 * i,
                      input_float(i & 1 ? 0.2f : -2.5f, i & 1 ? 2.5f : 0.6f, awkward));
        }
        for (unsigned i = 0; i < 16; i++) write_f32(m, X87T_OBJECT(k) + 0x78 + 4 * i, input_float(-1, 1, awkward));
        for (unsigned i = 0; i < 15; i++) write_f32(m, X87T_OBJECT(k) + 0xE0 + 4 * i, input_float(-2, 2, awkward));
        for (unsigned i = 0; i < 13; i++) write_f32(m, X87T_MATRIX(k) + 4 * i, input_float(-1, 1, awkward));
        for (unsigned i = 0; i < 24; i++) write_f32(m, X87T_PLANES(k) + 4 * i,
            i % 4 == 3 ? input_float(-3, 0.5f, awkward) : input_float(-1, 1, awkward));
        for (unsigned i = 0; i < 6; i++) m[X87T_BYTES(k) + i] = next() % 5 ? (uint8_t)next() : 0xFF;
    }
}
static void clone_memory(void) {
    for (unsigned r = 0; r < sizeof regions / sizeof *regions; r++)
        memcpy(rotating_memory + regions[r].address, shifting_memory + regions[r].address, regions[r].size);
}
static uint8_t *make_memory(void) {
    uint8_t *m = mmap(NULL, UINT64_C(1) << 32, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(m != MAP_FAILED);
    for (unsigned r = 0; r < sizeof regions / sizeof *regions; r++)
        assert(!mprotect(m + regions[r].address, regions[r].size, PROT_READ | PROT_WRITE));
    return m;
}
static int same_memory(void) {
    for (unsigned r = 0; r < sizeof regions / sizeof *regions; r++) {
        const Region *p = regions + r;
        if (memcmp(shifting_memory + p->address, rotating_memory + p->address, p->size)) {
            for (uint32_t i = p->address; i < p->address + p->size; i++)
                if (shifting_memory[i] != rotating_memory[i]) {
                    printf("MISMATCH guest byte %08X: %02X != %02X\n", i, shifting_memory[i], rotating_memory[i]); break;
                }
            return 0;
        }
    }
    return 1;
}
static int same_snapshot(const X87TranslatedSnapshot *a, const X87TranslatedSnapshot *b) {
    if (memcmp(a, b, offsetof(X87TranslatedSnapshot, failure))) return 0;
    if (!a->failure != !b->failure) return 0;
    return !a->failure || !strcmp(a->failure, b->failure);
}
static void show_snapshot(const char *label, const X87TranslatedSnapshot *s) {
    printf("  %s pc=%08X top=%u valid=%02X control=%04X status=%04X/%04X tags=%04X flags=%08X failure=%s host=%d/%d\n",
           label, s->pc, s->top, s->valid, s->control, s->status, s->status_word, s->tag_word,
           s->flags, s->failure ? s->failure : "none", s->host_round, s->host_exceptions);
    for (unsigned i = 0; i < 8; i++) printf(" %016llX", (unsigned long long)s->st[i]);
    puts("");
}
static int check_translated(void) {
    static const uint16_t controls[] = { 0x027F, 0x037F, 0x067F, 0x0A7F, 0x0E7F };
    static const unsigned depths[] = { 0, 2, 6, 8 };
    static const int host_modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
    unsigned calls = 0, failures = 0;
    for (unsigned corpus = 0; corpus < 2; corpus++) {
        assert(!fesetround(FE_TONEAREST)); fill_inputs(shifting_memory, corpus); clone_memory();
        uint8_t matrices[X87T_RECORDS * 0x40]; memcpy(matrices, shifting_memory + X87T_MATRIX(0), sizeof matrices);
        for (unsigned which = 0; which < X87T_FUNCTIONS; which++)
        for (unsigned alias = 0; alias < (which == 1 ? 4u : 1u); alias++)
        for (unsigned mode = 0; mode < sizeof controls / sizeof *controls; mode++)
        for (unsigned top = 0; top < 8; top++)
        for (unsigned depth = 0; depth < sizeof depths / sizeof *depths; depth++)
        for (unsigned k = 0; k < 16; k++) {
            /* Aliased calls mutate their input: restore both operands before
             * every case so the corpus remains varied instead of converging
             * to zeros/infinities after repeated matrix composition. */
            if (which == 1) for (unsigned j = k; j <= k + 1; j++) {
                memcpy(shifting_memory + X87T_MATRIX(j), matrices + j * 0x40, 0x40);
                memcpy(rotating_memory + X87T_MATRIX(j), matrices + j * 0x40, 0x40);
            }
            fenv_t initial;
            assert(!fesetround(host_modes[(mode + top) & 3]));
            feclearexcept(FE_ALL_EXCEPT); feraiseexcept(FE_DIVBYZERO); assert(!fegetenv(&initial));
            X87TranslatedSnapshot s, r;
            engine_flat_base = shifting_memory;
            x87t_shifting_call(which, k, top, depths[depth], controls[mode], alias, &s);
            assert(!fesetenv(&initial)); engine_flat_base = rotating_memory;
            x87t_rotating_call(which, k, top, depths[depth], controls[mode], alias, &r);
            if (!same_snapshot(&s, &r) || !same_memory() || (!depth && (r.pc != X87T_RETURN || r.failure))) {
                printf("MISMATCH %s corpus=%u record=%u TOP=%u depth=%u control=%04X alias=%u\n",
                       names[which], corpus, k, top, depths[depth], controls[mode], alias);
                show_snapshot("shifting", &s); show_snapshot("rotating", &r); return 0;
            }
            calls++; failures += r.failure != NULL;
        }
    }
    printf("PASS: %u translated calls, %u matching overflow failures; four leaves, every TOP, "
           "five control words/all rounding modes, depths 0/2/6/8, matrix aliases, finite/NaN/Inf/subnormal inputs.\n", calls, failures);
    puts("PASS: all logical x87 slots including empty contents, CPU registers/flags/control/status/tags, "
         "host FP flags/mode, and every mapped guest byte agree after each call; other guest pages protected.");
    return 1;
}
static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y);
}
static void benchmark(unsigned iterations, unsigned samples) {
    double *s = calloc(samples, sizeof *s), *r = calloc(samples, sizeof *r);
    assert(s && r); assert(!fesetround(FE_TONEAREST)); feclearexcept(FE_ALL_EXCEPT);
    rng = UINT64_C(0xD1B54A32D192ED03); fill_inputs(shifting_memory, 0); clone_memory();
    printf("BENCH: %u calls/sample, %u alternating-order pairs; release flags, finite inputs, empty entry stack; ns/call.\n",
           iterations, samples);
    for (unsigned which = 0; which < X87T_FUNCTIONS; which++) {
        for (unsigned sample = 0; sample < samples; sample++) {
            /* Warm both before each pair, then reverse first-run order. */
            engine_flat_base = shifting_memory; (void)x87t_shifting_time(which, 1000);
            engine_flat_base = rotating_memory; (void)x87t_rotating_time(which, 1000);
            for (unsigned side = 0; side < 2; side++) {
                unsigned rotating = side ^ (sample & 1);
                engine_flat_base = rotating ? rotating_memory : shifting_memory;
                if (rotating) r[sample] = x87t_rotating_time(which, iterations);
                else s[sample] = x87t_shifting_time(which, iterations);
            }
            printf("sample: %s %u shifting=%.3f rotating=%.3f\n", names[which], sample + 1, s[sample], r[sample]);
        }
        qsort(s, samples, sizeof *s, compare_double); qsort(r, samples, sizeof *r, compare_double);
        double sm = s[samples / 2], rm = r[samples / 2];
        printf("median: %s shifting=%.3f rotating=%.3f speedup=%.3fx saved=%.3f ns "
               "ranges=[%.3f,%.3f]/[%.3f,%.3f]\n", names[which], sm, rm, sm / rm, sm - rm,
               s[0], s[samples - 1], r[0], r[samples - 1]);
    }
    free(s); free(r);
}
int main(int argc, char **argv) {
    unsigned iterations = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 20000;
    unsigned samples = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 9;
    assert(samples && (samples & 1));
    fenv_t original; assert(!fegetenv(&original));
    shifting_memory = make_memory(); rotating_memory = make_memory();
    if (!check_translated()) return 1;
    if (iterations) benchmark(iterations, samples);
    assert(!fesetenv(&original));
    assert(!munmap(shifting_memory, UINT64_C(1) << 32));
    assert(!munmap(rotating_memory, UINT64_C(1) << 32));
    return 0;
}
