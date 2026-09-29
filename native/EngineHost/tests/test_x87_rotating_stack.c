/* The x87 register stack rotates over the register file (ST(i) lives in
 * fp_reg[(fp_top+i)&7], without shifting live values on push/pop) instead of keeping
 * ST(0) in slot 0 and shifting all eight registers on every push and pop.
 *
 * Differential check against the shifting layout it replaces
 * (x87_shifting_engine_cpu.h, the old header verbatim, compiled in
 * x87_stack_shifting.c):
 *  1. random streams of every x87 helper call the translator emits (loads,
 *     stores, integer loads/stores, arithmetic in all its forms, compares
 *     with their pops, FXCH, FFREE, FCMOV, unary and transcendental
 *     operations, FLDCW, FNINIT, FNCLEX, FNSTSW, FNSTENV/FLDENV, FNSAVE/
 *     FRSTOR, reads of empty registers, over- and underflow), with random
 *     and awkward values, rounding modes and starting TOP; after every
 *     operation the logical ST(0)..ST(7) bits, the tag bits, TOP, the status
 *     and control words, the tag word, EFLAGS, the engine_fail reason and
 *     every byte of the guest memory the operations write must match;
 *     Empty slots are compared too: FLDENV can make them valid again.
 */
#ifndef HALO_ARM64_FENV_FAST
#define HALO_ARM64_FENV_FAST 1   /* the release setting */
#endif
#include "engine_cpu.h"
#include "x87_stack_ops.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

uint8_t *engine_flat_base;
static uint8_t *memory_shifting, *memory_rotating;
enum { WINDOW = 0x1000, WINDOW_SIZE = 0x400, ENVIRONMENTS = 0x1200 };

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static unsigned below(unsigned n) { return (unsigned)(next() % n); }
static int chance(unsigned percent) { return below(100) < percent; }

static double awkward(void) {
    static const double special[] = { 0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5, 3.0, 0.1, 1e308, -1e308,
        4.9e-324, -4.9e-324, 2.2250738585072014e-308, 1e-310, (double)INFINITY, -(double)INFINITY, (double)NAN,
        -(double)NAN, 9223372036854775808.0, -9223372036854775808.0, 2147483647.5, 2147483648.0, -2147483648.5,
        32767.5, -32768.5, 0.49999999999999994, 3.4028234663852886e38, 1.401298464324817e-45, 6.02e23, 1e-3 };
    uint64_t r = next();
    switch (r & 3) {
    case 0: return special[(r >> 2) % (sizeof special / sizeof *special)];
    case 1: { double v; uint64_t b = next(); memcpy(&v, &b, 8); return v; }              /* any bit pattern */
    default: return ((double)(int64_t)(next() % 2000001) - 1000000.0) / (double)(1 + below(64));
    }
}
static uint16_t control_word(void) {
    if (chance(70)) return 0x027F;                                                    /* Halo's */
    if (chance(50)) return (uint16_t)(0x027F | (below(4) << 10));                    /* its rounding modes */
    return (uint16_t)next();
}
/* Mostly a register in use, sometimes any register, rarely an invalid index. */
static uint8_t stack_index(uint8_t valid) {
    if (chance(1)) return 8;
    if (chance(12) || !valid) return (uint8_t)below(8);
    unsigned depth = 0; while (depth < 8 && (valid & (1u << depth))) depth++;
    return (uint8_t)below(depth ? depth : 1);
}
static uint32_t scalar_address(void) {
    return WINDOW + (chance(80) ? 8 * below(64) : below(0x1F0));
}

