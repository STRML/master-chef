/* Real production setter router, block device control and COM methods; no
 * original engine, guest draw calls or Metal context. */
#include "../d3d9.c"
#include <assert.h>
/* This fixture never destroys a live Metal context. */
void mr_destroy(mr_context *context){(void)context;abort();}
/* No resident vertex copy is ever taken here: nothing draws. */
void mr_buffer_release(void *buffer){(void)buffer;abort();}

#include <stdarg.h>
uint8_t *engine_flat_base;
void guest_free(uint32_t p){assert(p>=0x10000&&p<0x100000);}
int guest_page_free(uint32_t p){(void)p;abort();}
static uint32_t allocation=0x10000;
uint32_t guest_alloc(uint32_t size){uint32_t p=allocation;allocation+=(size+15)&~15u;assert(allocation<0x100000);return p;}
uint32_t host_proc_address(const char *dll,const char *name){(void)dll;(void)name;return 0xfe000000;}
_Noreturn void host_exit(int code){(void)code;abort();}
uint64_t host_monotonic_ns(void){return 0;}
void host_log(const char *format,...){(void)format;}
static EngineCPU cpu;
static const uint32_t stack=0x100,output=0x300,data=0x400,return_site=0x005abbad;
static void args(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){cpu.gpr[4]=stack;S32(stack,return_site);S32(stack+4,a);S32(stack+8,b);S32(stack+12,c);S32(stack+16,d);S32(stack+20,e);}
static uint32_t control(D3DObj *dev,int method,uint32_t a,uint32_t b){uint32_t hr=0xdead;args(dev->guest,a,b,0,0);assert(stateblock_device_method(&cpu,dev,method,&hr));return hr;}
static uint32_t record(int method,uint32_t a,uint32_t b,uint32_t c,uint32_t d){uint32_t hr=0xdead;args(the_device->guest,a,b,c,d);assert(stateblock_record_method(&cpu,method,&hr));return hr;}
static uint32_t simple(D3DObj *o,int method,uint32_t a,uint32_t b){int argc=classes[o->kind].methods[method].argc;args(o->guest,a,b,0,0);method_simple(&cpu,o,method);assert(cpu.pc==return_site&&cpu.gpr[4]==stack+4+4*argc);return cpu.gpr[0];}
int main(void){
    engine_flat_base=calloc(1,0x100000);assert(engine_flat_base);setenv("HALO_STATEBLOCK_TRACE","0",1);
    the_device=obj_new(K_DEVICE);D3DObj *decl=obj_new(K_VDECL),*vs=obj_new(K_VSHADER),*ps=obj_new(K_PSHADER),*texture=obj_new(K_TEXTURE),*vb=obj_new(K_VB),*ib=obj_new(K_IB);
    render_defaults(640,480);lighting_defaults();binding_replace(&draw_state.decl,decl->guest);binding_replace(&draw_state.vs,vs->guest);binding_replace(&draw_state.ps,ps->guest);
    d3d9_set_vertex_input(1,0x112);assert(!draw_state.decl&&draw_state.fvf==0x112);
    d3d9_set_vertex_input(0,decl->guest);assert(draw_state.decl==decl->guest&&!draw_state.fvf);
    HostD3DStateSnapshot before;stateblock_snapshot(&before);
    assert(control(the_device,61,output,0)==D3DERR_INVALIDCALL);
    assert(control(the_device,60,0,0)==D3D_OK);HostStateBlock *pending=stateblock_recording;
    assert(control(the_device,60,0,0)==D3DERR_INVALIDCALL&&stateblock_recording==pending);
    assert(control(the_device,61,0,0)==D3DERR_INVALIDCALL&&stateblock_recording==pending);
    /* Actual005ADD56 initialization pattern: decl/null shaders. */
    assert(record(87,decl->guest,0,0,0)==0&&record(92,0,0,0,0)==0&&record(107,0,0,0,0)==0);
    HostD3DStateSnapshot unchanged;stateblock_snapshot(&unchanged);assert(!memcmp(&before,&unchanged,sizeof before));
    assert(control(the_device,61,output,0)==0);D3DObj *block=obj_from_guest(G32(output));assert(block->stateblock&&!stateblock_recording);
    assert(the_device->refs==2&&decl->refs==3);
    assert(simple(block,4,0,0)==0&&vs->refs==3&&ps->refs==3); /* Capture current bindings only. */
    binding_replace(&draw_state.decl,0);binding_replace(&draw_state.vs,0);binding_replace(&draw_state.ps,0);draw_state.world[12]=55;
    assert(simple(block,5,0,0)==0&&draw_state.decl==decl->guest&&draw_state.vs==vs->guest&&draw_state.ps==ps->guest&&draw_state.world[12]==55);
    HostD3DStateSnapshot before_sparse;stateblock_snapshot(&before_sparse);
    assert(control(the_device,60,0,0)==0);assert(simple(block,4,0,0)==D3DERR_INVALIDCALL&&simple(block,5,0,0)==D3DERR_INVALIDCALL);
    float floats[8]={1,2,3,4,5,6,7,8};memcpy(GPTR(data),floats,sizeof floats);
    assert(record(94,2,data,2,0)==0&&record(109,3,data,2,0)==0);
    assert(record(96,4,data,1,0)==0&&record(111,5,data,1,0)==0&&record(98,6,data,1,0)==0&&record(113,7,data,1,0)==0);
    unsigned char mask_before[sizeof(HostD3DStateSnapshot)];memcpy(mask_before,stateblock_recording->mask,sizeof mask_before);
    /* Failed/zero-length ranges don't mutate selection. */
    assert(record(65,2,0xdeadbeef,0,0)==D3DERR_INVALIDCALL&&record(92,texture->guest,0,0,0)==D3DERR_INVALIDCALL);
    assert(record(94,255,data,2,0)==D3DERR_INVALIDCALL&&record(109,223,data,2,0)==D3DERR_INVALIDCALL&&record(98,16,data,1,0)==D3DERR_INVALIDCALL);
    assert(record(94,256,0,0,0)==0&&!memcmp(mask_before,stateblock_recording->mask,sizeof mask_before));
    assert(record(57,7,0,0,0)==0&&record(65,2,texture->guest,0,0)==0&&record(65,3,texture->guest,0,0)==0);
    assert(record(69,2,5,2,0)==0&&record(67,2,1,4,0)==0&&record(100,1,vb->guest,12,24)==0&&record(104,ib->guest,0,0,0)==0);
    assert(record(89,0x112,0,0,0)==0); /* CoupledFVF/decl; block selected null decl. */
    HostD3DMaterial m={.diffuse={1,1,1,1},.power=3};memcpy(GPTR(data),&m,sizeof m);assert(record(49,data,0,0,0)==0);
    HostD3DLight light;default_light(&light);memcpy(GPTR(data),&light,sizeof light);assert(record(51,2,data,0,0)==0&&record(53,2,1,0,0)==0);
    stateblock_snapshot(&unchanged);assert(!memcmp(&before_sparse,&unchanged,sizeof unchanged));
    assert(control(the_device,61,output,0)==0);D3DObj *sparse=obj_from_guest(G32(output));assert(texture->refs==3&&vb->refs==2&&ib->refs==2);
    assert(simple(sparse,5,0,0)==0);assert(draw_state.fvf==0x112&&!draw_state.decl&&draw_state.vs_float[2][0]==1&&draw_state.ps_float[4][3]==8);
    assert(!memcmp(draw_state.vs_int[4],floats,16)&&!memcmp(draw_state.ps_int[5],floats,16));
    assert(!memcmp(&draw_state.vs_bool[6],floats,4)&&!memcmp(&draw_state.ps_bool[7],floats,4));
    assert(!draw_state.vs_int[3][0]&&!draw_state.ps_int[6][0]&&!draw_state.vs_bool[5]&&!draw_state.ps_bool[8]);
    assert(!draw_state.vs_float[1][0]&&!draw_state.ps_float[5][0]&&draw_state.stream[1]==vb->guest&&draw_state.offset[1]==12&&draw_state.stride[1]==24);
    assert(current_material.power==3&&light_slots[2].defined&&light_slots[2].enabled&&!light_slots[3].defined);
    memcpy(mask_before,sparse->stateblock->mask,sizeof mask_before);unsigned char *payload=sparse->stateblock->values;
    draw_state.vs_float[2][0]=77;draw_state.vs_float[1][0]=88;draw_state.world[12]=99;binding_replace(&draw_state.texture[2],0);
    assert(simple(sparse,4,0,0)==0&&texture->refs==3&&!memcmp(mask_before,sparse->stateblock->mask,sizeof mask_before));
    draw_state.vs_float[2][0]=0;draw_state.vs_float[1][0]=66;draw_state.world[12]=100;
    for(unsigned k=0;k<1000;k++){assert(simple(sparse,5,0,0)==0&&simple(sparse,4,0,0)==0);assert(sparse->stateblock->values==payload&&texture->refs==3);}
    assert(draw_state.vs_float[2][0]==77&&draw_state.vs_float[1][0]==66&&draw_state.world[12]==100);
    assert(simple(block,0,0,output)==0&&block->refs==2);assert(simple(block,2,0,0)==1);
    assert(simple(block,3,output,0)==0&&G32(output)==the_device->guest&&the_device->refs==4);stateblock_release(NULL,the_device->guest);
    assert(simple(block,2,0,0)==0&&!block->stateblock&&the_device->refs==2&&decl->refs==1&&vs->refs==2&&ps->refs==2);
    assert(!block->kind&&!block->guest); /* Final Release invalidates the object. */
    assert(simple(sparse,2,0,0)==0&&the_device->refs==1&&texture->refs==2&&vb->refs==2&&ib->refs==2);
    /* Application releases its shader while a custom block retains it. */
    D3DObj *pinned=obj_new(K_VSHADER);assert(control(the_device,60,0,0)==0&&record(92,pinned->guest,0,0,0)==0&&control(the_device,61,output,0)==0);
    D3DObj *pinblock=obj_from_guest(G32(output));assert(pinned->refs==2&&simple(pinned,2,0,0)==1);
    assert(simple(pinblock,5,0,0)==0&&draw_state.vs==pinned->guest);
    assert(simple(pinblock,2,0,0)==0&&pinned->refs==1&&the_device->refs==1); /* Live binding remains an owner. */
    binding_replace(&draw_state.vs,0);assert(!pinned->kind&&!pinned->refs);
    /* PredefinedCreate remains unchanged legacy no-op. */
    assert(control(the_device,59,1,output)==0);D3DObj *legacy=obj_from_guest(G32(output));assert(!legacy->stateblock&&!legacy->stateblock_device);
    assert(simple(legacy,4,0,0)==0&&simple(legacy,5,0,0)==0);
    free(engine_flat_base);puts("PASS: production custom control/setter/COM paths, invalid/nested recording, unchanged live state, sparse F/I/B masks, masked Capture/Apply, binding retains, QI/GetDevice/finalRelease, legacyCreate unchanged.");
}
