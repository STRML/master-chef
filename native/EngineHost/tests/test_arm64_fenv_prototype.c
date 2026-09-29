#pragma STDC FENV_ACCESS ON
#define HALO_ARM64_FENV_PROTOTYPE 1
#include "../../EngineReuse/engine_arm64_fenv_prototype.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <setjmp.h>
#ifdef HALO_ARM64_FENV_ACTUAL_HEADER
#include "../../EngineReuse/engine_cpu.h"
static uint32_t helper_output;
static void *helper_address(EngineCPU*c,uint32_t a,size_t n){(void)c;(void)a;assert(n==4);return &helper_output;}
static jmp_buf failure_jump;
static void helper_failure(EngineCPU*c,const char*r){(void)c;assert(!strcmp(r,"invalid x87 arithmetic"));longjmp(failure_jump,1);}
#endif
static uint64_t fpcr(void){uint64_t v;__asm__ volatile("mrs %0, fpcr":"=r"(v));return v;}
static uint64_t fpsr(void){uint64_t v;__asm__ volatile("mrs %0, fpsr":"=r"(v));return v;}
static void set_fp_state(uint64_t c,uint64_t s){__asm__ volatile("msr fpcr, %0\n\tmsr fpsr, %1"::"r"(c),"r"(s):"memory");}
static double frombits(uint64_t v){double d;memcpy(&d,&v,8);return d;}
static uint64_t bits(double d){uint64_t v;memcpy(&v,&d,8);return v;}
static uint32_t fbits(float d){uint32_t v;memcpy(&v,&d,4);return v;}
static const int modes[4]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
__attribute__((noinline)) static double reference(unsigned m,unsigned op,double a,double b){
 fenv_t e;feholdexcept(&e);fesetround(modes[m]);volatile double r;
 switch(op){case 0:r=a+b;break;case 1:r=a-b;break;case 2:r=a*b;break;default:r=a/b;}
 fesetenv(&e);return r;
}
__attribute__((noinline)) static float reference32(unsigned m,double a){fenv_t e;feholdexcept(&e);fesetround(modes[m]);volatile float r=(float)a;fesetenv(&e);return r;}
__attribute__((noinline)) static double fast(unsigned m,unsigned op,double a,double b){
#ifdef HALO_ARM64_FENV_ACTUAL_HEADER
EngineCPU cpu={0};cpu.fp_control=m<<10;return engine_fp_arithmetic(&cpu,(int)op,a,b);
#else
return halo_arm64_fp_arithmetic(m,op,a,b);
#endif
}
__attribute__((noinline)) static float fast32(unsigned m,double a){
#ifdef HALO_ARM64_FENV_ACTUAL_HEADER
EngineCPU cpu={0};cpu.fp_control=m<<10;cpu.write_address=helper_address;engine_write_f32(&cpu,0,a);float out;memcpy(&out,&helper_output,4);return out;
#else
return halo_arm64_fp_f32(m,a);
#endif
}
static uint64_t rng=0x123456789abcdef;
static uint64_t random64(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
static unsigned long checks;
static void one(unsigned guest,unsigned op,double a,double b,uint64_t control,uint64_t status){
 set_fp_state(control,status);uint64_t c=fpcr(),s=fpsr();double r=reference(guest,op,a,b);assert(fpcr()==c&&fpsr()==s);
 double q=fast(guest,op,a,b);assert(fpcr()==c&&fpsr()==s);
 if(bits(r)!=bits(q)){fprintf(stderr,"mismatch g%u op%u a%llx b%llx r%llx q%llx\n",guest,op,(unsigned long long)bits(a),(unsigned long long)bits(b),(unsigned long long)bits(r),(unsigned long long)bits(q));abort();}checks++;
 float rf=reference32(guest,a);assert(fpcr()==c&&fpsr()==s);float qf=fast32(guest,a);assert(fpcr()==c&&fpsr()==s);assert(fbits(rf)==fbits(qf));checks++;
}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
 uint64_t original=fpcr(),status=fpsr();
#ifdef HALO_ARM64_FENV_ACTUAL_HEADER
 EngineCPU bad={0};bad.failure=helper_failure;
 if(!setjmp(failure_jump)){(void)engine_fp_arithmetic(&bad,99,1,2);assert(0);}
 assert(fpcr()==original&&fpsr()==status);
#endif
 uint64_t vectors[]={0,0x8000000000000000ULL,1,0x8000000000000001ULL,0xfffffffffffffULL,0x10000000000000ULL,0x3ff0000000000000ULL,0x3ff0000000000001ULL,0x3ca0000000000000ULL,0x7fefffffffffffffULL,0xffefffffffffffffULL,0x7ff0000000000000ULL,0xfff0000000000000ULL,0x7ff8000000001234ULL,0x7ff0000000001234ULL,0xfff8000000009876ULL,0x3ff0000010000000ULL,0x380fffffe0000000ULL};
 for(unsigned host=0;host<4;host++)for(unsigned variant=0;variant<8;variant++){
  /* FZ/DN varied independently; status includes cumulative flags and QC. */
  uint64_t c=(original&~UINT64_C(0x3C09F00))|halo_arm64_guest_round(host)|((uint64_t)(variant&1)<<24)|((uint64_t)((variant>>1)&1)<<25)|((variant&4)?0x9F00:0);
  for(unsigned g=0;g<4;g++)for(unsigned op=0;op<4;op++)for(unsigned a=0;a<18;a++)for(unsigned b=0;b<18;b++)one(g,op,frombits(vectors[a]),frombits(vectors[b]),c,0x0800009f);
  for(unsigned i=0;i<10000;i++)one(i&3,(i>>2)&3,frombits(random64()),frombits(random64()),c,(i&1)?0:0x0800009f);
 }
 set_fp_state(original,status);printf("differential PASS checks=%lu hostFPCR=%llx hostFPSR=%llx\n",checks,(unsigned long long)original,(unsigned long long)status);
 const unsigned n=1000000;volatile double sink=0;
 for(unsigned round=0;round<3;round++){
  double t=seconds();for(unsigned i=0;i<n;i++)sink=reference(i&3,i&3,1.125,0.75);double libc=seconds()-t;
  t=seconds();for(unsigned i=0;i<n;i++)sink=fast(i&3,i&3,1.125,0.75);double asm_time=seconds()-t;
  printf("benchmark n=%u libc=%.6f prototype=%.6f ratio=%.2f sink=%.3f\n",n,libc,asm_time,libc/asm_time,sink);
 }
 double common_start=seconds();for(unsigned i=0;i<n;i++)sink=reference(0,i&3,1.125,0.75);double libc_common=seconds()-common_start;
 common_start=seconds();for(unsigned i=0;i<n;i++)sink=fast(0,i&3,1.125,0.75);double fast_common=seconds()-common_start;
 printf("commonmode0 benchmark libc=%.6f prototype=%.6f ratio=%.2f\n",libc_common,fast_common,libc_common/fast_common);
 double t=seconds();for(unsigned i=0;i<n;i++)sink=reference32(i&3,1.0000000596046448);double libc32=seconds()-t;
 t=seconds();for(unsigned i=0;i<n;i++)sink=fast32(i&3,1.0000000596046448);double fast_time32=seconds()-t;
 printf("f32 benchmark n=%u libc=%.6f prototype=%.6f ratio=%.2f\n",n,libc32,fast_time32,libc32/fast_time32);
 set_fp_state(original|0x9f00,status);uint64_t trap_readback=fpcr()&0x9f00;set_fp_state(original,status);
 printf("hardware trap-enable readback=%llx (attempted9f00)\n",(unsigned long long)trap_readback);
 set_fp_state(original,status);return 0;
}
