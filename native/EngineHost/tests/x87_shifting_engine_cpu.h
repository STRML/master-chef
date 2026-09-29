/* Test reference only: native/EngineReuse/engine_cpu.h exactly as it was
 * before the x87 stack became a rotating register file (commit f6766e0,
 * branch astra/build75-engine-perf), when ST(i) always lived in fp[i] and
 * every push and pop shifted the eight doubles. test_x87_rotating_stack
 * compiles the same x87 operations and the same translated functions
 * against this header and the current one and requires identical results.
 * Everything below this comment is the old header, unchanged. */
#ifndef HALO_ENGINE_CPU_H
#define HALO_ENGINE_CPU_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <fenv.h>
#ifndef HALO_ARM64_FENV_FAST
#define HALO_ARM64_FENV_FAST 0
#endif
#if HALO_ARM64_FENV_FAST && defined(__aarch64__)
#include "engine_arm64_fenv_prototype.h"
#endif
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF

typedef struct EngineCPU EngineCPU;
typedef void *(*EngineAddress)(EngineCPU *, uint32_t, size_t);
typedef void (*EngineFailure)(EngineCPU *, const char *);
struct EngineCPU {
    uint32_t gpr[8], flags, pc;
    double fp[8];
    uint8_t fp_valid, fp_top;
    uint16_t fp_control, fp_status;
    EngineAddress address;
    EngineAddress write_address;
    EngineFailure failure;
    void *context;
    uint64_t instruction_limit, instruction_count;
    uint32_t fs_base; /* Per-thread guest TEB; zero retains the primary-thread default. */
};

_Noreturn static inline void engine_fail(EngineCPU *cpu, const char *reason) {
    if (cpu->failure) cpu->failure(cpu, reason);
    abort(); /* A failure callback must leave execution; never resume a bad instruction. */
}
extern uint32_t engine_trace_lo, engine_trace_hi;
void engine_pc_trace(EngineCPU *cpu);
/* Per-instruction hook. The lean default only records the pc (faults, shims
 * and dispatch need it). ENGINE_STEP_FULL=1 restores the instruction budget,
 * the instruction counter and the HALO_TRACE_LO/HI pc trace, which the
 * bounded-invocation tests rely on; the game builds do not use them and the
 * checks cost several host operations on every translated instruction. */