static X87Op random_op(uint8_t valid) {
    /* Emphasize loads, stores and popping arithmetic. Depth steers pushes against pops so the stack
     * moves through every depth and now and then over- or underflows. */
    static const unsigned weight[X87_KINDS] = {
        [X87_PUSH] = 60, [X87_FLD_M32] = 60, [X87_FLD_M64] = 20, [X87_FLD_M80] = 8, [X87_FLD_ST] = 30,
        [X87_FILD] = 15, [X87_FST_ST] = 25, [X87_FST_M] = 60, [X87_FIST] = 15, [X87_ARITH] = 90,
        [X87_ARITH_M] = 50, [X87_ARITH_INT] = 8, [X87_COMPARE] = 30, [X87_FNSTSW] = 8, [X87_FNSTCW] = 3,
        [X87_FLDCW] = 6, [X87_FNINIT] = 2, [X87_FNCLEX] = 3, [X87_FXCH] = 30, [X87_FFREE] = 3,
        [X87_UNARY] = 15, [X87_TRANSCENDENTAL] = 12, [X87_FCMOV] = 10, [X87_FNSTENV] = 3,
        [X87_ENV_EDIT] = 4, [X87_FNSAVE] = 2, [X87_FRSTOR] = 3, [X87_POKE] = 15, [X87_READ] = 8 };
    unsigned depth = (unsigned)__builtin_popcount(valid), total = 0, w[X87_KINDS];
    for (unsigned k = 0; k < X87_KINDS; k++) {
        w[k] = weight[k];
        int pushes = k == X87_PUSH || k == X87_FLD_M32 || k == X87_FLD_M64 || k == X87_FLD_M80 || k == X87_FLD_ST || k == X87_FILD;
        if (pushes && depth >= 6) w[k] /= 4;
        if (pushes && depth <= 1) w[k] *= 3;
        total += w[k];
    }
    unsigned pick = below(total), kind = 0;
    while (pick >= w[kind]) pick -= w[kind++];
    X87Op op; memset(&op, 0, sizeof op);
    op.kind = (uint8_t)kind;
    op.a = stack_index(valid); op.b = stack_index(valid); op.c = stack_index(valid);
    op.d = (uint8_t)below(4);
    unsigned pop_percent = depth >= 5 ? 70 : depth <= 1 ? 15 : 45;
    op.pop = (uint8_t)(chance(pop_percent) ? (kind == X87_COMPARE && chance(30) ? 2 : 1) : 0);
    op.word = control_word(); op.word2 = (uint16_t)next();
    op.address = scalar_address();
    op.value = awkward();
    switch (kind) {
    case X87_FILD: case X87_FIST: case X87_FST_M: op.a = (uint8_t)below(3); op.c = (uint8_t)below(2); break;
    case X87_ARITH_M: case X87_ARITH_INT: op.a = (uint8_t)below(2); op.c = (uint8_t)below(2); break;
    case X87_ARITH:
        /* fop st(0),st(i) / fop st(i),st(0) / fopp st(i),st(0); a few arbitrary pairs */
        if (chance(90)) {
            unsigned i = stack_index(valid);
            if (op.pop) { op.a = (uint8_t)(i ? i : 1); op.b = op.a; op.c = 0; }
            else if (chance(50)) { op.a = 0; op.b = 0; op.c = (uint8_t)i; }
            else { op.a = (uint8_t)i; op.b = (uint8_t)i; op.c = 0; }
            if (chance(40)) { uint8_t t = op.b; op.b = op.c; op.c = t; }          /* fsubr/fdivr */
        }
        break;
    case X87_COMPARE: op.a = (uint8_t)below(5); op.c = (uint8_t)below(2); break;
    case X87_FCMOV: op.b = (uint8_t)below(2); break;
    case X87_TRANSCENDENTAL: op.a = (uint8_t)below(13); break;                 /* FSIN ... FXAM, FXTRACT */
    case X87_ENV_EDIT: op.bits = next(); /* arbitrary physical tags; fall through */
    case X87_FNSTENV: case X87_FNSAVE: case X87_FRSTOR: op.address = ENVIRONMENTS + 0x80 * below(4); break;
    case X87_POKE:
        if (chance(50)) { double v = awkward(); memcpy(&op.bits, &v, 8); }
        else op.bits = next();
        if (chance(30)) op.address = ENVIRONMENTS + 0x80 * below(4) + 8 * below(13);
        break;
    default: break;
    }
    return op;
}

static int same_snapshot(const X87Snapshot *x, const X87Snapshot *y) {
    if (memcmp(x->st, y->st, sizeof x->st) || memcmp(x->gpr, y->gpr, sizeof x->gpr)) return 0;
    if (x->flags != y->flags || x->pc != y->pc || x->control != y->control || x->status != y->status) return 0;
    if (x->status_word != y->status_word || x->tag_word != y->tag_word || x->valid != y->valid || x->top != y->top) return 0;
    if (!x->failure != !y->failure) return 0;
    return !x->failure || !strcmp(x->failure, y->failure);
}
static void print_snapshot(const char *name, const X87Snapshot *s) {
    printf("  %-9s top %u valid %02X status %04X/%04X control %04X tags %04X flags %08X pc %08X failure %s\n",
           name, s->top, s->valid, s->status, s->status_word, s->control, s->tag_word, s->flags, s->pc,
           s->failure ? s->failure : "-");
    printf("  %9s", "");
    for (unsigned i = 0; i < 8; i++) printf(" %016llX", (unsigned long long)s->st[i]);
    printf("\n  %9s", "");
    for (unsigned i = 0; i < 8; i++) printf(" %08X", s->gpr[i]);
    printf("\n");
}

