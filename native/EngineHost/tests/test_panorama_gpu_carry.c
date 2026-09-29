/* Production panorama producer with an injected GPU sink. No Metal/device or
 * game files required. Compile with clang -O2 test_panorama_gpu_carry.c -lm. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../panorama.h"
typedef struct { uint32_t guest,data,width,height,format,size; void *renderer; int gpu_dirty; } D3DObj;
static uint8_t memory[4096];
#define GPTR(p) (memory+(p))
static D3DObj device={.data=1},backbuffer={.guest=1,.data=64,.width=4,.height=4,.size=64};
static D3DObj *the_device=&device;
static unsigned frames_presented;
static int flush_failure;
static D3DObj *obj_from_guest(uint32_t guest){return guest==1?&backbuffer:NULL;}
static D3DObj *rt_surface(void){return &backbuffer;}
static int surface_prepare(D3DObj *s){(void)s;return 0;}
static int surface_flush(D3DObj *s){(void)s;return flush_failure;}
static void mr_clear(void *r,uint32_t c){(void)r;(void)c;}
static void mr_clear_depth(void *r,float d){(void)r;(void)d;}
static void mr_destroy(void *r){(void)r;}
static int mr_write_framebuffer(void *r,void *p,size_t s){(void)r;(void)p;(void)s;return 0;}
static uint32_t guest_page_alloc(uint32_t size){(void)size;return 512;}
static int guest_page_free(uint32_t base){(void)base;return 1;}
static void host_log(const char *fmt,...){(void)fmt;}
#define HOST_ENV getenv
#define G8(addr) 0
static struct { int scope; } fp_capture;
static int mr_blit_target_to(void *a,void *b){(void)a;(void)b;return 0;}
static int mr_fxaa_target_to(void *a,void *b,float subpix){(void)subpix;return mr_blit_target_to(a,b);}
#include "../panorama_render.inc"
static unsigned calls, releases, requested, result;
static HaloPanoramaInfo published;
static int acquire(uint32_t w,uint32_t h,void **textures,int *slot){
    (void)w;(void)h; *slot=2; for(int k=0;k<HALO_PANORAMA_LAYERS;k++)textures[k]=(void*)1; return 1;
}
static void release(int slot){assert(slot==2); releases++;}
static uint32_t carry(int slot,uint32_t mask,HaloPanoramaInfo *metadata){assert(slot==2); calls++;requested=mask;*metadata=published;return result&mask;}
static void draw(unsigned mask){
    host_panorama_reset();
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(mask&(1u<<k)){
        host_panorama_begin(k);
        host_panorama_projection(1,1,0,0,4,4,0);
        host_panorama_end(k);
    }
}
int main(void){
    backbuffer.renderer=(void*)1;
    HaloPanoramaGPUSink sink={acquire,release,carry};
    host_panorama_set_gpu_sink(&sink);
    draw(HALO_PANORAMA_MONO_MASK);
    assert(panorama_info.valid);
    host_panorama_gpu_carry_missing();
    assert(panorama_info.valid && calls==0); /* no previous slot necessary */
    published=panorama_info;
    host_panorama_gpu_handoff();
    result=~0u; draw(1u<<1);
    host_panorama_gpu_carry_missing();
    assert(panorama_info.valid && requested==(HALO_PANORAMA_MONO_MASK&~(1u<<1)));
    assert(panorama_info.layer_epoch[0]==published.layer_epoch[0]);
    assert(panorama_info.layer_epoch[1]==panorama_info.source_epoch);
    host_panorama_gpu_handoff();
    /* A newer producer frame can fail/pending-publish while the last actual
     * completed image remains older. Carry its matching metadata, not the
     * most recent projection seen on the producer. */
    draw(HALO_PANORAMA_MONO_MASK);
    panorama_held_px[0]=9;panorama_held_py[0]=8;
    panorama_held_epoch[0]=panorama_info.source_epoch;
    host_panorama_gpu_handoff();draw(1u<<1);
    host_panorama_gpu_carry_missing();
    assert(panorama_info.valid&&panorama_info.projection_x[0]==published.projection_x[0]);
    assert(panorama_info.projection_y[0]==published.projection_y[0]);
    assert(panorama_info.layer_epoch[0]==published.layer_epoch[0]);
    host_panorama_gpu_handoff();
    /* Inject one failed copy among otherwise successful copies. */
    result=~(1u<<7); draw(1u<<1);
    unsigned before=releases;
    host_panorama_gpu_carry_missing();
    assert(!panorama_info.valid && panorama_info.status==HALO_PANORAMA_WORLD_INCOMPLETE);
    assert(panorama_info.failure_reason==HALO_PANORAMA_READBACK_FAILED);
    assert(host_panorama_gpu_slot()==-1 && releases==before+1);
    assert(!host_panorama_all_layers_ready());
    host_panorama_present_complete(); assert(releases==before+1);
    /* Full refresh recovers even when there is no usable carry source. */
    result=0; draw(HALO_PANORAMA_MONO_MASK); before=calls;
    host_panorama_gpu_carry_missing(); assert(panorama_info.valid && calls==before);
    host_panorama_gpu_handoff();
    draw(1u<<1);host_panorama_gpu_carry_missing();assert(!panorama_info.valid);
    /* After a flat scene boundary, even a successful byte copy from the old
     * scene is not acceptable. A new producer can finish before GPU publish. */
    result=~0u;host_panorama_invalidate();draw(HALO_PANORAMA_MONO_MASK);
    host_panorama_gpu_handoff();draw(1u<<1);host_panorama_gpu_carry_missing();
    assert(!panorama_info.valid&&panorama_info.scene_epoch!=published.scene_epoch);
    /* A sink without carry must reject partial frames, not stale-publish. */
    sink.carry=NULL; host_panorama_set_gpu_sink(&sink);
    draw(HALO_PANORAMA_STEREO_MASK);host_panorama_gpu_carry_missing();assert(panorama_info.valid);
    host_panorama_gpu_handoff();draw(1u<<1);host_panorama_gpu_carry_missing();assert(!panorama_info.valid);
    /* A stale right-eye held bit is not needed when dropping to mono. */
    draw(HALO_PANORAMA_STEREO_MASK);host_panorama_gpu_handoff();
    draw(HALO_PANORAMA_MONO_MASK);host_panorama_gpu_carry_missing();assert(panorama_info.valid);
    puts("PANORAMA_GPU_CARRY_PASS");
}