#ifndef ENGINE_STEP_FULL
#define ENGINE_STEP_FULL 0
#endif
static inline void engine_step(EngineCPU *cpu,uint32_t pc) {
    cpu->pc=pc;
#if ENGINE_STEP_FULL
    if(cpu->instruction_limit && cpu->instruction_count>=cpu->instruction_limit)
        engine_fail(cpu,"original instruction budget exhausted");
    cpu->instruction_count++;
    if(pc>=engine_trace_lo && pc<engine_trace_hi) engine_pc_trace(cpu);
#endif
}
#ifdef ENGINE_FLAT_MEMORY
/* Host mode: the whole 32-bit guest address space is one flat mapping. */
extern uint8_t *engine_flat_base;
static inline void *engine_address(EngineCPU *cpu, uint32_t a, size_t n) {(void)cpu;(void)n;return engine_flat_base + a;}
static inline void *engine_write_address(EngineCPU *cpu,uint32_t a,size_t n) {(void)cpu;(void)n;return engine_flat_base + a;}
#else
static inline void *engine_address(EngineCPU *cpu, uint32_t a, size_t n) {
    if ((uint64_t)a + n > UINT64_C(0x100000000)) engine_fail(cpu, "address wrap");
    void *p = cpu->address(cpu, a, n);
    if (!p) engine_fail(cpu, "unmapped guest address");
    return p;
}
static inline void *engine_write_address(EngineCPU *cpu,uint32_t a,size_t n) {
    if(!cpu->write_address)return engine_address(cpu,a,n);
    if((uint64_t)a+n>UINT64_C(0x100000000))engine_fail(cpu,"write address wrap");
    void *p=cpu->write_address(cpu,a,n);
    if(!p)engine_fail(cpu,"unmapped or readonly guest write");
    return p;
}
#endif
#define ENGINE_MEMORY_ACCESS(bits,type) \
static inline type engine_read_u##bits(EngineCPU *c,uint32_t a) {type v;memcpy(&v,engine_address(c,a,sizeof v),sizeof v);return v;} \
static inline void engine_write_u##bits(EngineCPU *c,uint32_t a,type v) {memcpy(engine_write_address(c,a,sizeof v),&v,sizeof v);}
ENGINE_MEMORY_ACCESS(8,uint8_t)
ENGINE_MEMORY_ACCESS(16,uint16_t)
ENGINE_MEMORY_ACCESS(32,uint32_t)
ENGINE_MEMORY_ACCESS(64,uint64_t)
#undef ENGINE_MEMORY_ACCESS
static inline void engine_push(EngineCPU *c,uint32_t value,unsigned size) {
    if(size!=2&&size!=4)engine_fail(c,"invalid push width");
    c->gpr[4]-=size;
    if(size==2)engine_write_u16(c,c->gpr[4],(uint16_t)value);
    else engine_write_u32(c,c->gpr[4],value);
}
static inline uint32_t engine_pop(EngineCPU *c,unsigned size) {
    if(size!=2&&size!=4)engine_fail(c,"invalid pop width");
    uint32_t value=size==2?engine_read_u16(c,c->gpr[4]):engine_read_u32(c,c->gpr[4]);
    c->gpr[4]+=size;return value;
}
static inline void engine_divide(EngineCPU *c,uint32_t divisor,unsigned bits,int sign) {
    if(bits!=8&&bits!=16&&bits!=32)engine_fail(c,"invalid divide width");
    uint64_t mask=bits==32?UINT32_MAX:((UINT64_C(1)<<bits)-1);
    uint64_t numerator=bits==8?(c->gpr[0]&0xffffu):
        (((uint64_t)c->gpr[2]&mask)<<bits)|(c->gpr[0]&mask);
    divisor&=(uint32_t)mask;
    if(!divisor)engine_fail(c,"integer division by zero");
    uint64_t quotient,remainder;
    if(sign){
        int64_t n=bits==8?(int16_t)numerator:bits==16?(int32_t)numerator:(int64_t)numerator;
        int64_t d=bits==8?(int8_t)divisor:bits==16?(int16_t)divisor:(int32_t)divisor;
        if(n==INT64_MIN&&d==-1)engine_fail(c,"integer division overflow");
        int64_t q=n/d,r=n%d,limit=INT64_C(1)<<(bits-1);
        if(q < -limit || q>=limit)engine_fail(c,"integer quotient overflow");
        quotient=(uint64_t)q&mask;remainder=(uint64_t)r&mask;
    }else{quotient=numerator/divisor;remainder=numerator%divisor;
        if(quotient>mask)engine_fail(c,"integer quotient overflow");}
    if(bits==8)c->gpr[0]=(c->gpr[0]&0xffff0000u)|(uint32_t)quotient|((uint32_t)remainder<<8);
    else{c->gpr[0]=(c->gpr[0]&~(uint32_t)mask)|(uint32_t)quotient;
         c->gpr[2]=(c->gpr[2]&~(uint32_t)mask)|(uint32_t)remainder;}
    /* Arithmetic flags are undefined after DIV/IDIV; retained deterministically. */
}

static inline double engine_read_f32(EngineCPU *c,uint32_t a) {
    float v;memcpy(&v,engine_address(c,a,4),4);return (double)v;
}
static inline double engine_read_f64(EngineCPU *c,uint32_t a) {
    double v;memcpy(&v,engine_address(c,a,8),8);return v;
}
static inline int engine_fp_round_mode(EngineCPU *c) {
    static const int modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    return modes[(c->fp_control>>10)&3];
}
/* Round-to-nearest is the guest's rounding mode almost all the time, and the
 * host's own: the engine thread's FPCR is left at nearest by every path that
 * changes it. In that mode an x87 operation is exactly the plain binary64 one.
 * The FPCR/FPSR round trip below exists for the other modes, and every guest
 * add, subtract, multiply, divide and float store paid for it anyway: two
 * serialising FPSR writes each, about 23 ns an operation against about 1 ns,
 * at 31k call sites that the game tick and every bearing pass run through.
 * The host's floating-point exception flags are observed by neither the
 * engine nor the host (the guest's own status word is fp_status), so leaving
 * them sticky loses nothing. */
