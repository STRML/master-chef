/* Isolated prototype. Not included by engine_cpu.h; default OFF. */
#ifndef ENGINE_ARM64_FENV_PROTOTYPE_H
#define ENGINE_ARM64_FENV_PROTOTYPE_H
#include <stdint.h>
#ifndef HALO_ARM64_FENV_PROTOTYPE
#define HALO_ARM64_FENV_PROTOTYPE 0
#endif
#if (HALO_ARM64_FENV_PROTOTYPE || HALO_ARM64_FENV_FAST) && defined(__aarch64__)
/* Apple SDK arm64 fenv.h: traps15,12:8. Apple XNU FPCR_RMODE22:23.
 * x87 nearest/down/up/zero -> ARM nearest/minus/plus/zero. All other
 * host FPCR controls are retained, matching feholdexcept + fesetround. */
static inline uint64_t halo_arm64_guest_round(unsigned mode) {
    static const unsigned map[4]={0,2,1,3};return (uint64_t)map[mode&3]<<22;
}
#ifndef HALO_ARM64_FENV_SKIP_UNCHANGED
#define HALO_ARM64_FENV_SKIP_UNCHANGED 1
#endif
#if HALO_ARM64_FENV_SKIP_UNCHANGED
#define HALO_FP_SET "cmp %[temp], %[old]\n\t" "b.eq 1f\n\t" "msr fpcr, %[temp]\n\t" "1:\n\t"
#define HALO_FP_RESTORE "cmp %[temp], %[old]\n\t" "b.eq 2f\n\t" "msr fpcr, %[old]\n\t" "2:\n\t"
#else
#define HALO_FP_SET "msr fpcr, %[temp]\n\t"
#define HALO_FP_RESTORE "msr fpcr, %[old]\n\t"
#endif
#define HALO_FP_ENV_ASM(OP) \
    "mrs %[old], fpcr\n\t" \
    "mrs %[status], fpsr\n\t" \
    "and %[temp], %[old], %[keep]\n\t" \
    "orr %[temp], %[temp], %[round]\n\t" \
    HALO_FP_SET \
    "and %[masked], %[status], %[statuskeep]\n\t" \
    "msr fpsr, %[masked]\n\t" OP "\n\t" \
    HALO_FP_RESTORE \
    "msr fpsr, %[status]"
#define HALO_FP_INPUTS \
    [keep] "r" (~UINT64_C(0xC09F00)), [statuskeep] "r" (~UINT64_C(0x9F)), [round] "r" (halo_arm64_guest_round(mode))
static inline double halo_arm64_fp_arithmetic(unsigned mode,unsigned operation,double a,double b) {
    uint64_t old,status,temp,masked;double result;
#define HALO_DO(OP) __asm__ volatile(HALO_FP_ENV_ASM(OP " %d[result], %d[a], %d[b]") \
    :[old] "=&r"(old),[status] "=&r"(status),[temp] "=&r"(temp),[masked] "=&r"(masked),[result] "=&w"(result) \
    :HALO_FP_INPUTS,[a] "w"(a),[b] "w"(b):"memory","cc")
    switch(operation){case 0:HALO_DO("fadd");break;case 1:HALO_DO("fsub");break;
        case 2:HALO_DO("fmul");break;case 3:HALO_DO("fdiv");break;default:__builtin_trap();}
#undef HALO_DO
    return result;
}
static inline float halo_arm64_fp_f32(unsigned mode,double a) {
    uint64_t old,status,temp,masked;float result;
    __asm__ volatile(HALO_FP_ENV_ASM("fcvt %s[result], %d[a]")
        :[old] "=&r"(old),[status] "=&r"(status),[temp] "=&r"(temp),[masked] "=&r"(masked),[result] "=&w"(result)
        :HALO_FP_INPUTS,[a] "w"(a):"memory","cc");
    return result;
}
#undef HALO_FP_SET
#undef HALO_FP_RESTORE
#undef HALO_FP_INPUTS
#undef HALO_FP_ENV_ASM
#endif
#endif
