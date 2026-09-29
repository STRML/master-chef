/* Identical helper operations compiled once per engine_cpu.h layout.
 * X87_IMPL sets the symbol prefix; X87_ST(c,i) maps logical slots. */
#include "x87_stack_ops.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define X87_CAT2(a, b) a##_##b
#define X87_CAT(a, b) X87_CAT2(a, b)
#define X87_NAME(n) X87_CAT(X87_IMPL, n)

static EngineCPU x87_cpu;
static jmp_buf x87_jump;
static const char *x87_failure;
static void x87_fail(EngineCPU *c, const char *reason) { (void)c; x87_failure = reason; longjmp(x87_jump, 1); }

static void x87_snapshot(EngineCPU *c, X87Snapshot *out) {
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < 8; i++)
        memcpy(&out->st[i], &X87_ST(c, i), 8);
    memcpy(out->gpr, c->gpr, sizeof out->gpr); out->flags = c->flags; out->pc = c->pc;
    out->control = c->fp_control; out->status = c->fp_status;
    out->status_word = engine_fp_status(c); out->tag_word = engine_fp_tag_word(c);
    out->valid = c->fp_valid; out->top = c->fp_top; out->failure = x87_failure;
}

static void x87_init(EngineCPU *c, unsigned top, uint16_t control) {
    memset(c, 0, sizeof *c); c->failure = x87_fail;
    engine_fp_init(c); engine_fp_control(c, control); c->fp_top = (uint8_t)(top & 7);
    for (unsigned i = 0; i < 8; i++) X87_ST(c, i) = 1.25 + i;
}
void X87_NAME(reset)(unsigned top) { x87_init(&x87_cpu, top, 0x027F); }

