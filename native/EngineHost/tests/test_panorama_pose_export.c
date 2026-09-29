/* Per-layer camera export (panorama.h layer_pose, cut_epoch) through the
 * production producer: drawn layers take the frame's camera, held and
 * GPU-carried layers keep the camera they were drawn with, and a camera cut
 * or scene change starts a new cut epoch. No Metal, device or game files.
 * clang -O2 -I native/EngineHost native/EngineHost/tests/test_panorama_pose_export.c -lm */
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
static D3DObj *obj_from_guest(uint32_t guest){return guest==1?&backbuffer:NULL;}
static D3DObj *rt_surface(void){return &backbuffer;}
static int surface_prepare(D3DObj *s){(void)s;return 0;}
static int surface_flush(D3DObj *s){(void)s;return 0;}
static void mr_clear(void *r,uint32_t c){(void)r;(void)c;}
static void mr_clear_depth(void *r,float d){(void)r;(void)d;}
static void mr_destroy(void *r){(void)r;}
static uint32_t guest_page_alloc(uint32_t size){(void)size;return 512;}
static int guest_page_free(uint32_t base){(void)base;return 1;}
static void host_log(const char *fmt,...){(void)fmt;}
#define HOST_ENV getenv
#define G8(addr) 0
static struct { int scope; } fp_capture;
static int mr_blit_target_to(void *a,void *b){(void)a;(void)b;return 0;}
static int mr_fxaa_target_to(void *a,void *b,float subpix){(void)subpix;return mr_blit_target_to(a,b);}
#include "../panorama_render.inc"

/* A level camera at (x,y,z) turned `yaw` radians from +X about +Z: Halo's
 * world is z-up, so forward is (cos, sin, 0) and up is (0, 0, 1). */
static void camera(float pose[HALO_PANORAMA_POSE_FLOATS],float x,float y,float z,float yaw){
    const float value[HALO_PANORAMA_POSE_FLOATS]={x,y,z,cosf(yaw),sinf(yaw),0,0,0,1};
    memcpy(pose,value,sizeof value);
}
static void draw(const float *pose,unsigned mask){
    host_panorama_reset();
    host_panorama_set_camera(pose);
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(mask&(1u<<k)&&k!=HALO_PANORAMA_HUD_LAYER){
        host_panorama_begin(k);
        host_panorama_projection(1,1,0,0,4,4,0);
        host_panorama_end(k);
    }
}
static int same(const float *a,const float *b){return !memcmp(a,b,HALO_PANORAMA_POSE_FLOATS*sizeof *a);}
static void present(void){host_panorama_present_complete();frames_presented++;}

/* GPU sink: publishes into slot 1, carries from the metadata it was given. */
static HaloPanoramaInfo published;
static int acquire(uint32_t w,uint32_t h,void **textures,int *slot){
    (void)w;(void)h;*slot=1;for(int k=0;k<HALO_PANORAMA_LAYERS;k++)textures[k]=(void*)1;return 1;
}
static void release(int slot){(void)slot;}
static uint32_t carry(int slot,uint32_t mask,HaloPanoramaInfo *metadata){(void)slot;*metadata=published;return mask;}

