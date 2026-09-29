/* The x87 round-to-nearest fast path is bit-identical to the FPCR/FPSR path.
 *
 * In nearest mode engine_fp_arithmetic, engine_write_f32, rounding to integer
 * and FSQRT now use plain binary64 operations instead of the inline FPCR/FPSR
 * round trip. This compares every result bit for bit with the round trip over
 * random operands and the awkward ones (signed zeros, subnormals, infinities,
 * NaNs, overflow, underflow), checks that the other rounding modes still take
 * the exact path, and times both paths.
 *
 * clang -O2 -DENGINE_FLAT_MEMORY=1 -DHALO_ARM64_FENV_FAST=1 -frounding-math -ffp-contract=off \
 *   -I native/EngineReuse native/EngineHost/tests/test_x87_nearest_fast_path.c -lm -o /tmp/x87 && /tmp/x87
 */
#ifndef HALO_ARM64_FENV_FAST
#define HALO_ARM64_FENV_FAST 1   /* the release setting */
#endif
#include "engine_cpu.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

uint8_t *engine_flat_base;
static uint64_t bits(double v) { uint64_t b; memcpy(&b, &v, 8); return b; }
static uint32_t bits32(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }
static uint64_t state = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { state ^= state << 13; state ^= state >> 7; state ^= state << 17; return state; }
static double operand(void) {
    static const double special[] = { 0.0, -0.0, 1.0, -1.0, 0.1, 3.0, 1e308, -1e308, 1e-308, 4.9e-324, -4.9e-324,
                                      2.2250738585072014e-308, 1e-310, (double)INFINITY, -(double)INFINITY, (double)NAN,
                                      3.4028234663852886e38, 1.1754943508222875e-38, 1.401298464324817e-45, 6.02e23 };
    uint64_t r = next();
    if ((r & 7) == 0) return special[(r >> 3) % (sizeof special / sizeof *special)];
    double v; uint64_t b = next(); memcpy(&v, &b, 8); return v;   /* any bit pattern */
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(void) {
#if !(HALO_ARM64_FENV_FAST && defined(__aarch64__))
    puts("SKIP: needs arm64 and HALO_ARM64_FENV_FAST=1"); return 0;
#else
    static uint8_t memory[64]; engine_flat_base = memory;
    EngineCPU c; memset(&c, 0, sizeof c); engine_fp_init(&c);   /* 0x037F: nearest */
    assert(engine_fp_nearest(&c) && fegetround() == FE_TONEAREST);
    unsigned checked = 0;
    for (int n = 0; n < 2000000; n++) {
        double a = operand(), b = operand();
        for (int op = ENGINE_FP_ADD; op <= ENGINE_FP_DIV; op++) {
            double fast = engine_fp_arithmetic(&c, op, a, b), exact = halo_arm64_fp_arithmetic(0, (unsigned)op, a, b);
            if (bits(fast) != bits(exact) && !(isnan(fast) && isnan(exact))) {
                printf("MISMATCH op %d a=%a b=%a fast=%a exact=%a\n", op, a, b, fast, exact); return 1;
            }
            checked++;
        }
        engine_write_f32(&c, 0, a);
        float stored; memcpy(&stored, memory, 4);
        float exact32 = halo_arm64_fp_f32(0, a);
        if (bits32(stored) != bits32(exact32) && !(isnan(stored) && isnan(exact32))) { printf("MISMATCH f32 %a\n", a); return 1; }
        double r = engine_fp_round_integer(&c, a, 0);
        int old = fegetround(); fesetround(FE_TONEAREST); double r2 = nearbyint(a); fesetround(old);
        if (bits(r) != bits(r2) && !(isnan(r) && isnan(r2))) { printf("MISMATCH round %a\n", a); return 1; }
        engine_fp_init(&c); engine_fp_push(&c, a); engine_fp_unary(&c, ENGINE_FP_FSQRT);
        double s = engine_fp_read(&c, 0), s2 = sqrt(a); engine_fp_pop(&c);
        if (bits(s) != bits(s2) && !(isnan(s) && isnan(s2))) { printf("MISMATCH sqrt %a\n", a); return 1; }
    }
    assert(fegetround() == FE_TONEAREST);
    /* The other rounding modes keep the exact path. */
    for (unsigned mode = 1; mode < 4; mode++) {
        engine_fp_control(&c, (uint16_t)(0x037F | (mode << 10)));
        assert(!engine_fp_nearest(&c));
        for (int n = 0; n < 200000; n++) {
            double a = operand(), b = operand();
            for (int op = ENGINE_FP_ADD; op <= ENGINE_FP_DIV; op++) {
                double got = engine_fp_arithmetic(&c, op, a, b), want = halo_arm64_fp_arithmetic(mode, (unsigned)op, a, b);
                assert(bits(got) == bits(want) || (isnan(got) && isnan(want)));
            }
        }
        assert(fegetround() == FE_TONEAREST);
    }
    /* Rounding to an integer in every guest mode equals nearbyint under that
     * mode, whatever the host mode is. */
    for (unsigned mode = 0; mode < 4; mode++) {
        engine_fp_control(&c, (uint16_t)(0x037F | (mode << 10)));
        static const int host_modes[] = { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO };
        for (int n = 0; n < 400000; n++) {
            double a = operand();
            if ((n & 3) == 1) a = (double)(int64_t)(next() % 2000001) / 4.0 - 250000.0;   /* halves and quarters */
            int old = fegetround(); fesetround(host_modes[mode]); double want = nearbyint(a); fesetround(old);
            fesetround(host_modes[n & 3]); double got = engine_fp_round_integer(&c, a, 0); fesetround(old);
            if (bits(got) != bits(want) && !(isnan(got) && isnan(want))) { printf("MISMATCH round mode %u %a\n", mode, a); return 1; }
        }
    }
    assert(fegetround() == FE_TONEAREST);
    engine_fp_control(&c, 0x037F);
    /* Timing: a dependent chain, as translated code produces. */
    volatile double seed = 1.0000001; double x = seed, y = seed;
    double t0 = now();
    for (int n = 0; n < 20000000; n++) x = engine_fp_arithmetic(&c, ENGINE_FP_MUL, x, 1.0000000001);
    double t1 = now();
    for (int n = 0; n < 20000000; n++) y = halo_arm64_fp_arithmetic(0, ENGINE_FP_MUL, y, 1.0000000001);
    double t2 = now();
    assert(bits(x) == bits(y));
    printf("PASS: %u nearest-mode results bit-identical to the FPCR/FPSR path (+f32 store, round, sqrt); other modes exact; integer rounding exact in all four modes under any host mode\n", checked);
    printf("timing: fast %.2f ns/op, FPCR/FPSR path %.2f ns/op (%.1fx)\n", (t1 - t0) / 2e7 * 1e9, (t2 - t1) / 2e7 * 1e9, (t2 - t1) / (t1 - t0));
    return 0;
#endif
}
