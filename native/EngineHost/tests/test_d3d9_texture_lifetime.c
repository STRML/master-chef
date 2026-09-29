/* Production texture/surface ownership and real guest allocators. Metal contexts
 * are explicit counted doubles here; real GPU retention is tested separately. */
#include "../host.c"
static uint32_t lifetime_proc_address(const char *dll,const char *name){(void)dll;(void)name;return 0xfe000000;}
#define host_proc_address lifetime_proc_address
#include "../d3d9.c"
#undef host_proc_address
#include <assert.h>

static EngineCPU cpu;
static const uint32_t stack=0x1000,out=0x2000,desc=0x2100,iid=0x2200,return_site=0x005abbad;
static unsigned destroys,reads,writes,creates;
static uint32_t destroy_expected_data;
int mr_resize(mr_context *c,int w,int h){(void)c;(void)w;(void)h;return 0;}
mr_context *mr_create(int w,int h){assert(w&&h);return (mr_context *)(uintptr_t)(++creates+1);}
int mr_write_framebuffer(mr_context *c,const void *data,size_t size){assert(c&&data&&size);writes++;return 0;}
int mr_read_framebuffer(mr_context *c,void *data,size_t size){assert(c&&data&&size);memset(data,0x37,size);reads++;return 0;}
void mr_destroy(mr_context *c){assert(c);if(destroy_expected_data)assert(page_allocations[(destroy_expected_data-PAGE_START)/0x10000]);destroys++;}
/* No resident vertex copy is ever taken here: nothing draws. */
void mr_buffer_release(void *buffer){(void)buffer;abort();}
static void args(uint32_t object,uint32_t a,uint32_t b,uint32_t c,uint32_t d){
    cpu.gpr[4]=stack;S32(stack,return_site);S32(stack+4,object);S32(stack+8,a);S32(stack+12,b);S32(stack+16,c);S32(stack+20,d);
}
static uint32_t call(D3DObj *o,int method,uint32_t a,uint32_t b,uint32_t c){
    int kind=o->kind,argc=classes[kind].methods[method].argc;args(o->guest,a,b,c,0);
    if(kind==K_SURFACE)method_surface(&cpu,o,method);else if(kind==K_VOLUME)method_volume(&cpu,o,method);
    else if(kind==K_STATEBLOCK)method_simple(&cpu,o,method);else method_texture(&cpu,o,method);
    assert(cpu.pc==return_site&&cpu.gpr[4]==stack+4+4*argc);return cpu.gpr[0];
}
static uint32_t target(int method,uint32_t a,uint32_t b,uint32_t c,uint32_t d){uint32_t hr=~0u;args(the_device->guest,a,b,c,d);assert(target_binding_method(&cpu,the_device,method,&hr));return hr;}
static uint32_t bind(int method,uint32_t a,uint32_t b){uint32_t hr=~0u;args(the_device->guest,a,b,0,0);assert(resource_binding_method(&cpu,method,&hr));return hr;}
static uint32_t control(int method,uint32_t a){uint32_t hr=~0u;args(the_device->guest,a,0,0,0);assert(stateblock_device_method(&cpu,the_device,method,&hr));return hr;}
static D3DObj *texture(int kind,uint32_t levels,uint32_t usage){
    assert(create_texture(the_device,kind,32,32,kind==K_VOLTEX?8:1,levels,usage,22,1,out)==0);
    D3DObj *t=obj_find(G32(out));assert(t&&t->refs==1);return t;
}
static D3DObj *alias(D3DObj *t,uint32_t face,uint32_t level){
    assert(call(t,18,t->kind==K_CUBETEX?face:level,t->kind==K_CUBETEX?level:out,t->kind==K_CUBETEX?out:0)==0);
    D3DObj *s=obj_find(G32(out));assert(s&&s->owner==t->guest&&s->refs>0);return s;
}
static void freed(uint32_t data){assert(!page_allocations[(data-PAGE_START)/0x10000]);}
static void baseline(unsigned device_refs){assert((unsigned)the_device->refs==device_refs&&!render_surface_count);}
int main(void){
    engine_flat_base=mmap(NULL,GUEST_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(engine_flat_base!=MAP_FAILED);
    setenv("HALO_STATEBLOCK_TRACE","0",1);the_device=obj_new(K_DEVICE);render_defaults(64,64);
    assert(create_texture(the_device,K_TEXTURE,0,32,1,1,0,22,1,out)==D3DERR_INVALIDCALL);
    assert(create_texture(the_device,K_TEXTURE,32,32,1,7,0,22,1,out)==D3DERR_INVALIDCALL);
    assert(create_texture(the_device,K_VOLTEX,8192,8192,512,1,0,22,1,out)==D3DERR_INVALIDCALL);
    assert(create_surface(the_device,32,32,22,0,1,0)==D3DERR_INVALIDCALL);
    D3DObj *t=texture(K_TEXTURE,0,0);uint32_t tg=t->guest;assert(t->levels==6&&the_device->refs==2);
    assert(call(t,0,0,out,0)==0&&t->refs==2&&call(t,2,0,0,0)==1);
    assert(call(t,3,out,0,0)==0&&G32(out)==the_device->guest&&the_device->refs==3);obj_release(the_device);
    assert(call(t,18,6,out,0)==D3DERR_INVALIDCALL&&call(t,18,0,0,0)==D3DERR_INVALIDCALL&&t->refs==1);
    D3DObj *s=alias(t,0,0);uint32_t sg=s->guest,data=s->data;assert(t->refs==2&&s->refs==1);
    assert(alias(t,0,0)==s&&s->refs==2&&t->refs==2);
    assert(call(s,0,0,out,0)==0&&s->refs==3&&call(s,2,0,0,0)==2);
    assert(call(s,3,out,0,0)==0&&the_device->refs==3);obj_release(the_device);
    assert(call(t,2,0,0,0)==1&&obj_find(tg)); /* child keeps parent/data alive */
    assert(call(s,2,0,0,0)==1&&obj_find(tg));
    const uint32_t texture_iid[4]={0x85c31227,0x4f003de5,0x1af13a9b,0xb5188cc3};memcpy(GPTR(iid),texture_iid,16);
    assert(call(s,11,iid,out,0)==0&&G32(out)==tg&&t->refs==2);
    S32(iid,123);assert(call(s,11,iid,out,0)==0x80004002u&&!G32(out)&&t->refs==2);memcpy(GPTR(iid),texture_iid,16);
    assert(call(s,2,0,0,0)==0&&s->refs==0&&obj_find(sg)&&t->refs==1);
    assert(alias(t,0,0)==s&&s->refs==1&&t->refs==2);assert(call(s,2,0,0,0)==0&&t->refs==1);
    assert(call(t,2,0,0,0)==0&&!obj_find(tg)&&!obj_find(sg));freed(data);baseline(1);

    /* Every face x mip has distinct identity, dimensions and data. The previous
     * face*2+(level&1) map aliased mip 0 with mip 2 and every other level. */
    t=texture(K_CUBETEX,0,0);uint32_t aliases[36],pages[36];assert(t->levels==6);
    assert(call(t,18,6,0,out)==D3DERR_INVALIDCALL&&call(t,18,0,6,out)==D3DERR_INVALIDCALL);
    for(unsigned face=0;face<6;face++)for(unsigned level=0;level<6;level++){
        unsigned n=face*6+level;s=alias(t,face,level);aliases[n]=s->guest;pages[n]=s->data;
        for(unsigned j=0;j<n;j++)assert(aliases[j]!=aliases[n]&&pages[j]!=pages[n]);
        assert(s->width==(32u>>level)&&s->height==s->width&&s->data==t->face_data[face][level]);
        S32(s->data,0x1000+n);assert(call(s,2,0,0,0)==0);
    }
    assert(t->refs==1);
    for(unsigned face=0;face<6;face++)for(unsigned level=0;level<6;level++){
        unsigned n=face*6+level;s=alias(t,face,level);assert(s->guest==aliases[n]&&G32(s->data)==0x1000+n);assert(call(s,2,0,0,0)==0);
    }
    assert(call(t,2,0,0,0)==0);for(unsigned j=0;j<36;j++){freed(pages[j]);assert(!obj_find(aliases[j]));}baseline(1);

    /* Volume level descriptors and aliases retain actual mip depth. Mutations
     * through a volume update its parent's upload generation. */
    t=texture(K_VOLTEX,0,0);s=alias(t,0,1);tg=t->guest;sg=s->guest;data=s->data;
    assert(t->levels==6&&s->width==16&&s->height==16&&s->depth==4&&s->size==4096);
    assert(call(t,17,1,desc,0)==0&&G32(desc+16)==16&&G32(desc+20)==16&&G32(desc+24)==4);
    assert(call(s,8,desc,0,0)==0&&G32(desc+16)==16&&G32(desc+24)==4);
    unsigned generation=t->content_generation;
    assert(call(s,9,out,0,0)==0&&G32(out)==64&&G32(out+4)==1024&&G32(out+8)==data&&t->content_generation==generation+1);
    assert(call(s,10,0,0,0)==0&&t->content_generation==generation+2);
    assert(call(t,2,0,0,0)==1&&call(s,2,0,0,0)==0&&!obj_find(tg)&&!obj_find(sg));freed(data);baseline(1);

    /* Zero external alias references must not discard the last GPU contents.
     * A lock through the parent reads them back first; final destruction retires
     * the native target before its shared guest page is returned. */
    t=texture(K_TEXTURE,1,1);s=alias(t,0,0);tg=t->guest;sg=s->guest;data=s->data;
    assert(surface_prepare(s)==0&&render_surface_count==1&&writes==1);s->gpu_dirty=1;
    assert(call(s,2,0,0,0)==0&&!destroys&&s->renderer);
    assert(call(t,19,0,out,0)==0&&reads==1&&!s->gpu_dirty&&G8(data)==0x37&&G32(out+4)==data);
    assert(alias(t,0,0)==s&&call(s,2,0,0,0)==0);
    destroy_expected_data=data;assert(call(t,2,0,0,0)==0&&destroys==1&&!obj_find(sg));destroy_expected_data=0;freed(data);baseline(1);

    /* Application, sampler, GetTexture and state-block owners are independent. */
    t=texture(K_TEXTURE,1,0);s=alias(t,0,0);tg=t->guest;data=s->data;assert(call(s,2,0,0,0)==0);
    assert(bind(65,0,tg)==0&&t->refs==2&&bind(64,0,out)==0&&G32(out)==tg&&t->refs==3);
    assert(call(t,2,0,0,0)==2&&control(60,0)==0);uint32_t hr;args(the_device->guest,1,tg,0,0);assert(stateblock_record_method(&cpu,65,&hr)&&hr==0);
    assert(control(61,out)==0);D3DObj *block=obj_find(G32(out));assert(t->refs==3);
    assert(call(t,2,0,0,0)==2&&bind(65,0,0)==0&&t->refs==1);
    assert(call(block,5,0,0,0)==0&&t->refs==2&&draw_state.texture[1]==tg);
    assert(call(block,2,0,0,0)==0&&t->refs==1&&bind(65,1,0)==0&&!obj_find(tg));freed(data);baseline(1);

    /* A target binding alone keeps a texture's alias and its parent alive. Get
     * returns another owner. Depth Get reflects Set and missing-depth errors. */
    the_device->data=make_surface(64,64,22,1);the_device->level_surface[0]=make_surface(64,64,75,2);
    binding_replace(&current_rt_guest,the_device->data);binding_replace(&current_depth_guest,the_device->level_surface[0]);
    D3DObj *bb=obj_find(the_device->data),*implicit_depth=obj_find(the_device->level_surface[0]);assert(bb->refs==2&&implicit_depth->refs==2);
    assert(target(18,0,0,0,out)==0&&G32(out)==bb->guest&&bb->refs==3);obj_release(bb);
    assert(target(18,1,0,0,out)==D3DERR_INVALIDCALL&&bb->refs==2);
    t=texture(K_TEXTURE,1,1);s=alias(t,0,0);tg=t->guest;sg=s->guest;data=s->data;
    assert(target(37,0,sg,0,0)==0&&s->refs==2&&call(s,2,0,0,0)==1&&call(t,2,0,0,0)==1);
    assert(target(38,0,out,0,0)==0&&G32(out)==sg&&s->refs==2&&call(s,2,0,0,0)==1);
    assert(target(37,0,tg,0,0)==D3DERR_INVALIDCALL&&current_rt_guest==sg);
    assert(target(37,0,bb->guest,0,0)==0&&!obj_find(tg)&&!obj_find(sg));freed(data);
    assert(create_surface(the_device,64,64,75,2,0,out)==0);s=obj_find(G32(out));sg=s->guest;data=s->data;
    const uint32_t device_iid[4]={0xd0223b96,0x43fdbf7a,0x3ba4bd92,0xebb9820d};memcpy(GPTR(iid),device_iid,16);
    assert(call(s,11,iid,out,0)==0&&G32(out)==the_device->guest&&the_device->refs==3);obj_release(the_device);
    assert(target(39,sg,0,0,0)==0&&s->refs==2&&call(s,2,0,0,0)==1&&implicit_depth->refs==1);
    assert(target(40,out,0,0,0)==0&&G32(out)==sg&&s->refs==2);assert(call(s,2,0,0,0)==1);
    assert(target(39,0,0,0,0)==0&&!obj_find(sg));freed(data);
    assert(target(40,out,0,0,0)==0x88760866u&&!G32(out));
    assert(target(39,bb->guest,0,0,0)==D3DERR_INVALIDCALL&&!current_depth_guest);
    assert(target(39,implicit_depth->guest,0,0,0)==0);baseline(1);

    /* Reset relinquishes external RT/depth owners without destroying implicit
     * buffers; descriptors, pages and creating-device references all return. */
    assert(create_surface(the_device,32,32,22,1,0,out)==0);s=obj_find(G32(out));sg=s->guest;data=s->data;
    assert(target(37,0,sg,0,0)==0&&call(s,2,0,0,0)==1);
    uint32_t pp=0x2300;S32(pp,64);S32(pp+4,64);assert(reset_device(the_device,pp)==0&&!obj_find(sg));freed(data);baseline(1);

    /* Repeated bounded live sets exceed the old 65536 descriptor capacity and
     * several GiB of page turnover without increasing the high water marks. */
    int object_before=obj_count;uint32_t heap_before=heap_ptr,page_before=page_top;
    for(unsigned n=0;n<24000;n++){
        int kind=n%3==0?K_TEXTURE:n%3==1?K_CUBETEX:K_VOLTEX;t=texture(kind,3,0);tg=t->guest;
        for(unsigned level=0;level<3;level++){
            s=alias(t,kind==K_CUBETEX?n%6:0,level);pages[level]=s->data;S32(s->data,n);
            assert(call(s,2,0,0,0)==0);
        }
        assert(bind(65,0,tg)==0&&call(t,2,0,0,0)==1&&bind(65,0,0)==0&&!obj_find(tg));
        for(unsigned level=0;level<3;level++)freed(pages[level]);
    }
    assert(obj_count==object_before&&heap_ptr==heap_before&&page_top==page_before);baseline(1);
    printf("PASS: texture/cube/volume parent aliases, 36 distinct cube face-mips, COM/Get/container/device refs, cache-preserving GPU-target teardown doubles, sampler/custom-block/RT/depth/Reset ownership; 96000 descriptor turnovers, %llu page bytes, slots=%d heap-high=%08x page-high=%08x.\n",24000ull*3*65536,obj_count,heap_ptr,page_top);
    munmap(engine_flat_base,GUEST_SIZE);return 0;
}
