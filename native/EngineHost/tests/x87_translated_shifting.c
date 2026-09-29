/* Frozen baseline: compile actual generated functions against the old header. */
#include "x87_shifting_engine_cpu.h"
#define X87T_IMPL x87t_shifting
#define X87T_ST(c, i) ((c)->fp[i])
#include "x87_translated_impl.inc"
