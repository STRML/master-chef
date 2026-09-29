/* Candidate: the very same generated sources and compiler options. */
#include "engine_cpu.h"
#define X87T_IMPL x87t_rotating
#define X87T_ST(c, i) ((c)->fp_reg[engine_fp_physical(c, i)])
#include "x87_translated_impl.inc"