static inline int engine_fp_nearest(const EngineCPU *c) {
    return ((c->fp_control>>10)&3)==0;
}
static inline void engine_write_f32(EngineCPU *c,uint32_t a,double v) {
    if(engine_fp_nearest(c)){float value=(float)v;memcpy(engine_write_address(c,a,4),&value,4);return;}
#if HALO_ARM64_FENV_FAST && defined(__aarch64__)
    float value=halo_arm64_fp_f32((c->fp_control>>10)&3,v);
#else
    fenv_t environment;feholdexcept(&environment);fesetround(engine_fp_round_mode(c));
    volatile float value=(float)v;fesetenv(&environment);
#endif
    memcpy(engine_write_address(c,a,4),(const void*)&value,4);
}
static inline void engine_write_f64(EngineCPU *c,uint32_t a,double v) {
    memcpy(engine_write_address(c,a,8),&v,8);
}
static inline void engine_fp_init(EngineCPU *c) {
    c->fp_valid=0;c->fp_top=0;c->fp_control=0x037f;c->fp_status=0;
}
static inline void engine_fp_control(EngineCPU *c,uint16_t word) {
    c->fp_control=word; /* Exception masks and precision control are recorded, not enforced. */
}
static inline double engine_fp_read(EngineCPU *c,unsigned i) {
    if(i>=8 || !(c->fp_valid&(1u<<i))) engine_fail(c,"empty x87 stack register");
    return c->fp[i];
}
static inline void engine_fp_write(EngineCPU *c,unsigned i,double value) {
    if(i>=8) engine_fail(c,"invalid x87 stack register");
    c->fp[i]=value;c->fp_valid|=(uint8_t)(1u<<i);
}
static inline void engine_fp_push(EngineCPU *c,double value) {
    if(c->fp_valid&0x80) engine_fail(c,"x87 stack overflow");
    for(unsigned i=7;i;i--)c->fp[i]=c->fp[i-1];
    c->fp_valid=(uint8_t)((c->fp_valid<<1)|1);
    c->fp[0]=value;c->fp_top=(c->fp_top-1)&7;
}
static inline void engine_fp_pop(EngineCPU *c) {
    (void)engine_fp_read(c,0);
    for(unsigned i=0;i<7;i++)c->fp[i]=c->fp[i+1];
    c->fp_valid>>=1;c->fp_top=(c->fp_top+1)&7;
}
static inline void engine_fp_exchange(EngineCPU *c,unsigned i) {
    double a=engine_fp_read(c,0),b=engine_fp_read(c,i);
    engine_fp_write(c,0,b);engine_fp_write(c,i,a);
}
static inline uint16_t engine_fp_status(EngineCPU *c) {
    return (c->fp_status&~0x3800u)|((uint16_t)c->fp_top<<11);
}
static inline void engine_fp_compare(EngineCPU *c,double a,double b,int integer_flags) {
    uint32_t bits=(isnan(a)||isnan(b))?0x45u:a<b?1u:a==b?0x40u:0u;
    if(integer_flags)c->flags=(c->flags&~0x8d5u)|bits;
    else c->fp_status=(c->fp_status&~0x4500u)|(uint16_t)(bits<<8);
}
enum {ENGINE_FP_ADD,ENGINE_FP_SUB,ENGINE_FP_MUL,ENGINE_FP_DIV,
      ENGINE_FP_FCHS,ENGINE_FP_FABS,ENGINE_FP_FSQRT,ENGINE_FP_FRNDINT};
