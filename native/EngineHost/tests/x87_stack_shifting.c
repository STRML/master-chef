/* The shifting x87 stack side of test_x87_rotating_stack: the shared
 * operations and translated functions compiled against the old layout. */
#ifndef HALO_ARM64_FENV_FAST
#define HALO_ARM64_FENV_FAST 1   /* the release setting */
#endif
#include "x87_shifting_engine_cpu.h"
#define X87_IMPL shifting
#define X87_ST(c, i) ((c)->fp[i])
#include "x87_stack_ops.inc"
