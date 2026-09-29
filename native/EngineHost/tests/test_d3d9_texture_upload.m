/* Real Metal ownership boundary for D3D texture reclamation. Production upload,
 * pending draw, cache removal, target context destruction, and pixel readback. */
#include "../metalrenderer.m"
#include <assert.h>
static void triangle(mr_context *destination,uint32_t texture){
    mr_vertex_rhw v[3]={
        {.x=4,.y=4,.rhw=1,.color=0xffffffff,.u=0,.v=0},
        {.x=60,.y=4,.rhw=1,.color=0xffffffff,.u=1,.v=0},
        {.x=4,.y=60,.rhw=1,.color=0xffffffff,.u=0,.v=1}};
    mr_draw_state state={0};state.texture=texture;state.color_write_mask=15;state.cull_mode=1;state.alpha_test_ref=-1;
    mr_clear(destination,0xff000000);
    assert(mr_draw_rhw(destination,&state,MR_TRIANGLE_LIST,v,sizeof *v,3,NULL,0)==MR_OK);
    assert(destination->s->pending_cb&&destination->s->pending_cb.status==MTLCommandBufferStatusNotEnqueued);
}
static void check(mr_context *destination,unsigned channel){
    uint8_t pixels[64*64*4];assert(mr_read_framebuffer(destination,pixels,sizeof pixels)==0);
    size_t inside=(16*64+16)*4,outside=(56*64+56)*4;
    for(unsigned i=0;i<3;i++){assert(i==channel?pixels[inside+i]>245:pixels[inside+i]<5);assert(pixels[outside+i]<5);}
}
int main(void){@autoreleasepool {
    mr_context *destination=mr_create(64,64);if(!destination){fprintf(stderr,"Metal unavailable: %s\n",mr_last_error());return 77;}
    uint32_t *source=malloc(4*4*4);assert(source);for(unsigned i=0;i<16;i++)source[i]=0xff00ff00;
    uint32_t texture=mr_texture_create_cached_nomip(destination,UINT64_C(0x7000000001),4,4,source,16);assert(texture);
    memset(source,0,64);free(source); /* Upload cannot borrow these CPU bytes. */
    triangle(destination,texture);
    mr_texture_destroy(destination,texture); /* Draw must retain native texture. */
    assert(!destination->s->textures[texture]);check(destination,1);
    mr_context *render_target=mr_create(4,4);assert(render_target);
    mr_clear(render_target,0xff0000ff);texture=mr_target_texture(render_target);assert(texture);
    triangle(destination,texture); /* Sample a GPU-authoritative parent alias. */
    mr_destroy(render_target); /* Final parent destruction before explicit Present. */
    assert(!destination->s->textures[texture]);check(destination,0);
    mr_destroy(destination);
    puts("PASS: real Metal textures retain copied CPU uploads and pending sampled resources after cache removal; render-target context destruction preserves queued sampling, with green/blue triangle and black exterior readback.");return 0;
}}
