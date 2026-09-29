#include "../host.h"
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>
uint8_t *engine_flat_base;
static unsigned calls,logs;
static char last[2048];
void host_log(const char *format,...) { va_list ap;va_start(ap,format);vsnprintf(last,sizeof last,format,ap);va_end(ap);logs++; }
#include "../audio_ownership_trace.inc"
void engine_dispatch(EngineCPU *cpu,uint32_t address) {
    assert(!host_audio_ownership_dispatch(cpu,address));calls++;
    if(address==0x00442550u)cpu->gpr[0]=0xE5640000;
    if(address==0x00544120u){S32(0x200004,3);S32(0x10001C,UINT32_MAX);}
    cpu->pc=G32(cpu->gpr[4]);cpu->gpr[4]+=4;cpu->instruction_count+=7;
}
int main(void) {
    engine_flat_base=calloc(1,0x900000);assert(engine_flat_base);
    EngineCPU cpu={0};cpu.gpr[4]=0x11000;S32(0x11000,0x499417);S32(0x11004,0x66A044);
    EngineCPU saved=cpu;
    unsetenv("HALO_AUDIO_OWNERSHIP_TRACE");assert(!host_audio_ownership_dispatch(&cpu,0x442550));assert(!memcmp(&cpu,&saved,sizeof cpu));assert(!logs&&!calls);
    setenv("HALO_AUDIO_OWNERSHIP_TRACE","1",1);audio_owner_enabled=-1;
    S8(0x6A8150,1);S32(0x87BC14,0x12000);S32(0x6A8954,0x13000);S32(0x1300C,1);
    S32(0x1200C,0xE5640000);S32(0x12014,0x100000);S32(0x12010,0x14000);memcpy(GPTR(0x14000),"sound\\music\\title1\\title1",26);
    S32(0x10001C,0xE5640000);S32(0x7461A0,0x15000);S16(0x15020,1024);S32(0x15034,0x200000);S16(0x200000,0xE564);S32(0x200004,0x11);
    unsigned char *before=malloc(0x900000);memcpy(before,engine_flat_base,0x900000);
    assert(host_audio_ownership_dispatch(&cpu,0x442550));assert(calls==1);assert(cpu.gpr[0]==0xE5640000&&cpu.gpr[4]==0x11004&&cpu.pc==0x499417&&cpu.instruction_count==7);assert(!memcmp(before,engine_flat_base,0x900000));assert(strstr(last,"flags=00000011"));
    cpu.gpr[4]=0x11000;assert(host_audio_ownership_dispatch(&cpu,0x544120));assert(calls==2);assert(strstr(last,"definitionRecord=ffffffff record=e5640000"));assert(strstr(last,"flags=00000003"));
    S16(0x200000,0x9999);audio_owner_snapshot(&cpu,0,0,"stale",0xE5640000,0xE5640000);assert(strstr(last,"flags=ffffffff"));
    audio_owner_reports=512;saved=cpu;assert(!host_audio_ownership_dispatch(&cpu,0x544120));assert(!memcmp(&saved,&cpu,sizeof cpu));assert(calls==2);
    free(before);free(engine_flat_base);puts("audio ownership trace: PASS");
}