static inline void engine_fp_require_precision(EngineCPU *c) {
    (void)c; /* All x87 arithmetic is evaluated in binary64. */
}
static inline double engine_fp_arithmetic(EngineCPU *c,int operation,double a,double b) {
    engine_fp_require_precision(c);
    if(engine_fp_nearest(c))
        switch(operation){case ENGINE_FP_ADD:return a+b;case ENGINE_FP_SUB:return a-b;
            case ENGINE_FP_MUL:return a*b;case ENGINE_FP_DIV:return a/b;
            default:engine_fail(c,"invalid x87 arithmetic");}
#if HALO_ARM64_FENV_FAST && defined(__aarch64__)
    if((unsigned)operation>ENGINE_FP_DIV)engine_fail(c,"invalid x87 arithmetic");
    return halo_arm64_fp_arithmetic((c->fp_control>>10)&3,(unsigned)operation,a,b);
#else
    fenv_t environment;feholdexcept(&environment);fesetround(engine_fp_round_mode(c));
    volatile double result;
    switch(operation){case ENGINE_FP_ADD:result=a+b;break;
        case ENGINE_FP_SUB:result=a-b;break;case ENGINE_FP_MUL:result=a*b;break;
        case ENGINE_FP_DIV:result=a/b;break;default:engine_fail(c,"invalid x87 arithmetic");}
    fesetenv(&environment);
    return result;
#endif
}
/* Round to an integer in the guest's mode with that mode's own instruction
 * (frintn, frintm, frintp, frintz), none of which reads the host rounding
 * mode. Each is exact, so this equals nearbyint under the guest mode, and it
 * costs a tenth of switching the host mode around nearbyint, which CRT floor
 * (FRNDINT under round-down) and _ftol (FISTP under chop) did on every call. */
static inline double engine_fp_round_integer(EngineCPU *c,double value,int truncate) {
    if(truncate)return trunc(value);
    switch((c->fp_control>>10)&3){
        case 0:return __builtin_roundeven(value);
        case 1:return floor(value);
        case 2:return ceil(value);
        default:return trunc(value);
    }
}
static inline void engine_fp_unary(EngineCPU *c,int operation) {
    double value=engine_fp_read(c,0),result;
    switch(operation){case ENGINE_FP_FCHS:result=-value;break;
        case ENGINE_FP_FABS:result=fabs(value);break;
        case ENGINE_FP_FRNDINT:result=engine_fp_round_integer(c,value,0);break;
        case ENGINE_FP_FSQRT:{engine_fp_require_precision(c);
            if(engine_fp_nearest(c)){result=sqrt(value);break;}
            int old=fegetround();fesetround(engine_fp_round_mode(c));
            result=sqrt(value);fesetround(old);break;}
        default:engine_fail(c,"invalid x87 unary operation");}
    engine_fp_write(c,0,result);
}
static inline void engine_fp_load_integer(EngineCPU *c,uint32_t address,unsigned size) {
    int64_t value;
    if(size==2)value=(int16_t)engine_read_u16(c,address);
    else if(size==4)value=(int32_t)engine_read_u32(c,address);
    else if(size==8){value=(int64_t)engine_read_u64(c,address);
        /* x87 holds every int64 exactly; this double-backed stack does not.
         * Values the double represents exactly (including the 0x8000000000000000
         * indefinite that the CRT float-to-int64 helper reloads) are fine;
         * inexact magnitudes round like the guest would with a 53-bit mantissa. */
        c->fp_status|=0; }
    else engine_fail(c,"invalid FILD size");
    engine_fp_push(c,(double)value);
}
static inline void engine_fp_store_integer(EngineCPU *c,uint32_t address,unsigned size,int truncate) {
    double value=engine_fp_round_integer(c,engine_fp_read(c,0),truncate);
    double upper=size==2?32768.:size==4?2147483648.:9223372036854775808.;
    if(!isfinite(value)||value>=upper||value<-upper){
        c->fp_status|=1;
        if(size==2)engine_write_u16(c,address,0x8000);
        else if(size==4)engine_write_u32(c,address,0x80000000u);
        else if(size==8)engine_write_u64(c,address,UINT64_C(0x8000000000000000));
        else engine_fail(c,"invalid FIST size");
    }else if(size==2)engine_write_u16(c,address,(uint16_t)(int16_t)value);
    else if(size==4)engine_write_u32(c,address,(uint32_t)(int32_t)value);
    else if(size==8)engine_write_u64(c,address,(uint64_t)(int64_t)value);
    else engine_fail(c,"invalid FIST size");
}

