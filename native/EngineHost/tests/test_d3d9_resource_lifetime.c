/* Production D3D ownership plus production heap/page allocators. No game or GPU.
 * clang -O2 -g -D_DARWIN_C_SOURCE -DENGINE_FLAT_MEMORY=1 -I../EngineReuse
 * tests/test_d3d9_resource_lifetime.c -Wl,-dead_strip -lm -o /tmp/d3d_lifetime
 * Run a second build with -fsanitize=address,undefined -fno-omit-frame-pointer. */
#include "../host.c"
/* Proc dispatch is outside the lifetime fixture; the vtable storage is real. */
static uint32_t lifetime_proc_address(const char *dll,const char *name){(void)dll;(void)name;return 0xfe000000;}
#define host_proc_address lifetime_proc_address
#include "../d3d9.c"
#undef host_proc_address
#include <assert.h>
/* This fixture never destroys a live Metal context. */
void mr_destroy(mr_context *context){(void)context;abort();}
/* No resident vertex copy is ever taken here: nothing draws. */
void mr_buffer_release(void *buffer){(void)buffer;abort();}

static EngineCPU cpu;
static const uint32_t stack=0x1000,output=0x2000,return_site=0x005abbad;
static int resize_failure;
int mr_resize(mr_context *c,int w,int h){(void)c;(void)w;(void)h;return resize_failure;}
static void args(uint32_t a,uint32_t b,uint32_t c,uint32_t d,uint32_t e){cpu.gpr[4]=stack;S32(stack,return_site);S32(stack+4,a);S32(stack+8,b);S32(stack+12,c);S32(stack+16,d);S32(stack+20,e);}
static uint32_t bind(int method,uint32_t a,uint32_t b,uint32_t c,uint32_t d){uint32_t hr=~0u;args(the_device->guest,a,b,c,d);assert(resource_binding_method(&cpu,method,&hr));return hr;}
static uint32_t control(int method,uint32_t a,uint32_t b){uint32_t hr=~0u;args(the_device->guest,a,b,0,0);assert(stateblock_device_method(&cpu,the_device,method,&hr));return hr;}
static uint32_t record(int method,uint32_t a,uint32_t b,uint32_t c,uint32_t d){uint32_t hr=~0u;args(the_device->guest,a,b,c,d);assert(stateblock_record_method(&cpu,method,&hr));return hr;}
static uint32_t call(D3DObj *o,int method,uint32_t a,uint32_t b,uint32_t c){int kind=o->kind,argc=classes[kind].methods[method].argc;args(o->guest,a,b,c,0);if(kind==K_VB||kind==K_IB)method_buffer(&cpu,o,method);else method_simple(&cpu,o,method);assert(cpu.pc==return_site&&cpu.gpr[4]==stack+4+4*argc);return cpu.gpr[0];}
static D3DObj *buffer(int kind,uint32_t size){assert(create_buffer(the_device,kind,size,0,kind==K_VB?0x112:101,1,output)==0);D3DObj *b=obj_from_guest(G32(output));assert(b&&b->refs==1);return b;}
static void assert_page_free(uint32_t data){assert(!page_allocations[(data-PAGE_START)/0x10000]);}
int main(void){
    engine_flat_base=mmap(NULL,GUEST_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(engine_flat_base!=MAP_FAILED);
    setenv("HALO_STATEBLOCK_TRACE","0",1);the_device=obj_new(K_DEVICE);render_defaults(640,480);
    assert(create_buffer(the_device,K_IB,64,0,999,1,output)==D3DERR_INVALIDCALL);
    assert(create_buffer(the_device,K_VB,0,0,0,1,output)==D3DERR_INVALIDCALL);
    D3DObj *vb=buffer(K_VB,70000),*ib=buffer(K_IB,128);uint32_t vg=vb->guest,ig=ib->guest,vp=vb->data,ip=ib->data;
    assert(the_device->refs==3);
    assert(bind(100,0,ig,0,16)==D3DERR_INVALIDCALL&&bind(104,vg,0,0,0)==D3DERR_INVALIDCALL);
    assert(bind(100,16,vg,0,16)==D3DERR_INVALIDCALL&&bind(100,0,0xdeadbeef,0,16)==D3DERR_INVALIDCALL);
    assert(bind(100,0,vg,12,24)==0&&bind(100,0,vg,12,24)==0&&vb->refs==2);
    assert(bind(100,3,vg,0,16)==0&&vb->refs==3&&bind(104,ig,0,0,0)==0&&ib->refs==2);
    assert(call(vb,0,0,output,0)==0&&G32(output)==vg&&vb->refs==4);assert(call(vb,2,0,0,0)==3);
    assert(call(vb,3,output,0,0)==0&&G32(output)==the_device->guest&&the_device->refs==4);obj_release(the_device);
    assert(bind(101,0,output,output+4,output+8)==0&&G32(output)==vg&&G32(output+4)==12&&G32(output+8)==24&&vb->refs==4);assert(call(vb,2,0,0,0)==3);
    assert(bind(101,0,0,output+4,output+8)==D3DERR_INVALIDCALL&&vb->refs==3);
    assert(bind(105,output,0,0,0)==0&&G32(output)==ig&&ib->refs==3);assert(call(ib,2,0,0,0)==2);
    assert(call(vb,11,69990,11,output)==D3DERR_INVALIDCALL&&!vb->locked);
    assert(call(vb,11,69990,10,output)==0&&G32(output)==vp+69990&&vb->locked);
    assert(call(vb,11,0,1,output)==0&&vb->locked==2&&call(vb,12,0,0,0)==0&&call(vb,12,0,0,0)==0&&call(vb,12,0,0,0)==D3DERR_INVALIDCALL);
    /* Recording owns only selected fields, even before EndStateBlock. */
    assert(control(60,0,0)==0&&record(100,1,vg,4,24)==0&&record(104,ig,0,0,0)==0);
    assert(vb->refs==4&&ib->refs==3&&!draw_state.stream[1]);assert(call(vb,2,0,0,0)==3&&call(ib,2,0,0,0)==2);
    assert(control(61,output,0)==0);D3DObj *sb=obj_from_guest(G32(output));
    assert(bind(100,0,0,0,0)==0&&bind(100,3,0,0,0)==0&&bind(104,0,0,0,0)==0&&vb->refs==1&&ib->refs==1);
    assert(call(sb,5,0,0,0)==0&&draw_state.stream[1]==vg&&draw_state.indices==ig&&vb->refs==2&&ib->refs==2);
    for(unsigned i=0;i<1000;i++){assert(call(sb,4,0,0,0)==0&&call(sb,5,0,0,0)==0&&vb->refs==2&&ib->refs==2);}
    assert(call(sb,2,0,0,0)==0&&vb->refs==1&&ib->refs==1);
    /* Reset failure preserves owners; success releases the last live bindings. */
    the_device->data=make_surface(64,64,22,1);the_device->level_surface[0]=make_surface(64,64,75,2);
    obj_from_guest(the_device->data)->renderer=(mr_context *)(uintptr_t)1;
    uint32_t pp=0x3000;S32(pp,64);S32(pp+4,64);resize_failure=1;
    assert(reset_device(the_device,pp)==D3DERR_NOTAVAILABLE&&vb->refs==1&&ib->refs==1);
    resize_failure=0;assert(reset_device(the_device,pp)==0&&!obj_find(vg)&&!obj_find(ig));assert_page_free(vp);assert_page_free(ip);assert(the_device->refs==1);
    /* A forged/copy descriptor cannot redirect lookup to a recycled object slot. */
    vb=buffer(K_VB,64);uint32_t forged=0x4000;memcpy(GPTR(forged),GPTR(vb->guest),16);assert(!obj_from_guest(forged));
    uint32_t generation=G32(vb->guest+8);S32(vb->guest+8,generation+1);assert(!obj_from_guest(vb->guest));S32(vb->guest+8,generation);assert(obj_from_guest(vb->guest)==vb);
    vp=vb->data;vg=vb->guest;uint32_t slot=G32(vg+4);assert(call(vb,2,0,0,0)==0&&!obj_from_guest(vg));assert_page_free(vp);
    ib=buffer(K_IB,64);assert(G32(ib->guest+4)==slot&&ib->generation!=generation&&call(ib,2,0,0,0)==0);
    /* Capture replacing the last block owner releases the former buffer. */
    vb=buffer(K_VB,64);vp=vb->data;vg=vb->guest;assert(control(60,0,0)==0&&record(100,2,vg,0,16)==0&&control(61,output,0)==0);sb=obj_from_guest(G32(output));
    assert(call(vb,2,0,0,0)==1&&call(sb,4,0,0,0)==0&&!obj_find(vg));assert_page_free(vp);assert(call(sb,2,0,0,0)==0);
    /* Shader/declaration storage obeys the same application/binding/Get owners. */
    const int kinds[]={K_VDECL,K_VSHADER,K_PSHADER},setters[]={87,92,107},getters[]={88,93,108};
    for(unsigned i=0;i<3;i++){D3DObj *r=obj_new(kinds[i]);r->data=guest_alloc(64);r->size=64;obj_attach_device(r,the_device);uint32_t g=r->guest,bytes=r->data;
        assert(bind(setters[i],g,0,0,0)==0&&r->refs==2&&call(r,2,0,0,0)==1);assert(bind(getters[i],output,0,0,0)==0&&G32(output)==g&&r->refs==2);assert(bind(setters[i],0,0,0,0)==0&&r->refs==1);assert(call(r,2,0,0,0)==0&&!obj_find(g)&&guest_alloc_size(bytes)==0);}
    D3DObj *fvf_decl=obj_new(K_VDECL);uint32_t fvf_guest=fvf_decl->guest;
    assert(bind(87,fvf_guest,0,0,0)==0&&obj_release(fvf_decl)==1);
    assert(bind(89,0x112,0,0,0)==0&&draw_state.fvf==0x112&&!draw_state.decl&&!obj_find(fvf_guest));
    /* More than 65536 objects and >13 GiB page turnover with a fixed live peak. */
    int count_before=obj_count;uint32_t heap_before=heap_ptr,page_before=page_top;
    for(unsigned n=0;n<70000;n++){
        vb=buffer(K_VB,65537);ib=buffer(K_IB,1024);uint32_t vd=vb->data,id=ib->data;S32(vd,n);S32(id,~n);
        assert(bind(100,0,vb->guest,0,16)==0&&bind(104,ib->guest,0,0,0)==0);assert(call(vb,2,0,0,0)==1&&call(ib,2,0,0,0)==1);
        assert(G32(vd)==n&&G32(id)==~n);binding_replace(&draw_state.stream[0],0);binding_replace(&draw_state.indices,0);assert_page_free(vd);assert_page_free(id);
    }
    assert(obj_count<=count_before+2&&heap_ptr<=heap_before+64&&page_top<=page_before+3*65536&&the_device->refs==1);
    printf("PASS: production buffer COM/Lock/binding/Get/block/reset/shader ownership, identity/recycled slots, 140000 buffers, %llu bytes page turnover; slots=%d heap-high=%08x page-high=%08x.\n",70000ull*3*65536,obj_count,heap_ptr,page_top);
    uint32_t handles[9000];unsigned collisions=0;
    for(unsigned n=0;n<9000;n++){D3DObj *r=obj_new(K_VB);handles[n]=r->guest;collisions+=r->hash_next!=0;}
    assert(collisions);
    for(unsigned n=0;n<9000;n+=2)assert(obj_release(obj_find(handles[n]))==0&&!obj_find(handles[n]));
    for(unsigned n=1;n<9000;n+=2)assert(obj_find(handles[n])&&obj_release(obj_find(handles[n]))==0);
    for(unsigned n=0;n<9000;n++)assert(!obj_find(handles[n]));
    printf("PASS: native descriptor hash collision/removal stress; 9000 live descriptors, %u collisions.\n",collisions);
    munmap(engine_flat_base,GUEST_SIZE);return 0;
}
