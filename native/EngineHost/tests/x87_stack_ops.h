/* Shared by test_x87_rotating_stack.c and x87_stack_shifting.c. Each file
 * compiles x87_stack_ops.inc against its own engine_cpu.h (the rotating one
 * and the shifting reference) and exports the same entry points under its
 * own prefix; the test drives both with the same inputs and compares what
 * the engine can observe. */
#ifndef X87_STACK_OPS_H
#define X87_STACK_OPS_H
#include <stdint.h>

/* One x87 instruction each, as the helper calls tools/engine_reuse/fpu.py
 * emits for it. */
enum {
    X87_PUSH, X87_FLD_M32, X87_FLD_M64, X87_FLD_M80, X87_FLD_ST, X87_FILD,
    X87_FST_ST, X87_FST_M, X87_FIST, X87_ARITH, X87_ARITH_M, X87_ARITH_INT,
    X87_COMPARE, X87_FNSTSW, X87_FNSTCW, X87_FLDCW, X87_FNINIT, X87_FNCLEX,
    X87_FXCH, X87_FFREE, X87_UNARY, X87_TRANSCENDENTAL, X87_FCMOV,
    X87_FNSTENV, X87_ENV_EDIT, X87_FNSAVE, X87_FRSTOR, X87_POKE, X87_READ,
    X87_KINDS
};
typedef struct {
    uint8_t kind, a, b, c, d, pop; /* stack indices, sizes and selectors */
    uint16_t word, word2;          /* control word; status and tag edits */
    uint32_t address;
    double value;
    uint64_t bits;
} X87Op;

/* Everything a caller of the x87 helpers can observe. */
typedef struct {
    uint64_t st[8];                /* bits of every logical slot, including empty slots */
    uint32_t gpr[8], flags, pc;
    uint16_t control, status, status_word, tag_word;
    uint8_t valid, top;
    const char *failure;           /* engine_fail reason, or NULL */
} X87Snapshot;

#define X87_DECLARE(prefix) \
    void prefix##_reset(unsigned top); \
    void prefix##_apply(const X87Op *op, X87Snapshot *out);
X87_DECLARE(shifting)
X87_DECLARE(rotating)
#endif
