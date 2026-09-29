/* Production frame-start regression: a rotating frame may begin on any layer.
 * clang -O2 native/EngineHost/tests/test_panorama_epoch.c -lm -o /tmp/astra-epoch */
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
#define HOST_ENV(name) getenv(name)
#define G8(address) 0
static struct { int scope; } fp_capture;
static int mr_blit_target_to(void *r,void *t){(void)r;(void)t;return 0;}
static int mr_fxaa_target_to(void *a,void *b,float subpix){(void)subpix;return mr_blit_target_to(a,b);}
#include "../panorama_render.inc"
static void pass(int layer) {
    host_panorama_begin(layer);
    host_panorama_projection(1.5f,0.6f,0,0,4,4,0x1234);
    host_panorama_end(layer);
}
int main(void) {
    host_panorama_set_stereo(1);
    host_panorama_reset();
    pass(HALO_PANORAMA_UP);
    assert(panorama_info.source_epoch==1);
    pass(0); /* layer zero must not restart a frame already in progress */
    assert(panorama_info.source_epoch==1 && (panorama_mask&(1u<<HALO_PANORAMA_UP)));
    const int rest[]={2,6,7,8,9,4,1};
    for(unsigned i=0;i<sizeof rest/sizeof *rest;i++)pass(rest[i]);
    assert(panorama_info.valid && host_panorama_all_layers_ready());
    float held=panorama_info.projection_x[0];
    host_panorama_present_complete();
    pass(HALO_PANORAMA_REAR);pass(4);pass(1);
    assert(panorama_info.source_epoch==2 && panorama_info.valid);
    assert(panorama_info.projection_x[0]==held && !(panorama_mask&1));
    host_panorama_present_complete();assert(panorama_info.source_epoch==0);
    pass(2);assert(panorama_info.source_epoch==3);
    puts("panorama epoch: arbitrary first pass, no mid-frame reset, held projection PASS");
}