/* ---------------------------------------------------------------- */
/* Additions for whole-executable translation.                      */
/* ---------------------------------------------------------------- */
#include <time.h>
#define ENGINE_DEFAULT_FS_BASE 0x7FFDE000u
#define ENGINE_FS_BASE (cpu->fs_base ? cpu->fs_base : ENGINE_DEFAULT_FS_BASE)

static inline void engine_mul1(EngineCPU *c,uint32_t src,unsigned bits,int sign) {
    uint32_t cfof;
    if(bits==8){
        uint16_t r=sign?(uint16_t)(int16_t)((int8_t)(c->gpr[0]&0xffu)*(int8_t)src)
                       :(uint16_t)((c->gpr[0]&0xffu)*(src&0xffu));
        c->gpr[0]=(c->gpr[0]&0xffff0000u)|r;
        cfof=sign?((int16_t)r!=(int8_t)r):((r>>8)!=0);
    }else if(bits==16){
        uint32_t r=sign?(uint32_t)(int32_t)((int16_t)(c->gpr[0]&0xffffu)*(int16_t)src)
                       :(c->gpr[0]&0xffffu)*(src&0xffffu);
        c->gpr[0]=(c->gpr[0]&0xffff0000u)|(r&0xffffu);
        c->gpr[2]=(c->gpr[2]&0xffff0000u)|(r>>16);
        cfof=sign?((int32_t)r!=(int16_t)r):((r>>16)!=0);
    }else{
        uint64_t r=sign?(uint64_t)((int64_t)(int32_t)c->gpr[0]*(int64_t)(int32_t)src)
                       :(uint64_t)c->gpr[0]*src;
        c->gpr[0]=(uint32_t)r;c->gpr[2]=(uint32_t)(r>>32);
        cfof=sign?((int64_t)r!=(int32_t)r):((r>>32)!=0);
    }
    c->flags=(c->flags&~0x801u)|(cfof?0x801u:0u);
}
static inline uint32_t engine_bsf(uint32_t v){return (uint32_t)__builtin_ctz(v);}
static inline uint32_t engine_bsr(uint32_t v){return 31u-(uint32_t)__builtin_clz(v);}
static inline void engine_cpuid(EngineCPU *c) {
    /* A plain "GenuineIntel" family-6 CPU: FPU, TSC, CX8, CMOV, FXSR. No MMX/SSE/SSE2/3DNow!,
       so the game and CRT choose their x87 code paths. */
    switch(c->gpr[0]){
    case 0: c->gpr[0]=1;c->gpr[3]=0x756e6547u;c->gpr[2]=0x49656e69u;c->gpr[1]=0x6c65746eu;break;
    case 1: c->gpr[0]=0x00000686u;c->gpr[3]=0;c->gpr[1]=0;c->gpr[2]=(1u<<0)|(1u<<4)|(1u<<8)|(1u<<15)|(1u<<24);break; /* MMX off (test) */
    default:c->gpr[0]=c->gpr[1]=c->gpr[2]=c->gpr[3]=0;break;
    }
}
static inline void engine_rdtsc(EngineCPU *c) {
    uint64_t t=(uint64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)*3u; /* ~3 GHz tick */
    c->gpr[0]=(uint32_t)t;c->gpr[2]=(uint32_t)(t>>32);
}

