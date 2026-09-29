/* Resident static vertex buffers through the production D3D draw path.
 *
 * The real d3d9.c (COM objects, Lock/Unlock, SetStreamSource, the draw
 * methods, ProcessVertices, Release and slot reuse, the real guest heap and
 * page allocators) drives a fake renderer. Every programmable draw that is
 * handed a resident copy is checked byte for byte: the copy at the given
 * offset must hold exactly the bytes the arena path would have copied from
 * guest memory for that draw, stream zero and secondary streams alike. A
 * randomized run mixes draws of every kind with locks, writes, partial
 * relocks, draws while locked, ProcessVertices, dynamic and unaligned
 * buffers, releases, slot reuse and the fast-path switch. No GPU.
 *
 * clang -O2 -DENGINE_FLAT_MEMORY=1 -I.. -I../../EngineReuse test_vb_resident.c
 *   -ffunction-sections -fdata-sections -Wl,-dead_strip -lm -o /tmp/vb-resident */
#include "../host.c"
static uint32_t resident_proc_address(const char *dll,const char *name){(void)dll;(void)name;return 0xfe000000;}
#define host_proc_address resident_proc_address
#define VB_RESIDENT_BUDGET_BYTES (1024ull * 1024)
#include "../d3d9.c"
#undef host_proc_address
#include <assert.h>

/* Fake renderer. Resident copies are malloc'd blocks that remember their length. */
typedef struct { size_t length; uint8_t bytes[]; } FakeBuffer;
static unsigned buffers_live, buffers_created;
void *mr_buffer_create(const void *bytes,size_t length){
    FakeBuffer *b=malloc(sizeof *b+length);assert(b);b->length=length;memcpy(b->bytes,bytes,length);
    buffers_live++;buffers_created++;return b;
}
void mr_buffer_release(void *buffer){assert(buffer&&buffers_live);buffers_live--;free(buffer);}
const void *mr_buffer_contents(const void *buffer){return ((const FakeBuffer *)buffer)->bytes;}
size_t mr_buffer_length(const void *buffer){return ((const FakeBuffer *)buffer)->length;}
static int fast=1;
int mr_fast_paths_enabled(void){return fast;}
void mr_texture_bindings_begin(void) {}
void mr_texture_bindings_end(void) {}
void mr_draw_traffic_stats(uint64_t *a,uint64_t *b,uint64_t *c){if(a)*a=0;if(b)*b=0;if(c)*c=0;}

