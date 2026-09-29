/* Real-GPU regressions for independent texture addressing and bounded state
 * caches. Reuse the synthetic shader boundary from the draw-path fixture. */
#define main renderer_fixture_main
#include "test_metalrenderer_fastpaths.m"
#undef main

static void sample_at(mr_context *c, uint32_t texture, mr_program_sampler sampler,
                      float u, float v) {
    mr_vertex_rhw vertices[3] = {
        {0, 0, 0.5f, 1, 0xFFFFFFFFu, u, v},
        {SIZE * 2, 0, 0.5f, 1, 0xFFFFFFFFu, u, v},
        {0, SIZE * 2, 0.5f, 1, 0xFFFFFFFFu, u, v}
    };
    mr_draw_state state = {.texture=texture, .sampler=&sampler,
        .color_write_mask=15, .alpha_test_ref=-1, .cull_mode=1};
    assert(mr_draw_rhw(c, &state, MR_TRIANGLE_LIST, vertices,
                       sizeof vertices[0], 3, NULL, 0) == MR_OK);
}

static int coordinate(float value, unsigned mode) {
    if (mode == 4 && (value < 0 || value > 1)) return -1;
    if (mode == 1) value -= floorf(value);
    else if (mode == 2) {
        value = fmodf(fabsf(value), 2.f);
        if (value > 1) value = 2 - value;
    } else if (mode == 5) value = fabsf(value);
    return value < .5f ? 0 : 1;
}

int main(int argc, char **argv) {
 @autoreleasepool {
    setenv("HALO_PIPELINE_CACHE", "0", 1);
    setenv("HALO_PIPELINE_WORKERS", "0", 1);
    setenv("HALO_NO_ANISO", "1", 1);
    mr_context *c = mr_create(SIZE, SIZE);
    if (!c) { puts("SKIP no Metal device"); return 77; }
    const char *mode = argc > 1 ? argv[1] : "all";
    if (!strcmp(mode, "all") || !strcmp(mode, "sampler")) {
        uint32_t green=0xFF00FF00u;
        uint32_t texture=mr_texture_create(c,1,1,&green,4); assert(texture);
        for (unsigned i=0; i<768; ++i) {
            mr_program_sampler s={.type=MR_SAMPLER_2D, .address_u=1,
                .address_v=1, .min_filter=1+(i/256)%2, .mag_filter=1,
                .mip_filter=1, .max_mip_level=i%256};
            if (!sampler_for(c, &s)) { fprintf(stderr,"FAIL sampler state %u: %s\n",i,mr_last_error()); return 1; }
            sample_at(c,texture,s,.25f,.25f);
        }
        assert(c->s->program_sampler_count <= MR_MAX_PROGRAM_SAMPLERS);
        expect_pixel(c,20,20,green);
        mr_texture_destroy(c,texture);
        puts("PASS sampler pressure: 768 requests stay bounded and succeed");
    }
    if (!strcmp(mode, "all") || !strcmp(mode, "depth")) {
        /* Keep earlier encoded states alive while the cache is replaced. */
        mr_clear(c, 0xFF000000u); mr_clear_depth(c, 1.f);
        for (unsigned i=0; i<192; ++i) {
            if (!depth_stencil_state_for(c,1,4,1,1,1,1,1,8,i,255-i)) {
                fprintf(stderr,"FAIL depth state %u: %s\n",i,mr_last_error()); return 1;
            }
            mr_vertex_rhw vertices[3]={{0,0,.5f,1,0xFF00FF00u,0,0},
                {SIZE*2,0,.5f,1,0xFF00FF00u,0,0},{0,SIZE*2,.5f,1,0xFF00FF00u,0,0}};
            mr_draw_state state={.color_write_mask=15,.alpha_test_ref=-1,.cull_mode=1,
                .depth_enable=1,.depth_compare=4,.depth_write=1,.stencil_enable=1,
                .stencil_fail=1,.stencil_depth_fail=1,.stencil_pass=1,.stencil_compare=8,
                .stencil_read_mask=i,.stencil_write_mask=255-i};
            assert(mr_draw_rhw(c,&state,MR_TRIANGLE_LIST,vertices,sizeof vertices[0],3,NULL,0)==MR_OK);
        }
        assert(c->s->depth_stencil_count <= 64);
        expect_pixel(c,20,20,0xFF00FF00u);
        puts("PASS depth/stencil pressure: 192 states, bounded cache, queued draw pixels intact");
    }
    if (!strcmp(mode, "all") || !strcmp(mode, "address")) {
        uint32_t pixels[]={0xFFFF0000u,0xFF00FF00u,0xFF0000FFu,0xFFFFFFFFu};
        uint32_t texture=mr_texture_create(c,2,2,pixels,8); assert(texture);
        const float probes[]={-2.25f,-1.25f,-.75f,-.25f,.25f,.75f,1.25f,1.75f,2.25f};
        for (int fast=0; fast<=1; ++fast) {
            mr_set_fast_paths(fast);
            for (unsigned u=1; u<=5; ++u) for (unsigned v=1; v<=5; ++v) {
                mr_program_sampler s={.type=MR_SAMPLER_2D, .address_u=u,
                    .address_v=v, .min_filter=1, .mag_filter=1};
                if (!sampler_for(c,&s)) { fprintf(stderr,"FAIL addressing %u,%u: %s\n",u,v,mr_last_error()); return 1; }
                for (unsigned p=0; p<sizeof probes/sizeof *probes; ++p) {
                    float pu=probes[p],pv=probes[8-p];
                    sample_at(c,texture,s,pu,pv);
                    int x=coordinate(pu,u),y=coordinate(pv,v);
                    expect_pixel(c,20,20,x<0||y<0?0:pixels[y*2+x]);
                }
            }
        }
        mr_texture_destroy(c,texture);
        puts("PASS independent U/V addressing: 450 exact GPU pixel checks, both draw paths");
    }
    mr_destroy(c);
 }
 return 0;
}
