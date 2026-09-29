/* Included once for each layout. No generated source is copied or modified. */
#include "x87_translated.h"
#include <setjmp.h>
#include <stdio.h>
#include <time.h>

#define X87T_CAT2(a, b) a##_##b
#define X87T_CAT(a, b) X87T_CAT2(a, b)
#define X87T_NAME(n) X87T_CAT(X87T_IMPL, n)

static jmp_buf x87t_jump;
static const char *x87t_failure;
static void x87t_fail(EngineCPU *c, const char *reason) {
    (void)c; x87t_failure = reason; longjmp(x87t_jump, 1);
}
void X87T_NAME(sub_0050D5B0)(EngineCPU *cpu);
void X87T_NAME(sub_004CC0D0)(EngineCPU *cpu);
void X87T_NAME(sub_00554260)(EngineCPU *cpu);
void X87T_NAME(sub_00553380)(EngineCPU *cpu);

static void x87t_init(EngineCPU *c, unsigned top, unsigned depth, uint16_t control) {
    memset(c, 0, sizeof *c);
    c->failure = x87t_fail;
    engine_fp_init(c); engine_fp_control(c, control); c->fp_top = (uint8_t)top;
    /* Distinct raw values, including empty slots, with equal logical seeds. */
    for (unsigned i = 0; i < 8; i++) X87T_ST(c, i) = (17.25 + i) * (i & 1 ? -1 : 1);
    c->fp_valid = (uint8_t)((1u << depth) - 1u);
    c->fp_status = 0x4500u;
    for (unsigned i = 0; i < 8; i++) c->gpr[i] = 0x12345000u + i * 0x1111u;
    c->flags = 0x202u; c->instruction_count = 0x1234u; c->fs_base = 0x23456000u;
}

static void x87t_call_one(EngineCPU *c, unsigned which, unsigned k, unsigned alias) {
    c->gpr[4] = X87T_STACK;
    switch (which) {
    case 0:
        engine_push(c, k & 1, 4); engine_push(c, X87T_RETURN, 4);
        c->gpr[1] = X87T_OBJECT(k); c->gpr[7] = X87T_BOX(k); c->pc = 0x0050D5B0u;
        X87T_NAME(sub_0050D5B0)(c); break;
    case 1: {
        /* Exercise both translated memmove branches, including all equal. */
        uint32_t left = X87T_MATRIX(k), right = X87T_MATRIX((k + 1) % X87T_RECORDS);
        uint32_t out = alias == 1 ? left : alias == 2 ? right : X87T_OUTPUT(k);
        if (alias == 3) right = out = left;
        engine_push(c, out, 4); engine_push(c, right, 4); engine_push(c, left, 4);
        engine_push(c, X87T_RETURN, 4); c->pc = 0x004CC0D0u;
        X87T_NAME(sub_004CC0D0)(c); break;
    }
    case 2:
        engine_push(c, X87T_RETURN, 4); c->gpr[0] = X87T_BOX(k);
        c->gpr[3] = X87T_PLANES(k); c->gpr[7] = 6; c->pc = 0x00554260u;
        X87T_NAME(sub_00554260)(c); break;
    default:
        engine_push(c, X87T_RETURN, 4); c->gpr[1] = X87T_BOX(k);
        c->gpr[2] = X87T_BYTES(k); c->gpr[6] = X87T_OUTPUT(k); c->pc = 0x00553380u;
        X87T_NAME(sub_00553380)(c); break;
    }
}

void X87T_NAME(call)(unsigned which, unsigned record, unsigned top, unsigned depth,
                    uint16_t control, unsigned alias, X87TranslatedSnapshot *out) {
    /* Static CPU remains defined after a failure callback's longjmp. */
    static EngineCPU c;
    x87t_init(&c, top, depth, control); x87t_failure = NULL;
    if (!setjmp(x87t_jump)) x87t_call_one(&c, which, record, alias);
    memset(out, 0, sizeof *out);
    for (unsigned i = 0; i < 8; i++) { double v = X87T_ST(&c, i); memcpy(out->st + i, &v, 8); }
    memcpy(out->gpr, c.gpr, sizeof out->gpr);
    out->flags = c.flags; out->pc = c.pc; out->control = c.fp_control;
    out->status = c.fp_status; out->status_word = engine_fp_status(&c);
    out->tag_word = engine_fp_tag_word(&c); out->valid = c.fp_valid; out->top = c.fp_top;
    out->instruction_count = c.instruction_count; out->instruction_limit = c.instruction_limit;
    out->fs_base = c.fs_base; out->failure = x87t_failure;
    out->host_round = fegetround(); out->host_exceptions = fetestexcept(FE_ALL_EXCEPT);
}

double X87T_NAME(time)(unsigned which, unsigned iterations) {
    EngineCPU c; x87t_init(&c, 0, 0, 0x027F);
    c.failure = NULL; /* Timing has no active setjmp frame; unexpected faults abort. */
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (unsigned n = 0; n < iterations; n++) {
        x87t_call_one(&c, which, n & (X87T_RECORDS - 1), 0);
        if (c.pc != X87T_RETURN || c.fp_valid) abort();
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return ((t1.tv_sec - t0.tv_sec) * 1e9 + t1.tv_nsec - t0.tv_nsec) / iterations;
}

/* Generated sources use the register macros; keep these after the harness. */
#include "engine_flags.h"
#include "engine_registers.h"
#define sub_0050D5B0 X87T_NAME(sub_0050D5B0)
#define sub_004CC0D0 X87T_NAME(sub_004CC0D0)
#define sub_00554260 X87T_NAME(sub_00554260)
#define sub_00553380 X87T_NAME(sub_00553380)
#include <sub_0050D5B0.c>
#include <sub_004CC0D0.c>
#include <sub_00554260.c>
#include <sub_00553380.c>