int main(void){
    backbuffer.renderer=(void*)1;
    float a[9],b[9],c[9],d[9],e[9];
    camera(a,10,20,1,0.f);
    /* 1. A full sphere: every drawn layer, and the HUD with the centre,
     * carries the frame's camera bit for bit. The first frame is a cut. */
    draw(a,HALO_PANORAMA_MONO_MASK);
    assert(panorama_info.valid&&panorama_info.source_epoch==1&&panorama_info.cut_epoch==1);
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)
        if((HALO_PANORAMA_MONO_MASK|(1u<<HALO_PANORAMA_HUD_LAYER))&(1u<<k))assert(same(panorama_info.layer_pose[k],a));
    present();
    /* 2. A turn of one stick step (0.04 rad) with a little movement is not a
     * cut. Only the centre is redrawn; every held layer keeps camera a. */
    camera(b,10.05f,20.02f,1,0.04f);
    draw(b,1u<<HALO_PANORAMA_CENTRE_LEFT);
    assert(panorama_info.valid&&panorama_info.source_epoch==2&&panorama_info.cut_epoch==1);
    assert(same(panorama_info.layer_pose[1],b)&&same(panorama_info.layer_pose[HALO_PANORAMA_HUD_LAYER],b));
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)
        if(k!=1&&k!=HALO_PANORAMA_HUD_LAYER&&(HALO_PANORAMA_MONO_MASK&(1u<<k))){
            assert(same(panorama_info.layer_pose[k],a)&&panorama_info.layer_epoch[k]==1);
        }
    HaloPanoramaInfo copy;const void *layers[HALO_PANORAMA_LAYERS];
    assert(host_panorama_frame(&copy,layers)&&same(copy.layer_pose[0],a)&&same(copy.layer_pose[1],b));
    present();
    /* 3. Thirty-five degrees in one frame is not the stick: a cut. The new
     * frame's own epoch becomes cut_epoch, so the older layers fall behind it. */
    camera(c,10.05f,20.02f,1,0.04f+0.61f);
    draw(c,HALO_PANORAMA_MONO_MASK);
    assert(panorama_info.source_epoch==3&&panorama_info.cut_epoch==3);
    assert(same(panorama_info.layer_pose[0],c)&&same(panorama_info.layer_pose[2],c)&&panorama_info.layer_epoch[2]==panorama_info.cut_epoch);
    present();
    /* ...and the cut epoch stays put until the next cut. */
    draw(c,1u<<HALO_PANORAMA_CENTRE_LEFT);assert(panorama_info.cut_epoch==3);present();
    /* 4. A jump of three world units in one frame is a cut too. */
    camera(d,13.1f,20.02f,1,0.65f);
    draw(d,HALO_PANORAMA_MONO_MASK);assert(panorama_info.source_epoch==5&&panorama_info.cut_epoch==5);present();
    /* 5. An unusable camera is recorded as unknown (zero) and the next
     * usable one starts a new cut. */
    float broken[9];memcpy(broken,d,sizeof broken);broken[4]=NAN;
    draw(broken,1u<<HALO_PANORAMA_CENTRE_LEFT);
    for(int k=0;k<HALO_PANORAMA_POSE_FLOATS;k++)assert(panorama_info.layer_pose[1][k]==0.f);
    assert(panorama_info.cut_epoch==6);present();
    draw(d,1u<<HALO_PANORAMA_CENTRE_LEFT);assert(panorama_info.cut_epoch==7&&same(panorama_info.layer_pose[1],d));present();
    /* 6. A scene change (flat frame or Reset) starts a new cut with the
     * first frame of the new scene, however still the camera is. */
    uint64_t scene=panorama_info.scene_epoch;
    host_panorama_invalidate();
    draw(d,HALO_PANORAMA_MONO_MASK);
    assert(panorama_info.scene_epoch==scene+1&&panorama_info.cut_epoch==panorama_info.source_epoch);
    present();
    /* 7. Stereo: the right eye's centre records the same frame camera. */
    host_panorama_set_stereo(1);
    draw(d,HALO_PANORAMA_STEREO_MASK);
    assert(panorama_info.valid&&same(panorama_info.layer_pose[HALO_PANORAMA_CENTRE_RIGHT],d));
    present();host_panorama_set_stereo(0);
    /* 8. Zero-copy frames: a carried layer takes the camera from the
     * published metadata it was copied from, not from the producer's held
     * arrays, exactly like its epoch and projection. */
    HaloPanoramaGPUSink sink={acquire,release,carry};
    host_panorama_set_gpu_sink(&sink);
    host_panorama_invalidate();
    draw(d,HALO_PANORAMA_MONO_MASK);host_panorama_gpu_carry_missing();
    assert(panorama_info.valid);
    published=panorama_info;host_panorama_gpu_handoff();present();
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)for(int j=0;j<HALO_PANORAMA_POSE_FLOATS;j++)panorama_held_pose[k][j]=-99.f;
    camera(e,13.12f,20.03f,1,0.69f);
    draw(e,1u<<HALO_PANORAMA_CENTRE_LEFT);host_panorama_gpu_carry_missing();
    assert(panorama_info.valid&&same(panorama_info.layer_pose[1],e));
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)
        if(k!=1&&k!=HALO_PANORAMA_HUD_LAYER&&(HALO_PANORAMA_MONO_MASK&(1u<<k))){
            assert(same(panorama_info.layer_pose[k],published.layer_pose[k])&&same(panorama_info.layer_pose[k],d));
            assert(panorama_info.layer_epoch[k]==published.layer_epoch[k]);
        }
    assert(panorama_info.cut_epoch==published.cut_epoch);
    host_panorama_gpu_handoff();present();
    /* 9. The export is informational: a frame drawn with no camera at all
     * still publishes, with the layers it drew marked unknown. */
    host_panorama_set_gpu_sink(NULL);host_panorama_invalidate();
    host_panorama_reset();host_panorama_set_camera(NULL);
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(HALO_PANORAMA_MONO_MASK&(1u<<k)){
        host_panorama_begin(k);host_panorama_projection(1,1,0,0,4,4,0);host_panorama_end(k);
    }
    assert(panorama_info.valid);
    for(int j=0;j<HALO_PANORAMA_POSE_FLOATS;j++)assert(panorama_info.layer_pose[0][j]==0.f);
    puts("PANORAMA_POSE_EXPORT_PASS drawn=frame held=drawn-with carried=published cuts=turn,jump,unusable,scene");
    return 0;
}