/* 80-bit extended values: converted to/from binary64 (arm64 has no native long double). */
static inline double engine_f80_to_double(const uint8_t *p) {
    uint64_t mant;uint16_t se;memcpy(&mant,p,8);memcpy(&se,p+8,2);
    int sign=se>>15,exp=se&0x7fff;
    if(exp==0&&mant==0)return sign?-0.0:0.0;
    if(exp==0x7fff){if((mant<<1)==0)return sign?-INFINITY:INFINITY;return NAN;}
    double v=ldexp((double)mant,exp-16383-63);return sign?-v:v;
}
static inline void engine_double_to_f80(double v,uint8_t *p) {
    uint64_t mant=0;uint16_t se=0;int sign=signbit(v)?1:0;
    if(isnan(v)){se=0x7fff;mant=UINT64_C(0xC000000000000000);}
    else if(isinf(v)){se=0x7fff;mant=UINT64_C(0x8000000000000000);}
    else if(v==0){se=0;mant=0;}
    else{int e;double f=frexp(fabs(v),&e);mant=(uint64_t)ldexp(f,64);se=(uint16_t)(e-1+16383);}
    se|=(uint16_t)(sign<<15);memcpy(p,&mant,8);memcpy(p+8,&se,2);
}
static inline double engine_read_f80(EngineCPU *c,uint32_t a){return engine_f80_to_double(engine_address(c,a,10));}
static inline void engine_write_f80(EngineCPU *c,uint32_t a,double v){engine_double_to_f80(v,engine_write_address(c,a,10));}
static inline double engine_fp_integer_operand(EngineCPU *c,uint32_t a,unsigned size) {
    return size==2?(double)(int16_t)engine_read_u16(c,a):(double)(int32_t)engine_read_u32(c,a);
}
static inline void engine_fp_free(EngineCPU *c,unsigned i) {
    if(i<8)c->fp_valid&=(uint8_t)~(1u<<i);
}
enum {ENGINE_FP_FSIN,ENGINE_FP_FCOS,ENGINE_FP_FSINCOS,ENGINE_FP_FPTAN,ENGINE_FP_FPATAN,
      ENGINE_FP_FYL2X,ENGINE_FP_FYL2XP1,ENGINE_FP_F2XM1,ENGINE_FP_FSCALE,ENGINE_FP_FPREM,
      ENGINE_FP_FPREM1,ENGINE_FP_FXAM,ENGINE_FP_FXTRACT};
