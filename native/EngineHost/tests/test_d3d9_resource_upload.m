/* Real offscreen Metal draw: release/overwrite source VB/IB bytes while the
 * production renderer's command buffer is still uncommitted. No game assets. */
#include "../metalrenderer.m"
#include <assert.h>
int main(void){@autoreleasepool {
    mr_context *c=mr_create(64,64);if(!c){fprintf(stderr,"Metal unavailable: %s\n",mr_last_error());return 77;}
    mr_vertex_rhw *v=calloc(3,sizeof *v);uint16_t *ix=malloc(3*sizeof *ix);assert(v&&ix);
    v[0]=(mr_vertex_rhw){.x=4,.y=4,.rhw=1,.color=0xffff0000};
    v[1]=(mr_vertex_rhw){.x=60,.y=4,.rhw=1,.color=0xffff0000};
    v[2]=(mr_vertex_rhw){.x=4,.y=60,.rhw=1,.color=0xffff0000};
    ix[0]=0;ix[1]=1;ix[2]=2;
    mr_draw_state state={0};state.color_write_mask=15;state.cull_mode=1;state.alpha_test_ref=-1;
    mr_clear(c,0xff000000);assert(mr_draw_rhw(c,&state,MR_TRIANGLE_LIST,v,sizeof *v,3,ix,3)==MR_OK);
    assert(c->s->pending_cb&&c->s->pending_cb.status==MTLCommandBufferStatusNotEnqueued);
    /* A stale CPU pointer would now supply green degenerate geometry/indices. */
    memset(v,0,3*sizeof *v);v[0].color=v[1].color=v[2].color=0xff00ff00;memset(ix,0,3*sizeof *ix);free(v);free(ix);
    uint8_t pixels[64*64*4];assert(mr_read_framebuffer(c,pixels,sizeof pixels)==0);
    size_t inside=(16*64+16)*4,outside=(56*64+56)*4;
    assert(pixels[inside+2]>245&&pixels[inside+1]<5&&pixels[inside]<5);
    assert(pixels[outside]<5&&pixels[outside+1]<5&&pixels[outside+2]<5);
    mr_destroy(c);puts("PASS: real pending Metal indexed RHW draw survives source-byte overwrite/free before GPU commit, with red triangle/black exterior readback.");return 0;
}}
