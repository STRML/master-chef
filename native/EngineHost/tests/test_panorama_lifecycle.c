/* CPU-only production-producer test: distinct sentinels for every epoch/layer.
 * This exercises the host capture implementation with fake framebuffer IO;
 * no original engine, Metal context or application process is needed. */
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
#define HOST_ENV(name) getenv(name)
static struct { int scope; } fp_capture;
static uint8_t G8(uint32_t address) { assert(address==0x00718FC9u); return 0; }
static int mr_blit_target_to(void *renderer, void *texture) {
    (void)renderer; (void)texture;
    assert(!"CPU lifecycle test must not perform a GPU blit"); return -1;
}
static int mr_fxaa_target_to(void *renderer, void *texture, float subpix) {
    (void)subpix; return mr_blit_target_to(renderer, texture);
}
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
static unsigned freed_hud_pages;
static int guest_page_free(uint32_t base){assert(base==512);freed_hud_pages++;return 1;}
static void host_log(const char *fmt,...){(void)fmt;}
#include "../panorama_render.inc"
static uint32_t sentinel(unsigned epoch,unsigned layer){return 0xff000000u|(epoch<<8)|layer;}
static void fill(D3DObj *s,uint32_t value){for(unsigned i=0;i<s->size/4;i++)memcpy(GPTR(s->data+i*4),&value,4);}
static void render(unsigned epoch,int missing_pass){
    host_panorama_reset();
    const int order[]={0,2,5,6,7,8,9,1};
    for(unsigned n=0;n<sizeof order/sizeof *order;n++){
        int pass=order[n];host_panorama_begin(pass);
        if(pass!=missing_pass)host_panorama_projection(1.5f,0.6f,0,0,backbuffer.width,backbuffer.height,0x1234);
        fill(&backbuffer,sentinel(epoch,(unsigned)pass));
        if(pass==1)fill(panorama_hud,sentinel(epoch,3));
        host_panorama_end(pass);
    }
}
static int present(HaloPanoramaInfo *info,uint32_t output[HALO_PANORAMA_LAYERS]){
    const void *layers[HALO_PANORAMA_LAYERS];int valid=host_panorama_frame(info,layers);
    if(valid)for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(k!=HALO_PANORAMA_CENTRE_RIGHT)memcpy(&output[k],layers[k],4);
    host_panorama_present_complete();
    frames_presented++;return valid;
}
int main(void){
    HaloPanoramaInfo info;uint32_t output[HALO_PANORAMA_LAYERS];
    render(1,-1);assert(present(&info,output));
    assert(info.source_epoch==1&&info.status==HALO_PANORAMA_COMPLETE);
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(k!=HALO_PANORAMA_CENTRE_RIGHT)assert(output[k]==sentinel(1,k));
    fill(&backbuffer,0xffabcdefu);
    /* No hook has run for this distinct flat frame. Old code relabels A. */
    assert(!present(&info,output)&&"stale panorama republished on no-hook Present");
    assert(info.status==HALO_PANORAMA_FLAT&&info.source_epoch==0);
    assert(!host_panorama_all_layers_ready()&&"flat transition retained old scene bearings");
    render(2,2);assert(!present(&info,output));
    assert(info.source_epoch==2&&info.status==HALO_PANORAMA_WORLD_INCOMPLETE&&info.failure_reason==HALO_PANORAMA_NO_PROJECTION);
    render(3,-1);assert(present(&info,output));
    assert(info.source_epoch==3&&info.status==HALO_PANORAMA_COMPLETE);
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(k!=HALO_PANORAMA_CENTRE_RIGHT)assert(output[k]==sentinel(3,k));
    host_panorama_reset();assert(!present(&info,output));
    backbuffer.width=2;backbuffer.height=2;backbuffer.size=16;
    render(4,-1);assert(present(&info,output));assert(info.width==2&&info.height==2&&info.source_epoch==4);
    assert(freed_hud_pages==1);
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(k!=HALO_PANORAMA_CENTRE_RIGHT)assert(output[k]==sentinel(4,k));
    flush_failure=1;render(5,-1);assert(!present(&info,output));
    assert(info.status==HALO_PANORAMA_WORLD_INCOMPLETE&&info.failure_reason==HALO_PANORAMA_READBACK_FAILED);
    flush_failure=0;
    /* Exact source viewport origin survives normalization. */
    backbuffer.width=8;backbuffer.height=8;backbuffer.size=256;
    host_panorama_begin(0);host_panorama_projection(1.5f,0.6f,1,2,6,4,0x1234);host_panorama_end(0);
    assert(panorama_info.viewport_u_min[0]==0.125f&&panorama_info.viewport_v_min[0]==0.25f);
    assert(panorama_info.viewport_u_max[0]==0.875f&&panorama_info.viewport_v_max[0]==0.75f);
    assert(fabsf(panorama_info.projection_x[0]-1.125f)<1e-6f&&fabsf(panorama_info.projection_y[0]-0.3f)<1e-6f);
    render(7,-1);assert(panorama_info.valid);
    uint64_t aborted_epoch=panorama_info.source_epoch;
    host_panorama_abort();assert(!present(&info,output));
    assert(info.source_epoch==aborted_epoch&&info.status==HALO_PANORAMA_WORLD_INCOMPLETE);
    assert(info.failure_reason==HALO_PANORAMA_RENDER_ABORTED);
    /* Return to the same raster after a flat/loading Present. A new center
     * alone must not make the prior scene's peripheral sentinels publishable. */
    render(9,-1);assert(present(&info,output));assert(!present(&info,output));
    host_panorama_begin(1);host_panorama_projection(1.5f,0.6f,0,0,8,8,0x1234);
    fill(&backbuffer,sentinel(10,1));fill(panorama_hud,sentinel(10,3));host_panorama_end(1);
    assert(!present(&info,output)&&info.status==HALO_PANORAMA_WORLD_INCOMPLETE);
    puts("PANORAMA_LIFECYCLE_PASS coherentSentinels=9 flatTransitionInvalidated=true partialNewSceneRejected=true missingSideRejected=true recovered=true resetCleared=true");
}
