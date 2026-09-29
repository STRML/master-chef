/* Differential test for draw_indices (d3d9_render.inc), which rebases and
 * validates every indexed draw's indices on the engine thread.
 *
 * The production function, with its 16-bit list/strip fast path, runs next to
 * a verbatim copy of the loop it had before, on the same flat guest memory,
 * over randomized and edge-case inputs: all primitive types including fans,
 * 16- and 32-bit and invalid formats, no index buffer, bases that push
 * indices below zero, to nv and past 65535, vertex counts from 0 to past the
 * 65536 limit, and index ranges at the top of the 4 GB guest space. The
 * result pointer (NULL or not), the output count and every output index must
 * match. On a rejected draw the scratch contents are not compared: the old
 * loop left a partial prefix there, and both callers treat NULL as failure
 * without reading it. Timing compares the two on a typical draw.
 *
 * clang -O2 -DENGINE_FLAT_MEMORY=1 -I.. -I../../EngineReuse test_draw_indices.c
 *   -ffunction-sections -fdata-sections -Wl,-dead_strip -lm -o /tmp/draw-indices */
#include "../d3d9.c"
#include <assert.h>
#include <sys/mman.h>
uint8_t *engine_flat_base;

/* The loop as it was before the fast path, with its own scratch. */
static void *reference_scratch; static size_t reference_capacity;
static uint16_t *reference_draw_indices(uint32_t prim,uint32_t nv,uint32_t source,uint32_t count,uint32_t format,int32_t base,uint32_t *out_count) {
    unsigned is=format==101?2:format==102?4:0;uint32_t n=source?count:nv;
    *out_count=prim==6?(n>=3?(n-2)*3:0):source?count:0;
    if(!*out_count)return NULL;
    if(*out_count>196608||n>196608||(source&&(!is||(uint64_t)source+(uint64_t)n*is>0x100000000ULL)))return NULL;
    uint16_t *out=draw_scratch_grow(&reference_scratch,&reference_capacity,(size_t)*out_count*2);
    if(!out)return NULL;
    for(uint32_t i=0;i<*out_count;i++){
        uint32_t k=prim==6?(i%3==0?0:i/3+(i%3)):i;
        int64_t v=source?(int64_t)(is==2?G16(source+k*2):G32(source+k*4))+base:k;
        if(v<0||v>=nv||v>65535)return NULL;
        out[i]=(uint16_t)v;
    }
    return out;
}

static uint64_t rng=0x243F6A8885A308D3ull;
static uint32_t rnd(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return (uint32_t)(rng>>11);}
static uint32_t below(uint32_t n){return n?rnd()%n:0;}
static double now_ns(void){return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);}

static unsigned compared,accepted;
static void check(uint32_t prim,uint32_t nv,uint32_t source,uint32_t count,uint32_t format,int32_t base){
    uint32_t want_count=~0u,got_count=~1u;
    uint16_t *want=reference_draw_indices(prim,nv,source,count,format,base,&want_count);
    uint16_t *got=draw_indices(prim,nv,source,count,format,base,&got_count);
    assert(want_count==got_count);assert(!want==!got);
    if(want){assert(!memcmp(want,got,(size_t)got_count*2));accepted++;}
    compared++;
}

int main(void){
    engine_flat_base=mmap(NULL,UINT64_C(0x100000000),PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);
    assert(engine_flat_base!=MAP_FAILED);
    const uint32_t area=0x00100000u,top=0xFFFF0000u;   /* 1 MB of indices low, 64 KB at the top of the space */
    /* The lower half holds indices below 1200, the upper half a mix with the odd huge one. */
    for(uint32_t i=0;i<0x80000u;i++)S16(area+2*i,(uint16_t)(i<0x40000u||below(64)?below(1200):rnd()));
    for(uint32_t i=0;i<0x8000u;i++)S16(top+2*i,(uint16_t)below(3000));
    /* Randomized. */
    const uint32_t formats[]={101,101,101,102,0,77};
    for(unsigned n=0;n<400000;n++){
        uint32_t prim=1+below(6),format=formats[below(6)];
        uint32_t nv=below(8)?1000+below(1000):(below(2)?below(70000):65536+below(3));
        uint32_t count=below(10)?below(1500):below(200000);
        uint32_t source=below(10)?area+2*below(0x78000):(below(2)?0:top+below(0x10000));
        int32_t base=below(4)?0:(below(2)?-(int32_t)below(1300):(int32_t)below(70000));
        if(below(3)==0)base=-(int32_t)below(40);
        check(prim,nv,source,count,format,base);
    }
    /* Edges: exact limits of every bound. */
    for(uint32_t prim=1;prim<=6;prim++)for(int f=0;f<3;f++){
        uint32_t format=f==0?101:f==1?102:0;
        const uint32_t counts[]={0,1,2,3,4,196607,196608,196609,65536,65537};
        for(unsigned c=0;c<10;c++){
            check(prim,65536,area,counts[c],format,0);check(prim,1200,area,counts[c],format,0);
            check(prim,1,area,counts[c],format,0);check(prim,counts[c],0,0,format,0);
            check(prim,1200,0xFFFFFFFEu,counts[c],format,0);check(prim,1200,top+0xFFF0u,counts[c]&15,format,0);
        }
        /* Bases that put the smallest index at -1/0 and the largest at nv-1/nv and 65535/65536. */
        uint32_t lo=65535,hi=0;for(uint32_t i=0;i<1200;i++){uint16_t v=G16(area+2*i);lo=v<lo?v:lo;hi=v>hi?v:hi;}
        const int32_t bases[]={-(int32_t)lo-1,-(int32_t)lo,-(int32_t)lo+1,0,1,-1,65535-(int32_t)hi,65536-(int32_t)hi};
        for(unsigned b=0;b<8;b++){check(prim,(uint32_t)(hi+bases[b]+1),area,1200,format,bases[b]);check(prim,(uint32_t)(hi+bases[b]),area,1200,format,bases[b]);check(prim,65536,area,1200,format,bases[b]);}
    }
    printf("draw_indices: %u inputs compared, %u accepted, identical result, count and indices\n",compared,accepted);
    /* Timing on a typical static draw: 1200 16-bit indices, list, base 0. */
    for(uint32_t i=0;i<1200;i++)S16(area+2*i,(uint16_t)below(900));
    uint32_t out=0;double best_old=1e30,best_new=1e30;
    for(int rep=0;rep<9;rep++){
        double t0=now_ns();for(int k=0;k<20000;k++)assert(reference_draw_indices(4,900,area,1200,101,0,&out));
        double t1=now_ns();for(int k=0;k<20000;k++)assert(draw_indices(4,900,area,1200,101,0,&out));
        double t2=now_ns();if(t1-t0<best_old)best_old=t1-t0;if(t2-t1<best_new)best_new=t2-t1;
    }
    printf("timing, 1200 indices: before %.0f ns, now %.0f ns per draw\n",best_old/20000,best_new/20000);
    puts("PASS: draw_indices fast path matches the original loop on every input.");
    return 0;
}