static unsigned draws,resident_stream0,resident_streams,arena_stream0;
static const void *last_resident[16];
static size_t last_offset[16];
int mr_draw_program(mr_context *c,const mr_program_state *state,int prim,const void *vertices,size_t stride,uint32_t nv,const uint16_t *indices,uint32_t ni){
    (void)c;(void)prim;(void)indices;(void)ni;
    draws++;
    for(int s=0;s<16;s++){
        last_resident[s]=state->resident[s];last_offset[s]=state->resident_offset[s];
        if(!state->resident[s]){if(!s)arena_stream0++;continue;}
        const void *expect=s?state->stream_bytes[s]:vertices;size_t length=s?state->stream_length[s]:(size_t)nv*stride;
        const FakeBuffer *b=state->resident[s];
        /* The identity the whole change rests on. */
        assert(expect&&state->resident_offset[s]<=b->length&&length<=b->length-state->resident_offset[s]);
        assert(!memcmp(b->bytes+state->resident_offset[s],expect,length));
        if(s)resident_streams++;else resident_stream0++;
    }
    return 0;
}
/* Reached only through paths this fixture never takes. */
void mr_destroy(mr_context *context){(void)context;abort();}
int mr_resize(mr_context *c,int w,int h){(void)c;(void)w;(void)h;abort();}
mr_context *mr_create(int w,int h){(void)w;(void)h;abort();}
int mr_read_framebuffer(mr_context *c,void *out,size_t size){(void)c;(void)out;(void)size;abort();}
int mr_write_framebuffer(mr_context *c,const void *in,size_t size){(void)c;(void)in;(void)size;abort();}
void mr_set_viewport(mr_context *c,int x,int y,int w,int h){(void)c;(void)x;(void)y;(void)w;(void)h;}
void mr_set_overlay_target(mr_context *c){(void)c;}
const char *mr_last_error(void){return "";}
int mr_draw_rhw(mr_context *c,const mr_draw_state *st,int prim,const void *v,size_t stride,uint32_t nv,const uint16_t *ix,uint32_t ni){(void)c;(void)st;(void)prim;(void)v;(void)stride;(void)nv;(void)ix;(void)ni;abort();}
int mr_draw_fixed_clip(mr_context *c,const mr_program_state *st,int prim,const void *v,size_t stride,uint32_t nv,const uint16_t *ix,uint32_t ni){(void)c;(void)st;(void)prim;(void)v;(void)stride;(void)nv;(void)ix;(void)ni;abort();}
int mr_draw_program_rhw(mr_context *c,const mr_program_state *st,int prim,const void *v,size_t stride,uint32_t nv,const uint16_t *ix,uint32_t ni){(void)c;(void)st;(void)prim;(void)v;(void)stride;(void)nv;(void)ix;(void)ni;abort();}
/* No texture is bound in this fixture. */
uint32_t mr_target_texture(mr_context *c){(void)c;abort();}
uint32_t mr_texture_find_cached(mr_context *c,uint64_t key){(void)c;(void)key;abort();}
uint32_t mr_texture_create_cached(mr_context *c,uint64_t key,int w,int h,const void *p,size_t pitch){(void)c;(void)key;(void)w;(void)h;(void)p;(void)pitch;abort();}
uint32_t mr_texture_create_cached_nomip(mr_context *c,uint64_t key,int w,int h,const void *p,size_t pitch){(void)c;(void)key;(void)w;(void)h;(void)p;(void)pitch;abort();}
uint32_t mr_texture_create_cube_cached(mr_context *c,uint64_t key,int edge,const void *faces[6],size_t pitch){(void)c;(void)key;(void)edge;(void)faces;(void)pitch;abort();}
uint32_t mr_texture_create_volume_cached(mr_context *c,uint64_t key,int w,int h,int d,const void *p,size_t pitch,size_t slice){(void)c;(void)key;(void)w;(void)h;(void)d;(void)p;(void)pitch;(void)slice;abort();}
size_t halo_texture_bgra_size(uint32_t w,uint32_t h){(void)w;(void)h;abort();}
int halo_texture_decode(uint32_t format,uint32_t w,uint32_t h,const void *src,size_t size,size_t pitch,void *out,size_t out_size){(void)format;(void)w;(void)h;(void)src;(void)size;(void)pitch;(void)out;(void)out_size;abort();}

static EngineCPU cpu;
static const uint32_t stack=0x1000,output=0x2000,return_site=0x005abbad;
static void args(const uint32_t *a,unsigned n){cpu.gpr[4]=stack;S32(stack,return_site);S32(stack+4,the_device->guest);for(unsigned i=0;i<n;i++)S32(stack+8+4*i,a[i]);}
static uint32_t device_call(int method,const uint32_t *a,unsigned n){uint32_t hr=~0u;args(a,n);assert(render_method(&cpu,the_device,method,&hr));return hr;}
static uint32_t bind(int method,uint32_t a,uint32_t b,uint32_t c,uint32_t d){uint32_t v[4]={a,b,c,d};return device_call(method,v,4);}
static uint32_t buffer_call(D3DObj *o,int method,uint32_t a,uint32_t b,uint32_t c,uint32_t d){
    int argc=classes[o->kind].methods[method].argc;cpu.gpr[4]=stack;S32(stack,return_site);S32(stack+4,o->guest);S32(stack+8,a);S32(stack+12,b);S32(stack+16,c);S32(stack+20,d);
    method_buffer(&cpu,o,method);assert(cpu.pc==return_site&&cpu.gpr[4]==stack+4+4*argc);return cpu.gpr[0];
}
static uint64_t rng=0x9E3779B97F4A7C15ull;
static uint32_t rnd(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)(rng>>11);}
static uint32_t below(uint32_t n){return n?rnd()%n:0;}
static void fill(uint32_t guest,uint32_t bytes){for(uint32_t i=0;i<bytes;i++)*((uint8_t *)GPTR(guest)+i)=(uint8_t)rnd();}