static void x87_execute(EngineCPU *cpu, const X87Op *op) {
    static const int arithmetic[4] = { ENGINE_FP_ADD, ENGINE_FP_SUB, ENGINE_FP_MUL, ENGINE_FP_DIV };
    const uint32_t a = op->address;
    const int operation = arithmetic[op->d & 3];
    switch (op->kind) {
    case X87_PUSH: engine_fp_push(cpu, op->value); break;                     /* fld1, fldz, fldpi... */
    case X87_FLD_M32: engine_fp_push(cpu, engine_read_f32(cpu, a)); break;
    case X87_FLD_M64: engine_fp_push(cpu, engine_read_f64(cpu, a)); break;
    case X87_FLD_M80: engine_fp_push(cpu, engine_read_f80(cpu, a)); break;
    case X87_FLD_ST: engine_fp_push(cpu, engine_fp_read(cpu, op->a)); break;
    case X87_FILD: engine_fp_load_integer(cpu, a, op->a == 0 ? 2 : op->a == 1 ? 4 : 8); break;
    case X87_FST_ST:                                                         /* fst/fstp st(i) */
        engine_fp_write(cpu, op->a, engine_fp_read(cpu, 0));
        if (op->pop) engine_fp_pop(cpu);
        break;
    case X87_FST_M:
        if (op->a == 0) engine_write_f32(cpu, a, engine_fp_read(cpu, 0));
        else if (op->a == 1) engine_write_f64(cpu, a, engine_fp_read(cpu, 0));
        else engine_write_f80(cpu, a, engine_fp_read(cpu, 0));
        if (op->pop) engine_fp_pop(cpu);
        break;
    case X87_FIST:                                                           /* fist/fistp/fisttp */
        engine_fp_store_integer(cpu, a, op->a == 0 ? 2 : op->a == 1 ? 4 : 8, op->c & 1);
        if (op->pop) engine_fp_pop(cpu);
        break;
    case X87_ARITH:                                                          /* fop st(i), st(j) and fopp */
        engine_fp_write(cpu, op->a, engine_fp_arithmetic(cpu, operation, engine_fp_read(cpu, op->b), engine_fp_read(cpu, op->c)));
        if (op->pop) engine_fp_pop(cpu);
        break;
    case X87_ARITH_M:                                                        /* fop m32/m64, reversed or not */
        if (op->c & 1) engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, operation, op->a ? engine_read_f64(cpu, a) : engine_read_f32(cpu, a), engine_fp_read(cpu, 0)));
        else engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, operation, engine_fp_read(cpu, 0), op->a ? engine_read_f64(cpu, a) : engine_read_f32(cpu, a)));
        break;
    case X87_ARITH_INT:                                                      /* fiop m16/m32 */
        if (op->c & 1) engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, operation, engine_fp_integer_operand(cpu, a, op->a ? 4 : 2), engine_fp_read(cpu, 0)));
        else engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, operation, engine_fp_read(cpu, 0), engine_fp_integer_operand(cpu, a, op->a ? 4 : 2)));
        break;
    case X87_COMPARE: {                                                      /* fcom/fucom/fcomi/ficom/ftst [p[p]] */
        switch (op->a % 5) {
        case 0: engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_fp_read(cpu, op->b), op->c & 1); break;
        case 1: engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_read_f32(cpu, a), op->c & 1); break;
        case 2: engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_read_f64(cpu, a), op->c & 1); break;
        case 3: engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_fp_integer_operand(cpu, a, op->b & 1 ? 4 : 2), 0); break;
        default: engine_fp_compare(cpu, engine_fp_read(cpu, 0), 0.0, 0); break;
        }
        for (unsigned n = 0; n < op->pop; n++) engine_fp_pop(cpu);
        break;
    }
    case X87_FNSTSW: engine_write_u16(cpu, a, engine_fp_status(cpu)); break;
    case X87_FNSTCW: engine_write_u16(cpu, a, cpu->fp_control); break;
    case X87_FLDCW: engine_fp_control(cpu, op->word); break;
    case X87_FNINIT: engine_fp_init(cpu); break;
    case X87_FNCLEX: cpu->fp_status &= (uint16_t)~0x80ffu; break;
    case X87_FXCH: engine_fp_exchange(cpu, op->a); break;
    case X87_FFREE: engine_fp_free(cpu, op->a); break;
    case X87_UNARY: engine_fp_unary(cpu, ENGINE_FP_FCHS + (op->a & 3)); break;
    case X87_TRANSCENDENTAL: {
        int trans = op->a % 13;
        /* The frozen interpreter casts FSCALE's exponent and FPREM's
         * quotient to integers without defining NaN/Inf conversion. Keep
         * those operands within its defined C domain; all other helpers
         * still receive arbitrary bits, NaNs, infinities and subnormals. */
        if (trans == ENGINE_FP_FSCALE && (cpu->fp_valid & 2u) && isnan(X87_ST(cpu, 1)))
            engine_fp_write(cpu, 1, 3.0);
        if ((trans == ENGINE_FP_FPREM || trans == ENGINE_FP_FPREM1) && (cpu->fp_valid & 3u) == 3u &&
            !isfinite(X87_ST(cpu, 0) / X87_ST(cpu, 1))) {
            engine_fp_write(cpu, 0, 7.5); engine_fp_write(cpu, 1, 2.0);
        }
        engine_fp_transcendental(cpu, trans);
        break;
    }
    case X87_FCMOV: if (op->b & 1) engine_fp_write(cpu, 0, engine_fp_read(cpu, op->a)); break;
    case X87_FNSTENV: engine_fp_store_environment(cpu, a); break;
    case X87_ENV_EDIT: {
        /* Arbitrary control/status/TOP/tag edits, including resurrecting
         * empty registers, must preserve the previous interpreter exactly. */
        engine_fp_store_environment(cpu, a);
        engine_write_u32(cpu, a, op->word);
        engine_write_u32(cpu, a + 4, op->word2);
        engine_write_u32(cpu, a + 8, (uint16_t)op->bits);
        engine_fp_load_environment(cpu, a);
        break;
    }
    case X87_FNSAVE: engine_fp_save(cpu, a); break;
    case X87_FRSTOR: engine_fp_restore(cpu, a); break;
    case X87_POKE: engine_write_u64(cpu, a, op->bits); break;
    case X87_READ: (void)engine_fp_read(cpu, op->a); break;
    default: abort();
    }
}

void X87_NAME(apply)(const X87Op *op, X87Snapshot *out) {
    x87_failure = NULL;
    if (!setjmp(x87_jump)) x87_execute(&x87_cpu, op);
    x87_snapshot(&x87_cpu, out);
}
