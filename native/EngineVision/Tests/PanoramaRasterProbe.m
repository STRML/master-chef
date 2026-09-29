#import <Foundation/Foundation.h>
#include "metalrenderer.h"
#include <assert.h>
#include <stdio.h>

int main(void) { @autoreleasepool {
    mr_context *context=mr_create(64,64);
    if(!context){fprintf(stderr,"Metal raster probe unavailable: %s\n",mr_last_error());return 2;}
    mr_vertex_rhw vertices[]={ {4,4,0.5,-1,0xFFFF0000,0,0}, {60,4,0.5,-1,0xFFFF0000,0,0}, {4,60,0.5,-1,0xFFFF0000,0,0} };
    mr_draw_state state={.color_write_mask=15,.alpha_test_ref=-1};
    uint32_t pixels[64*64];
    mr_clear(context,0);
    assert(mr_draw_rhw(context,&state,MR_TRIANGLE_LIST,vertices,sizeof vertices[0],3,NULL,0)==0);
    assert(mr_read_framebuffer(context,pixels,sizeof pixels)==0);
    for(int i=0;i<64*64;i++)assert(pixels[i]==0); // behind-camera triangles must be clipped
    mr_destroy(context);context=mr_create(64,64);assert(context);
    /* Exercise the same HUD-target reset used at the start of every panorama
     * pass: replacing guest bytes must also replace the live Metal target, or
     * pause can expose the previous smaller scene behind its controls. */
    for(int i=0;i<64*64;i++)pixels[i]=0xFF4A4A4Au;
    assert(mr_write_framebuffer(context,pixels,sizeof pixels)==0);
    assert(mr_read_framebuffer(context,pixels,sizeof pixels)==0);
    assert(pixels[0]==0xFF4A4A4Au&&pixels[63*64+63]==0xFF4A4A4Au);
    memset(pixels,0,sizeof pixels);
    assert(mr_write_framebuffer(context,pixels,sizeof pixels)==0);
    assert(mr_read_framebuffer(context,pixels,sizeof pixels)==0);
    for(int i=0;i<64*64;i++)assert(pixels[i]==0); // reset clears prior HUD target contents
    mr_set_overlay_target(context);mr_clear(context,0);state.blend=MR_BLEND_SRC_ALPHA;
    for(int i=0;i<3;i++){vertices[i].rhw=1;vertices[i].color=0x80FF0000;}
    assert(mr_draw_rhw(context,&state,MR_TRIANGLE_LIST,vertices,sizeof vertices[0],3,NULL,0)==0);
    assert(mr_read_framebuffer(context,pixels,sizeof pixels)==0);
    uint32_t first=pixels[8*64+8];assert((first>>24)>=127&&(first>>24)<=129);
    for(int i=0;i<3;i++)vertices[i].color=0x800000FF;
    assert(mr_draw_rhw(context,&state,MR_TRIANGLE_LIST,vertices,sizeof vertices[0],3,NULL,0)==0);
    assert(mr_read_framebuffer(context,pixels,sizeof pixels)==0);
    uint32_t value=pixels[8*64+8];
    assert((value>>24)>=191&&(value>>24)<=193);
    assert(((value>>16)&255)>=63&&((value>>16)&255)<=65);
    assert((value&255)>=127&&(value&255)<=129);
    mr_destroy(context);
    puts("PASS: behind-camera clipping, HUD target reset after prior content, and transparent HUD source-over coverage on Metal GPU.");
    return 0;
} }