enum { POS_STRIDE=24, UV_STRIDE=8, VERTS=2000 };
typedef struct { D3DObj *o; uint32_t guest, size, usage; } Buffer;
static Buffer make_vb(uint32_t size,uint32_t usage){
    assert(create_buffer(the_device,K_VB,size,usage,0,usage&0x200?2:1,output)==0);
    Buffer b={obj_from_guest(G32(output)),G32(output),size,usage};assert(b.o);b.o->fvf=2;
    /* Fill through Lock/Unlock, the way sub_00524980 does. */
    assert(buffer_call(b.o,11,0,0,output,0)==0);fill(G32(output),size);assert(buffer_call(b.o,12,0,0,0,0)==0);
    return b;
}
static void drop(Buffer *b){if(b->o){obj_release(b->o);b->o=NULL;}}
static int draw_ok(uint32_t hr){return hr==0;}
static D3DObj *pvshader;
static void pv_write(Buffer *dest,uint32_t first,uint32_t count){
    uint32_t previous=draw_state.vs;assert(bind(92,pvshader->guest,0,0,0)==0);
    uint32_t revision=dest->o->content_generation;
    uint32_t pv[6]={0,first,count,dest->guest,0,1};assert(device_call(85,pv,6)==0);
    assert(dest->o->content_generation==revision+1);
    for(uint32_t i=0;i<count;i++)assert(!memcmp(GPTR(dest->o->data+(first+i)*12),draw_state.vs_float[0],12));
    assert(bind(92,previous,0,0,0)==0);
}
static uint32_t sb_control(int method,uint32_t a){
    uint32_t v[]={a},hr=~0u;args(v,1);assert(stateblock_device_method(&cpu,the_device,method,&hr));return hr;
}
static uint32_t sb_apply(D3DObj *o){
    args(NULL,0);S32(stack+4,o->guest);method_simple(&cpu,o,5);return cpu.gpr[0];
}
/* Volatile observations and noinline prevent either measured path being elided. */
static volatile uintptr_t benchmark_sink;
__attribute__((noinline)) static void benchmark_copy(void *dest,const void *src,size_t length){
    memcpy(dest,src,length);benchmark_sink^=((const uint8_t *)dest)[length-1];
}
__attribute__((noinline)) static void benchmark_resident(D3DObj *o,size_t length){
    const void *buffer=vb_resident(o,0,length);assert(buffer);benchmark_sink^=(uintptr_t)buffer;
}
static void benchmark(Buffer *b,size_t length){
    const unsigned draws_per_sample=4000,slots=256;assert(length<=b->size);
    uint8_t *arena=malloc(length*slots);assert(arena);memset(arena,0,length*slots);
    double copy_best=1e30,resident_best=1e30;
    for(unsigned rep=0;rep<7;rep++){
        uint64_t t0=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for(unsigned k=0;k<draws_per_sample;k++)benchmark_copy(arena+(k%slots)*length,GPTR(b->o->data),length);
        uint64_t t1=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for(unsigned k=0;k<draws_per_sample;k++)benchmark_resident(b->o,length);
        uint64_t t2=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        double copy=(t1-t0)/(double)draws_per_sample,resident=(t2-t1)/(double)draws_per_sample;
        if(copy<copy_best)copy_best=copy;if(resident<resident_best)resident_best=resident;
    }
    printf("warm %zu-byte stream: arena memcpy %.0f ns, verified resident %.0f ns per draw (Mac CPU microbenchmark, no GPU)\n",length,copy_best,resident_best);
    free(arena);
}