static inline void engine_fp_transcendental(EngineCPU *c,int op) {
    double a,b;
    switch(op){
    case ENGINE_FP_FSIN:engine_fp_write(c,0,sin(engine_fp_read(c,0)));c->fp_status&=(uint16_t)~0x0400u;break;
    case ENGINE_FP_FCOS:engine_fp_write(c,0,cos(engine_fp_read(c,0)));c->fp_status&=(uint16_t)~0x0400u;break;
    case ENGINE_FP_FSINCOS:a=engine_fp_read(c,0);engine_fp_write(c,0,sin(a));engine_fp_push(c,cos(a));c->fp_status&=(uint16_t)~0x0400u;break;
    case ENGINE_FP_FPTAN:engine_fp_write(c,0,tan(engine_fp_read(c,0)));engine_fp_push(c,1.0);c->fp_status&=(uint16_t)~0x0400u;break;
    case ENGINE_FP_FPATAN:b=engine_fp_read(c,0);a=engine_fp_read(c,1);engine_fp_write(c,1,atan2(a,b));engine_fp_pop(c);break;
    case ENGINE_FP_FYL2X:b=engine_fp_read(c,0);a=engine_fp_read(c,1);engine_fp_write(c,1,a*log2(b));engine_fp_pop(c);break;
    case ENGINE_FP_FYL2XP1:b=engine_fp_read(c,0);a=engine_fp_read(c,1);engine_fp_write(c,1,a*log2(1.0+b));engine_fp_pop(c);break;
    case ENGINE_FP_F2XM1:engine_fp_write(c,0,exp2(engine_fp_read(c,0))-1.0);break;
    case ENGINE_FP_FSCALE:a=engine_fp_read(c,0);b=trunc(engine_fp_read(c,1));
        if(b>20000)b=20000;if(b<-20000)b=-20000;engine_fp_write(c,0,ldexp(a,(int)b));break;
    case ENGINE_FP_FPREM:case ENGINE_FP_FPREM1:{
        a=engine_fp_read(c,0);b=engine_fp_read(c,1);
        double r=op==ENGINE_FP_FPREM?fmod(a,b):remainder(a,b);
        double q=fabs(trunc(a/b));uint32_t qi=(uint32_t)fmod(q,8.0);
        c->fp_status&=(uint16_t)~0x4700u;
        if(qi&1)c->fp_status|=0x0200;if(qi&2)c->fp_status|=0x0100;if(qi&4)c->fp_status|=0x4000;
        engine_fp_write(c,0,r);break;}
    case ENGINE_FP_FXAM:{
        uint16_t bits;
        if(!(c->fp_valid&1u))bits=0x4100;
        else{a=c->fp[0];
            if(isnan(a))bits=0x0100;else if(isinf(a))bits=0x0500;else if(a==0)bits=0x4000;
            else if(fpclassify(a)==FP_SUBNORMAL)bits=0x4400;else bits=0x0400;
            if(signbit(a))bits|=0x0200;}
        c->fp_status=(uint16_t)((c->fp_status&~0x4700u)|bits);break;}
    case ENGINE_FP_FXTRACT:{a=engine_fp_read(c,0);int e;double f=frexp(a,&e);
        engine_fp_write(c,0,(double)(e-1));engine_fp_push(c,f*2.0);break;}
    default:engine_fail(c,"invalid x87 transcendental");
    }
}
/* 28-byte protected-mode x87 environment. Register contents are separate (FNSAVE/FRSTOR). */
static inline uint16_t engine_fp_tag_word(EngineCPU *c) {
    uint16_t tags=0;
    for(unsigned phys=0;phys<8;phys++){
        unsigned logical=(phys-c->fp_top)&7;uint16_t t;
        if(!(c->fp_valid&(1u<<logical)))t=3;
        else{double v=c->fp[logical];t=(v==0)?1:(isnan(v)||isinf(v)||fpclassify(v)==FP_SUBNORMAL)?2:0;}
        tags|=(uint16_t)(t<<(phys*2));
    }
    return tags;
}
static inline void engine_fp_store_environment(EngineCPU *c,uint32_t a) {
    engine_write_u32(c,a,c->fp_control);engine_write_u32(c,a+4,engine_fp_status(c));
    engine_write_u32(c,a+8,engine_fp_tag_word(c));engine_write_u32(c,a+12,0);
    engine_write_u32(c,a+16,0);engine_write_u32(c,a+20,0);engine_write_u32(c,a+24,0);
}
static inline void engine_fp_load_environment(EngineCPU *c,uint32_t a) {
    engine_fp_control(c,(uint16_t)engine_read_u32(c,a));
    uint32_t status=engine_read_u32(c,a+4),tags=engine_read_u32(c,a+8);
    c->fp_status=(uint16_t)(status&0x47ffu);c->fp_top=(uint8_t)((status>>11)&7);
    c->fp_valid=0;
    for(unsigned phys=0;phys<8;phys++){
        if(((tags>>(phys*2))&3)!=3)c->fp_valid|=(uint8_t)(1u<<((phys-c->fp_top)&7));
    }
}
static inline void engine_fp_save(EngineCPU *c,uint32_t a) {
    engine_fp_store_environment(c,a);
    for(unsigned i=0;i<8;i++)engine_write_f80(c,a+28+i*10,(c->fp_valid&(1u<<i))?c->fp[i]:0.0);
    engine_fp_init(c);
}
static inline void engine_fp_restore(EngineCPU *c,uint32_t a) {
    engine_fp_load_environment(c,a);
    for(unsigned i=0;i<8;i++)c->fp[i]=engine_read_f80(c,a+28+i*10);
}
#endif
