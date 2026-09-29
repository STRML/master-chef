/* Execute the existing lifted original FP history/cache instructions, not a
 * reimplementation of history writes. Requires source-only generated files
 * from the audited build24 engine directory; no engine or GPU is started. */
#include "../host.h"
#include "engine_flags.h"
#include <assert.h>
#include "engine_registers.h"
#include "../../build/engine-reuse/desktop-build24-fp-capture/sub_00492430.c"
#include "../../build/engine-reuse/desktop-build24-fp-capture/sub_00493E50.c"
#include "../../build/engine-reuse/desktop-build24-fp-capture/sub_00493740.c"
#include "../../build/engine-reuse/desktop-build24-fp-capture/sub_004D6880.c"
#include "../../build/engine-reuse/desktop-build24-fp-capture/sub_004CB970.c"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#undef eflags

uint8_t *engine_flat_base;
uint32_t engine_trace_lo,engine_trace_hi;
void engine_pc_trace(EngineCPU *cpu){(void)cpu;}
void host_log(const char *format,...){(void)format;}
void host_panorama_reset(void){}
void host_panorama_invalidate(void){}
void host_panorama_abort(void){}
void host_panorama_begin(int pass){(void)pass;}
int host_panorama_camera_moving(void){return 0;}
void host_panorama_set_camera(const float *pose){(void)pose;}
void host_panorama_end(int pass){(void)pass;}
void host_panorama_projection(float x,float y,int vx,int vy,int w,int h,uint32_t caller){(void)x;(void)y;(void)vx;(void)vy;(void)w;(void)h;(void)caller;}
void host_panorama_ui(int active){(void)active;}
void host_panorama_viewmodel(int active){(void)active;}
static void ret_to_caller(EngineCPU *cpu){cpu->pc=engine_pop(cpu,4);}
#define OARG(i) G32(cpu->gpr[4]+4u+4u*(i))
/* This boundary fixture exercises the unpaced budget target. */
float host_frame_pacer_budget_target(float fallback) { return fallback; }
#include "../panorama_hooks.inc"
static unsigned builds;static int escape_builder;
void engine_dispatch(EngineCPU *cpu,uint32_t address){
    cpu->pc=address;
    if(address==0x00492430u){assert(host_panorama_dispatch(cpu,address)==0);if(escape_builder){cpu->pc=0xDEADu;return;}sub_00492430(cpu);return;}
    if(address==0x00493E50u){sub_00493E50(cpu);return;}
    if(address==0x00493740u){sub_00493740(cpu);return;}
    if(address==0x004D6880u){builds++;sub_004D6880(cpu);return;}
    if(address==0x004CB970u){sub_004CB970(cpu);return;}
    // Only activation/weapon-validity queries are stubbed. Empty animation
    // node hierarchy avoids assets while original constructor still executes.
    if(address==0x00445AC0u){cpu->gpr[0]=0;ret_to_caller(cpu);return;}
    if(address==0x00472740u){cpu->gpr[0]=UINT32_MAX;ret_to_caller(cpu);return;}
    if(address==0x004F6EC0u){cpu->gpr[0]=1;ret_to_caller(cpu);return;}
    assert(!"unexpected original call");
}
static float getf(uint32_t a){float f;memcpy(&f,GPTR(a),4);return f;}
static void setup(void){
    memset(engine_flat_base,0,0x900000);builds=0;escape_builder=0;
    S32(0x006B2D98u,0x30000);S32(0x30004,1);S32(0x30008,0);S8(0x30000,1);S8(0x30050,1);
    S32(0x008603B0u,0x40000);S32(0x40034,0x41000);S32(0x41008,0x42000);S32(0x42000,0);
    S32(0x0087BC14u,0x43000);S32(0x43014,0x44000);S32(0x44000+0x478,1);S32(0x43034,0x45000);
    panorama_nested=1;panorama_renderer_count=1;panorama_fp_cache_built=0;panorama_fp_cache_nested=0;
    float central[9]={10,20,30,1,0,0,0,0,1};memcpy(panorama_fp_central_pose[0],central,36);
}
static void pass(float angle){
    float pose[9]={10,20,30,cosf(angle),sinf(angle),0,0,0,1};memcpy(GPTR(0x007C3114u),pose,36);
    unsigned char view_before[0x18C];memset(GPTR(0x007C127Cu),0xB7,sizeof view_before);memcpy(view_before,GPTR(0x007C127Cu),sizeof view_before);
    EngineCPU cpu={0};cpu.gpr[4]=0x10000;cpu.gpr[3]=0xBAD123;cpu.fp_control=0x037f;S32(0x10000,0x0050C09Cu);
    if(!host_panorama_dispatch(&cpu,0x00492430u))engine_dispatch(&cpu,0x00492430u);
    assert(cpu.pc==0x0050C09Cu && cpu.gpr[4]==0x10004 && cpu.gpr[3]==0xBAD123);
    assert(!memcmp(GPTR(0x007C3114u),pose,36));
    assert(!memcmp(GPTR(0x007C127Cu),view_before,sizeof view_before));
}
int main(void){
    engine_flat_base=calloc(0x900000,1);assert(engine_flat_base);
    setup();panorama_world_fp=0;pass(-1.0471975512f);pass(1.0471975512f);pass(0);
    assert(builds==3&&getf(0x30060)==0&&fabsf(getf(0x30068)-1.0471975512f)<1e-6f);
    setup();panorama_world_fp=1;pass(-1.0471975512f);pass(1.0471975512f);pass(0);
    assert(builds==1&&getf(0x30060)==0&&getf(0x30068)==0&&getf(0x30064)==0&&getf(0x3006C)==0);
    assert(getf(0x30070)==10&&getf(0x3007C)==0); // One genuine previous→current update.
    panorama_fp_cache_built=0;pass(-1.0471975512f);assert(builds==2&&getf(0x3007C)==10);
    setup();panorama_world_fp=1;escape_builder=1;float pose[9]={10,20,30,0,1,0,0,0,1};memcpy(GPTR(0x007C3114u),pose,36);
    EngineCPU cpu={0};cpu.gpr[4]=0x10000;S32(0x10000,0x0050C09Cu);
    assert(host_panorama_dispatch(&cpu,0x00492430u)==1&&cpu.pc==0xDEADu);
    assert(!memcmp(GPTR(0x007C3114u),pose,36)&&!panorama_fp_cache_built&&!panorama_fp_cache_nested);
    // Exact callsite guard: never suppress a different original caller.
    S32(0x10000,0x401234);assert(host_panorama_dispatch(&cpu,0x00492430u)==0);
    free(engine_flat_base);
    puts("PASS: lifted original FP cache/history exposes three-pass yaw contamination; opt-in hook builds central once, retains previous history, restores yawed view/pose and escaped state.");
}
