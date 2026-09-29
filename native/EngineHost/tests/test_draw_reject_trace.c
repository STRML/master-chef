/* Actual opt-in reject logger and HRESULT path, with no engine/GPU execution. */
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

static unsigned logs;
static char lines[256][2048];
void host_log(const char *format,...){assert(logs<256);va_list ap;va_start(ap,format);vsnprintf(lines[logs++],sizeof lines[0],format,ap);va_end(ap);}
static void reset(const char *flag,const char *from,const char *to){
    memset(&draw_reject_trace,0,sizeof draw_reject_trace);logs=0;
    setenv("HALO_DRAW_REJECT_TRACE",flag,1);if(from)setenv("HALO_DRAW_REJECT_FROM",from,1);else unsetenv("HALO_DRAW_REJECT_FROM");if(to)setenv("HALO_DRAW_REJECT_TO",to,1);else unsetenv("HALO_DRAW_REJECT_TO");
}
int main(void){
    engine_flat_base=calloc(1,0x10000);assert(engine_flat_base);
    static const uint8_t declaration[]={0,0,0,0,2,0,0,0, 0,0,12,0,4,0,10,0, 0,0,16,0,1,0,5,0, 255,0,0,0,17,0,0,0};
    obj_count=2;objs[1]=(D3DObj){.kind=K_VDECL,.guest=0x200,.data=0x400,.size=sizeof declaration};memcpy(GPTR(0x400),declaration,sizeof declaration);
    for(int i=1;i<obj_count;i++)obj_register(&objs[i]);
    render_defaults(640,480);draw_state.decl=0x200;draw_state.rs[27]=1;draw_state.rs[19]=3;draw_state.rs[20]=4;
    reset("0",NULL,NULL);draw_reject_diagnostic("fixed 3D blend factors");assert(!logs&&!draw_reject_trace.details);
    reset("true",NULL,NULL);draw_reject_diagnostic("vertex layout");assert(!logs);
    reset("1","100","102");frames_presented=98;draw_reject_diagnostic("vertex layout");assert(!logs);
    frames_presented=99;draw_reject_trace.draw=(DrawRejectContext){.caller=0x00533850,.primitive=4,.vertices=0x2000,.stride=24,.count=8,.indices=0x3000,.index_count=12,.index_format=101,.base=-2,.serial=11};
    HostD3DDrawState before=draw_state;unsigned draws_before=draw_calls;render_trace_active=0;render_trace_reason[0]=0;
    assert(unsupported_draw("fixed 3D blend factors")==D3DERR_NOTAVAILABLE);
    assert(!memcmp(&before,&draw_state,sizeof before)&&draw_calls==draws_before&&!render_trace_reason[0]);
    assert(strstr(lines[0],"draw=11 reason=fixed 3D blend factors caller=00533850")&&strstr(lines[0],"stride=24 nv=8")&&strstr(lines[0],"blendEn=1 src=3 dst=4 op=1"));
    assert(strstr(lines[1],"stream=0 offset=0 type=2 method=0 usage=0 usageIndex=0"));
    assert(strstr(lines[4],"stream=255"));
    unsigned count=logs;draw_reject_diagnostic("fixed 3D blend factors");assert(logs==count);
    draw_reject_diagnostic("vertex layout");assert(logs==count+1&&strstr(lines[count],"draw=11 reason=vertex layout"));
    assert(draw_reject_trace.declarations==1); /* declaration emitted once */
    for(unsigned i=0;i<100;i++){draw_state.rs[19]=100+i;before=draw_state;draw_reject_diagnostic("fixed 3D blend factors");assert(!memcmp(&before,&draw_state,sizeof before));}
    assert(draw_reject_trace.details==32&&logs==32+4+1);count=logs;
    draw_reject_diagnostic("more");assert(logs==count);
    reset("1","100","102");frames_presented=102;draw_reject_diagnostic("vertex layout");assert(!logs);
    reset("1","bad","102");frames_presented=100;draw_reject_diagnostic("vertex layout");assert(!logs);
    reset("1",NULL,NULL);draw_state.decl=0xdeadbeef;draw_reject_diagnostic("vertex layout");assert(logs==1&&draw_reject_trace.declarations==0);
    free(engine_flat_base);puts("PASS: exact opt-in/window, numeric blend/layout/caller/draw diagnostics, repeat dedup, lifetime caps, invalid handle safety, unchanged draw state/HRESULT/fallback reason.");
}