int main(void) {
    memory_shifting = mmap(NULL, UINT64_C(1) << 32, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    memory_rotating = mmap(NULL, UINT64_C(1) << 32, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_NORESERVE, -1, 0);
    assert(memory_shifting != MAP_FAILED && memory_rotating != MAP_FAILED);
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define X87_SANITIZED 1
#endif
#endif
#ifdef X87_SANITIZED
    const unsigned sequences = 1500;
#else
    const unsigned sequences = 12000;
#endif
    /* 1. Random operation streams. */
    unsigned long long operations = 0, failures = 0, per_kind[X87_KINDS] = {0}, successful[X87_KINDS] = {0};
    unsigned trans_seen = 0, unary_seen = 0, arithmetic_seen = 0, rounding_seen = 0;
    unsigned depth_seen = 0, tops_seen = 0;
    for (unsigned sequence = 0; sequence < sequences; sequence++) {
        unsigned top = below(8);
        for (unsigned i = 0; i < WINDOW_SIZE; i++) memory_shifting[WINDOW + i] = memory_rotating[WINDOW + i] = (uint8_t)next();
        engine_flat_base = memory_shifting; shifting_reset(top);
        engine_flat_base = memory_rotating; rotating_reset(top);
        uint8_t valid = 0;
        for (unsigned step = 0; step < 300; step++) {
            X87Op op = random_op(valid);
            X87Snapshot s, r;
            engine_flat_base = memory_shifting; shifting_apply(&op, &s);
            engine_flat_base = memory_rotating; rotating_apply(&op, &r);
            if (!same_snapshot(&s, &r) || memcmp(memory_shifting + WINDOW, memory_rotating + WINDOW, WINDOW_SIZE)) {
                printf("MISMATCH sequence %u step %u kind %u a %u b %u c %u d %u pop %u address %08X value %a\n",
                       sequence, step, op.kind, op.a, op.b, op.c, op.d, op.pop, op.address, op.value);
                print_snapshot("shifting", &s); print_snapshot("rotating", &r);
                for (unsigned i = 0; i < WINDOW_SIZE; i++)
                    if (memory_shifting[WINDOW + i] != memory_rotating[WINDOW + i]) { printf("  memory differs at %08X\n", WINDOW + i); break; }
                return 1;
            }
            operations++; per_kind[op.kind]++; failures += r.failure != NULL;
            if (!r.failure) {
                successful[op.kind]++;
                if (op.kind == X87_TRANSCENDENTAL) trans_seen |= 1u << op.a;
                if (op.kind == X87_UNARY) unary_seen |= 1u << (op.a & 3);
                if (op.kind == X87_ARITH) arithmetic_seen |= 1u << op.d;
                rounding_seen |= 1u << ((r.control >> 10) & 3);
            }
            valid = r.valid; depth_seen |= 1u << __builtin_popcount(valid); tops_seen |= 1u << r.top;
        }
    }
    for (unsigned k = 0; k < X87_KINDS; k++) assert(per_kind[k] > 0 && successful[k] > 0);
    assert(trans_seen == 0x1FFF && unary_seen == 15 && arithmetic_seen == 15 && rounding_seen == 15);
    assert(depth_seen == 0x1FF && tops_seen == 0xFF && failures > 0);
    printf("PASS: %llu random x87 operations (%d kinds, every depth 0-8 and TOP 0-7, %llu engine_fail cases) "
           "identical to the shifting stack: ST(0)-ST(7), tags, TOP, status, control, tag word, EFLAGS, failures, guest memory\n",
           operations, X87_KINDS, failures);

    return 0;
}

#define X87_IMPL rotating
#define X87_ST(c, i) ((c)->fp_reg[engine_fp_physical((c), (i))])
#include "x87_stack_ops.inc"
