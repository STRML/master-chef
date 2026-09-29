/* Focused implicit-surface and renderer ownership test for IDirect3DDevice9::Reset. */
#include "../d3d9.c"
#include <assert.h>
/* This fixture never destroys a live Metal context. */
void mr_destroy(mr_context *context){(void)context;abort();}
/* No resident vertex copy is ever taken here: nothing draws. */
void mr_buffer_release(void *buffer){(void)buffer;abort();}


uint8_t *engine_flat_base;
/* This fixture never owns reclaimable guest allocations. */
void guest_free(uint32_t p){(void)p;abort();}

static uint32_t test_page_top = 0x10000u;
static int resize_calls, resize_failure;
static int resized_w, resized_h;
static uint32_t freed_pages[8];static unsigned freed_count;
int guest_page_free(uint32_t base) { assert(base&&freed_count<8);freed_pages[freed_count++]=base;return 1; }
uint32_t guest_page_alloc(uint32_t size) {
    uint32_t result=test_page_top,need=(size+0xffffu)&~0xffffu;
    assert((uint64_t)test_page_top+need<0x02000000u);test_page_top+=need;return result;
}
int mr_resize(mr_context *context,int width,int height) {
    assert(context==(mr_context *)(uintptr_t)0x1234u);resize_calls++;resized_w=width;resized_h=height;return resize_failure;
}
void host_log(const char *format, ...) { (void)format; }

static void put_index(uint32_t guest,uint32_t index){S32(guest+4,index);}
int main(void) {
    engine_flat_base=calloc(1,0x02000000u);assert(engine_flat_base);
    obj_count=4;memset(objs,0,sizeof objs);
    D3DObj *device=&objs[1],*backbuffer=&objs[2],*depth=&objs[3];
    device->kind=K_DEVICE;device->guest=0x1000;device->data=0x1100;device->level_surface[0]=0x1200;
    backbuffer->kind=K_SURFACE;backbuffer->guest=0x1100;backbuffer->width=640;backbuffer->height=480;backbuffer->format=FMT_X8R8G8B8;
    backbuffer->size=640*480*4;backbuffer->data_capacity=backbuffer->size;backbuffer->data=guest_page_alloc(backbuffer->data_capacity);
    backbuffer->renderer=(mr_context *)(uintptr_t)0x1234u;backbuffer->gpu_dirty=1;
    depth->kind=K_SURFACE;depth->guest=0x1200;depth->width=640;depth->height=480;depth->format=75;depth->size=640*480*4;
    depth->data_capacity=depth->size;depth->data=guest_page_alloc(depth->data_capacity);depth->gpu_dirty=1;
    put_index(device->guest,1);put_index(backbuffer->guest,2);put_index(depth->guest,3);
    the_device=device;current_rt_guest=0x7777;backbuffer_w=640;backbuffer_h=480;
    uint32_t pp=0x2000;S32(pp,1280);S32(pp+4,960);S32(pp+8,0);S32(pp+40,75);
    draw_state.texture[0]=0xdeadbeefu;draw_state.viewport[2]=7;current_material.diffuse[0]=1.f;panorama_mask=7;panorama_info.valid=1;
    panorama_ready_mask=HALO_PANORAMA_MONO_MASK;panorama_ready_width=640;panorama_ready_height=480;
    uint32_t old_depth_data=depth->data;

    resize_failure=1;uint32_t old_data=backbuffer->data;
    assert(reset_device(device,pp)==D3DERR_NOTAVAILABLE);assert(backbuffer->data==old_data&&backbuffer->width==640);
    assert(backbuffer_w==640&&current_rt_guest==0x7777&&panorama_mask==7&&panorama_info.valid);
    assert(!freed_count&&host_panorama_all_layers_ready());

    resize_failure=0;assert(reset_device(device,pp)==D3D_OK);
    assert(resize_calls==2&&resized_w==1280&&resized_h==960);
    assert(G32(pp)==1280&&G32(pp+4)==960&&G32(pp+8)==FMT_X8R8G8B8);
    assert(backbuffer->width==1280&&backbuffer->height==960&&backbuffer->format==FMT_X8R8G8B8&&backbuffer->usage==1);
    assert(depth->width==1280&&depth->height==960&&depth->format==75&&depth->usage==2);
    assert(backbuffer->size==1280*960*4&&depth->size==1280*960*4);
    assert(backbuffer->renderer==(mr_context *)(uintptr_t)0x1234u&&!backbuffer->gpu_dirty&&!depth->gpu_dirty);
    assert(current_rt_guest==device->data&&backbuffer_w==1280&&backbuffer_h==960);
    assert(draw_state.viewport[0]==0&&draw_state.viewport[1]==0&&draw_state.viewport[2]==1280&&draw_state.viewport[3]==960);
    assert(draw_state.texture[0]==0&&draw_state.rs[7]==1&&draw_state.rs[168]==15&&current_material.diffuse[0]==0.f);
    assert(panorama_mask==0&&!panorama_info.valid);
    assert(!host_panorama_all_layers_ready()&&freed_count==2&&freed_pages[0]==old_data&&freed_pages[1]==old_depth_data);

    uint32_t bb_storage=backbuffer->data,depth_storage=depth->data,page_before=test_page_top;
    S32(pp,800);S32(pp+4,600);assert(reset_device(device,pp)==D3D_OK);
    assert(backbuffer->data==bb_storage&&depth->data==depth_storage&&test_page_top==page_before);
    assert(backbuffer->size==800*600*4&&depth->size==800*600*4&&draw_state.viewport[2]==800&&draw_state.viewport[3]==600);
    assert(resize_calls==3&&resized_w==800&&resized_h==600);
    assert(freed_count==2);

    panorama_ready_mask=HALO_PANORAMA_MONO_MASK;panorama_ready_width=800;panorama_ready_height=600;
    S32(pp,0);S32(pp+4,0);assert(reset_device(device,pp)==D3D_OK);
    assert(G32(pp)==800&&G32(pp+4)==600&&resize_calls==4);
    assert(!host_panorama_all_layers_ready()&&freed_count==2); /* same-size reset */
    assert(reset_device(device,0)==D3DERR_INVALIDCALL);
    free(engine_flat_base);
    puts("PASS: D3D9 Reset retains renderer, releases replaced owned pages, reuses smaller storage, invalidates same-size panorama history, and preserves state on resize failure.");
    return 0;
}