static void check_equality(void){
    uint8_t a[272],b[272];unsigned checked=0;
    for(size_t offset=0;offset<8;offset++)for(size_t length=0;length<=257;length++){
        for(size_t i=0;i<sizeof a;i++)a[i]=(uint8_t)rnd();memcpy(b,a,sizeof a);
        assert(vb_resident_equal(a+offset,b+offset,length));checked++;
        for(size_t i=0;i<length;i++){
            b[offset+i]^=1;assert(!vb_resident_equal(a+offset,b+offset,length));
            b[offset+i]^=1;checked++;
        }
        if(length<257){b[offset+length]^=1;assert(vb_resident_equal(a+offset,b+offset,length));}
    }
    printf("resident equality: %u byte-position/alignment/range cases\n",checked);
}
int main(void){
    check_equality();rng=0x9E3779B97F4A7C15ull;
    engine_flat_base=mmap(NULL,GUEST_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(engine_flat_base!=MAP_FAILED);
    the_device=obj_new(K_DEVICE);render_defaults(640,480);
    the_device->data=make_surface(64,64,22,1);D3DObj *rt=obj_from_guest(the_device->data);assert(rt);
    rt->renderer=(mr_context *)(uintptr_t)0x1234;rt->gpu_dirty=1;   /* GPU copy authoritative: no upload */
    /* Shaders and a declaration: position+colour in stream 0, a texcoord in stream 1. */
    D3DObj *vs=obj_new(K_VSHADER),*ps=obj_new(K_PSHADER),*decl=obj_new(K_VDECL);
    vs->data=guest_alloc(64);vs->size=64;vs->content_key=11;ps->data=guest_alloc(64);ps->size=64;ps->content_key=22;
    const uint8_t elements[]={0,0,0,0,2,0,0,0, 0,0,12,0,4,0,10,0, 1,0,0,0,1,0,5,0, 255,0,0,0,17,0,0,0};
    decl->data=guest_alloc(sizeof elements);decl->size=sizeof elements;memcpy(GPTR(decl->data),elements,sizeof elements);
    assert(bind(87,decl->guest,0,0,0)==0&&bind(92,vs->guest,0,0,0)==0&&bind(107,ps->guest,0,0,0)==0);
    /* Valid VS 1.1: mov oPos, c0. ProcessVertices writes real guest memory. */
    const uint32_t pvwords[]={0xfffe0101u,1,0xc00f0000u,0xa0e40000u,0x0000ffffu};
    pvshader=obj_new(K_VSHADER);pvshader->data=guest_alloc(sizeof pvwords);pvshader->size=sizeof pvwords;
    memcpy(GPTR(pvshader->data),pvwords,sizeof pvwords);
    draw_state.vs_float[0][0]=1;draw_state.vs_float[0][1]=2;draw_state.vs_float[0][2]=3;draw_state.vs_float[0][3]=1;
    /* Index buffer: 16-bit, indices kept below 1000 so any NumVertices >= 1000 draws. */
    assert(create_buffer(the_device,K_IB,60000,8,101,1,output)==0);D3DObj *ib=obj_from_guest(G32(output));
    for(uint32_t i=0;i<30000;i++)S16(ib->data+2*i,(uint16_t)below(1000));
    assert(bind(104,ib->guest,0,0,0)==0);

    Buffer pos=make_vb(VERTS*POS_STRIDE,8),uv=make_vb(VERTS*UV_STRIDE,8),dyn=make_vb(VERTS*POS_STRIDE,0x208);
    uint32_t pos_offset=0;
    #define BIND_STREAMS() do{assert(bind(100,0,pos.guest,pos_offset,POS_STRIDE)==0&&bind(100,1,uv.guest,0,UV_STRIDE)==0);}while(0)
    BIND_STREAMS();

    /* 1. A static buffer is copied once and then bound for every bearing. */
    uint32_t a[8]={4,0,0,1500,0,400};
    unsigned created=buffers_created;
    for(int bearing=0;bearing<9;bearing++){assert(draw_ok(device_call(82,a,6)));assert(last_resident[0]==pos.o->resident&&last_resident[1]==uv.o->resident&&last_offset[0]==0);}
    assert(buffers_created==created+2&&pos.o->resident_copies==1&&resident_stream0==9&&resident_streams==9);
    /* 2. A draw while the buffer is locked copies from guest memory. */
    assert(buffer_call(pos.o,11,0,0,output,0)==0);unsigned arena=arena_stream0;
    assert(draw_ok(device_call(82,a,6))&&!last_resident[0]&&arena_stream0==arena+1);
    fill(G32(output)+48,480);assert(buffer_call(pos.o,12,0,0,0,0)==0);
    /* ...and after Unlock the next draw takes a fresh copy holding the write. */
    assert(draw_ok(device_call(82,a,6))&&last_resident[0]==pos.o->resident&&pos.o->resident_copies==2);
    /* 3. Dynamic buffers never get a copy. */
    assert(bind(100,0,dyn.guest,0,POS_STRIDE)==0);
    for(int i=0;i<3;i++){assert(draw_ok(device_call(82,a,6))&&!last_resident[0]&&last_resident[1]);}
    assert(!dyn.o->resident);
    BIND_STREAMS();
    /* 4. The UP draws take user memory: stream zero must not use the bound buffer's copy. */
    uint32_t user=guest_alloc(64*POS_STRIDE);fill(user,64*POS_STRIDE);
    uint32_t up[4]={4,20,user,POS_STRIDE};assert(draw_ok(device_call(83,up,4))&&!last_resident[0]);BIND_STREAMS();
    uint32_t user_ix=guest_alloc(120);for(uint32_t i=0;i<60;i++)S16(user_ix+2*i,(uint16_t)below(64));
    uint32_t upi[8]={4,0,64,20,user_ix,101,user,POS_STRIDE};assert(draw_ok(device_call(84,upi,8))&&!last_resident[0]);
    BIND_STREAMS();assert(bind(104,ib->guest,0,0,0)==0);
    /* 5. An unaligned stream offset falls back to the copy. */
    pos_offset=2;BIND_STREAMS();uint32_t a2[6]={4,0,0,1000,0,100};
    assert(draw_ok(device_call(82,a2,6))&&!last_resident[0]);pos_offset=0;BIND_STREAMS();
    /* 6. ProcessVertices writes its destination without a Lock. */
    unsigned copies=pos.o->resident_copies;
    pv_write(&pos,3,4);
    assert(draw_ok(device_call(82,a,6))&&last_resident[0]==pos.o->resident&&pos.o->resident_copies==copies+1);
    /* 7. The fast-path switch turns residency off without touching the draw. */
    fast=0;assert(draw_ok(device_call(82,a,6))&&!last_resident[0]&&!last_resident[1]);fast=1;
    /* 8. Released buffers release their copies; a reused slot starts clean. */
    uint32_t old_slot=G32(pos.guest+4),old_generation=pos.o->generation;unsigned live=buffers_live;
    assert(bind(100,0,0,0,0)==0);drop(&pos);assert(buffers_live==live-1);
    pos=make_vb(VERTS*POS_STRIDE,8);assert(G32(pos.guest+4)==old_slot&&pos.o->generation!=old_generation&&!pos.o->resident&&!pos.o->resident_copies);
    BIND_STREAMS();assert(draw_ok(device_call(82,a,6))&&last_resident[0]==pos.o->resident&&pos.o->resident_copies==1);
    /* 9. A buffer rewritten again and again goes back to per-draw copies. */
    Buffer churn=make_vb(VERTS*POS_STRIDE,0);assert(bind(100,0,churn.guest,0,POS_STRIDE)==0);
    for(int n=0;n<8;n++){assert(buffer_call(churn.o,11,0,0,output,0)==0);fill(G32(output),64);assert(buffer_call(churn.o,12,0,0,0,0)==0);assert(draw_ok(device_call(82,a,6)));}
    assert(churn.o->resident_refused&&!churn.o->resident&&!last_resident[0]);
    BIND_STREAMS();

    /* 10. Randomized: every kind of draw interleaved with every kind of write. */
    Buffer pool[6];for(int i=0;i<6;i++)pool[i]=make_vb(VERTS*POS_STRIDE,i==5?0x200:0);
    unsigned before=draws,accepted=0;
    for(unsigned step=0;step<60000;step++){
        Buffer *b=&pool[below(6)];
        uint32_t r=below(400);
        if(r==0){uint32_t off=below(VERTS*POS_STRIDE),len=below(VERTS*POS_STRIDE-off+1);
            assert(buffer_call(b->o,11,off,len,output,0)==0);fill(G32(output),len>256?256:len);
            if(below(2)){uint32_t v[6]={4,0,0,1000+below(300),0,1+below(300)};assert(bind(100,0,b->guest,0,POS_STRIDE)==0);
                if(draw_ok(device_call(82,v,6)))assert(!last_resident[0]);}   /* locked: never resident */
            assert(buffer_call(b->o,12,0,0,0,0)==0);
        }else if(r==1){pv_write(b,below(VERTS-8),1+below(8));
        }else if(r<4){if(b!=&pool[5]){assert(bind(100,0,0,0,0)==0);drop(b);*b=make_vb(VERTS*POS_STRIDE,below(4)?0:0x200);}
        }else if(r<30){uint32_t v[4]={4,1+below(20),user,POS_STRIDE};assert(bind(100,0,b->guest,0,POS_STRIDE)==0);
            if(draw_ok(device_call(83,v,4)))assert(!last_resident[0]);
        }else{
            uint32_t offset=below(4)?POS_STRIDE*below(100):below(64);
            assert(bind(100,0,b->guest,offset,POS_STRIDE)==0);
            unsigned seen=draws;int ok;
            if(below(3)){uint32_t v[3]={below(2)?4:5,below(500),1+below(400)};ok=draw_ok(device_call(81,v,3));}
            else{uint32_t v[6]={4,below(400),below(3)?0:below(50),1000+below(300),below(29000),1+below(300)};ok=draw_ok(device_call(82,v,6));}
            accepted+=ok;
            if(draws!=seen){
                if(offset&3)assert(!last_resident[0]);
                if(b->o->usage&0x200)assert(!last_resident[0]);
                if(!fast)assert(!last_resident[0]&&!last_resident[1]);
            }
        }
        if(fast?!below(997):!below(50))fast=!fast;   /* short stretches with the fast paths off */
    }
    fast=1;
    printf("randomized: %u draws (%u accepted), stream0 resident %u / arena %u, secondary resident %u, copies %u\n",
           draws-before,accepted,resident_stream0,arena_stream0,resident_streams,buffers_created);
    assert(resident_stream0>15000&&arena_stream0>5000&&resident_streams>30000);

    /* An untracked write is caught on the very next draw: stale bytes are never used. */
    Buffer rogue=make_vb(VERTS*POS_STRIDE,0);assert(bind(100,0,rogue.guest,0,POS_STRIDE)==0);
    assert(draw_ok(device_call(82,a,6))&&last_resident[0]==rogue.o->resident);
    fill(rogue.o->data+POS_STRIDE*10,POS_STRIDE);
    assert(draw_ok(device_call(82,a,6))&&!last_resident[0]&&rogue.o->resident_refused&&!rogue.o->resident);
    assert(vb_resident_mismatches==1);
    assert(draw_ok(device_call(82,a,6))&&!last_resident[0]);
    /* Dirty bytes outside one drawn range are checked as soon as a later draw reads them. */
    Buffer rogue2=make_vb(VERTS*POS_STRIDE,0);assert(vb_resident(rogue2.o,0,64));
    fill(rogue2.o->data+256,64);assert(vb_resident(rogue2.o,0,64));
    assert(!vb_resident(rogue2.o,256,64)&&rogue2.o->resident_refused&&vb_resident_mismatches==2);
    /* Streaming flags opt out permanently, even on a non-DYNAMIC buffer. */
    for(unsigned flag=0x1000;flag<=0x2000;flag*=2){
        Buffer streaming=make_vb(VERTS*POS_STRIDE,0);assert(vb_resident(streaming.o,0,64));
        assert(buffer_call(streaming.o,11,24,64,output,flag)==0);
        assert(!streaming.o->resident&&streaming.o->resident_refused);fill(G32(output),64);
        assert(buffer_call(streaming.o,12,0,0,0,0)==0&&!vb_resident(streaming.o,0,64));drop(&streaming);
    }
    /* READONLY remains conservatively invalidated; nested locks cannot reuse a snapshot. */
    Buffer readonly=make_vb(VERTS*POS_STRIDE,0);assert(vb_resident(readonly.o,0,64));
    assert(buffer_call(readonly.o,11,0,0,output,0x10)==0&&buffer_call(readonly.o,11,0,64,output,0)==0);
    assert(buffer_call(readonly.o,12,0,0,0,0)==0&&!vb_resident(readonly.o,0,64));
    assert(buffer_call(readonly.o,12,0,0,0,0)==0&&vb_resident(readonly.o,0,64));drop(&readonly);
    /* Sparse stateblocks retain the buffer instance, offsets and stride. */
    Buffer blocked=make_vb(VERTS*POS_STRIDE,0);uint32_t blocked_guest=blocked.guest;
    assert(sb_control(60,0)==0&&bind(100,0,blocked.guest,POS_STRIDE*2,POS_STRIDE)==0&&sb_control(61,output)==0);
    D3DObj *block=obj_from_guest(G32(output));assert(block&&blocked.o->refs==2);
    drop(&blocked);assert(sb_apply(block)==0&&draw_state.stream[0]==blocked_guest&&draw_state.offset[0]==POS_STRIDE*2);
    assert(draw_ok(device_call(82,a,6))&&last_resident[0]&&last_offset[0]==POS_STRIDE*2);
    D3DObj *retained=obj_from_guest(blocked_guest);assert(retained&&retained->refs==2);
    obj_release(block);assert(retained->refs==1);BIND_STREAMS();assert(!obj_find(blocked_guest));
    benchmark(&pos,900*POS_STRIDE);benchmark(&pos,pos.size);

    /* Everything released: no copy outlives its buffer. */
    assert(bind(100,0,0,0,0)==0&&bind(100,1,0,0,0)==0);
    drop(&pos);drop(&uv);drop(&dyn);drop(&churn);drop(&rogue);drop(&rogue2);for(int i=0;i<6;i++)drop(&pool[i]);
    assert(buffers_live==0&&vb_resident_live_bytes==0);
    /* A large mission remains bounded. Denied geometry still draws from the
     * guest arena, and becomes eligible when an older object releases space. */
    Buffer pressure[24];
    for(unsigned i=0;i<24;i++){
        pressure[i]=make_vb(65536,0);
        const void *copy=vb_resident(pressure[i].o,0,64);
        assert((copy!=NULL)==(i<16));
        assert(vb_resident_live_bytes<=VB_RESIDENT_BUDGET_BYTES);
    }
    assert(vb_resident_live_bytes==VB_RESIDENT_BUDGET_BYTES&&vb_resident_budget_fallbacks==8);
    assert(bind(100,0,pressure[16].guest,0,POS_STRIDE)==0);
    assert(bind(100,1,pressure[16].guest,0,UV_STRIDE)==0);
    assert(draw_ok(device_call(82,a,6))&&!last_resident[0]);
    drop(&pressure[0]);
    assert(draw_ok(device_call(82,a,6))&&last_resident[0]);
    /* Replacing a dirty generation at capacity releases the old ownership. */
    assert(buffer_call(pressure[16].o,11,0,64,output,0)==0);
    fill(G32(output),64);assert(buffer_call(pressure[16].o,12,0,0,0,0)==0);
    assert(draw_ok(device_call(82,a,6))&&last_resident[0]);
    assert(vb_resident_live_bytes==VB_RESIDENT_BUDGET_BYTES);
    assert(bind(100,0,0,0,0)==0&&bind(100,1,0,0,0)==0);
    for(unsigned i=0;i<24;i++)drop(&pressure[i]);
    assert(buffers_live==0&&vb_resident_live_bytes==0&&vb_resident_peak_bytes==VB_RESIDENT_BUDGET_BYTES);
    printf("PASS: resident vertex copies byte-identical to the arena bytes on every draw (%u draws); copy once per generation, Lock/Unlock and successful ProcessVertices invalidate, never while locked, never for dynamic, UP or unaligned streams, released with the buffer, clean slot reuse, churn fallback, fast-path switch, NOOVERWRITE/DISCARD, stateblocks, immediate unlocked-write fallback.\n",draws);
    munmap(engine_flat_base,GUEST_SIZE);return 0;
}
