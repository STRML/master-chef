/* Exercise production diagnostics without an engine or Metal context. */
#include "../d3d9.c"
#include <assert.h>
/* This fixture never destroys a live Metal context. */
void mr_destroy(mr_context *context){(void)context;abort();}
/* No resident vertex copy is ever taken here: nothing draws. */
void mr_buffer_release(void *buffer){(void)buffer;abort();}

#include <stdarg.h>

uint8_t *engine_flat_base;
/* This fixture never owns reclaimable guest allocations. */
void guest_free(uint32_t p){(void)p;abort();}
int guest_page_free(uint32_t p){(void)p;abort();}

static uint64_t test_now;
static unsigned log_count;
static char log_lines[64][768];
uint64_t host_monotonic_ns(void){return test_now;}
void host_log(const char *format,...){
    assert(log_count<64);va_list ap;va_start(ap,format);
    vsnprintf(log_lines[log_count++],sizeof log_lines[0],format,ap);va_end(ap);
}
static void reset_trace(const char *flag){
    assert(setenv("HALO_STATEBLOCK_TRACE",flag,1)==0);
    memset(&stateblock_trace,0,sizeof stateblock_trace);log_count=0;test_now=0;
}
int main(void){
    engine_flat_base=calloc(1,4096);assert(engine_flat_base);
    EngineCPU cpu={0};cpu.gpr[4]=0x100;S32(cpu.gpr[4],0x005abbadu);
    render_defaults(640,480);draw_state.vs=0x1234;draw_state.decl=0x5678;
    draw_state.stream[0]=0x9000;draw_state.stride[0]=32;
    __typeof__(draw_state) before=draw_state;EngineCPU cpu_before=cpu;

    reset_trace("0");stateblock_trace_event(&cpu,SB_TRACE_APPLY,0xa000,0);stateblock_trace_poll();
    assert(!log_count&&!stateblock_trace.counts[SB_TRACE_APPLY]);
    reset_trace("true");stateblock_trace_event(&cpu,SB_TRACE_APPLY,0xa000,0);
    assert(!log_count); /* Exact opt-in, no accidental presence-only enable. */

    reset_trace("1");stateblock_trace_poll();
    assert(log_count==2&&strstr(log_lines[1],"apply=0"));
    for(unsigned a=0;a<SB_TRACE_ACTIONS;a++)stateblock_trace_event(&cpu,a,0xa000,a==SB_TRACE_CREATE?1:0);
    assert(strstr(log_lines[6],"apply frame=1 caller=005abbad block=0000a000"));
    assert(strstr(log_lines[6],"vs=00001234 ps=00000000 decl=00005678"));
    char baseline[768];strcpy(baseline,log_lines[6]);
    draw_state.world[12]=3.f;draw_state.vs_float[1][2]=9.f;draw_state.stride[0]=24;
    stateblock_trace_event(&cpu,SB_TRACE_APPLY,0xa000,0);
    const char *labels[]={"bindings=","matrices=","float-constants="};
    for(unsigned i=0;i<3;i++){
        const char *old=strstr(baseline,labels[i]),*changed=strstr(log_lines[7],labels[i]);
        assert(old&&changed&&strncmp(old+strlen(labels[i]),changed+strlen(labels[i]),16)!=0);
    }
    draw_state=before;
    for(unsigned i=0;i<1000;i++)stateblock_trace_event(&cpu,SB_TRACE_APPLY,0xa000,0);
    assert(stateblock_trace.details==32&&stateblock_trace.omitted==974);
    assert(stateblock_trace.counts[SB_TRACE_APPLY]==1002&&log_count==34);
    test_now=999999999;stateblock_trace_poll();assert(log_count==34);
    test_now=1000000000;frames_presented=12;stateblock_trace_poll();
    assert(log_count==35&&strstr(log_lines[34],"frame=12")&&strstr(log_lines[34],"apply=1002")&&strstr(log_lines[34],"detail-omitted=974"));
    stateblock_trace_poll();assert(log_count==35);
    assert(memcmp(&before,&draw_state,sizeof before)==0&&memcmp(&cpu_before,&cpu,sizeof cpu)==0);
    assert(G32(cpu.gpr[4])==0x005abbadu);

    /* Enter the real success-returning Capture/Apply shim too: diagnostics
     * leave the original stdcall result/stack and no-op render semantics intact. */
    reset_trace("1");D3DObj block={.kind=K_STATEBLOCK,.guest=0xa000,.refs=1};
    for(int method=4;method<=5;method++){
        cpu=cpu_before;S32(cpu.gpr[4]+4,block.guest);
        method_simple(&cpu,&block,method);
        assert(cpu.pc==0x005abbadu&&cpu.gpr[0]==D3D_OK&&cpu.gpr[4]==cpu_before.gpr[4]+8);
        assert(memcmp(&before,&draw_state,sizeof before)==0);
    }
    assert(stateblock_trace.counts[SB_TRACE_CAPTURE]==1&&stateblock_trace.counts[SB_TRACE_APPLY]==1);
    assert(strstr(log_lines[1],"capture")&&strstr(log_lines[2],"apply"));
    free(engine_flat_base);puts("PASS: opt-in stateblock diagnostics preserve CPU/draw state, hash changes, cap detail, count omitted events, and rate-limit zero/nonzero summaries.");
    return 0;
}
