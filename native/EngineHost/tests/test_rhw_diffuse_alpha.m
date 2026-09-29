#include "../metalrenderer.m"
#include <assert.h>
int main(void){@autoreleasepool{
    mr_context*c=mr_create(16,16);assert(c);
    uint32_t texel=0x204080c0;uint32_t t=mr_texture_create(c,1,1,&texel,4);assert(t);
    mr_vertex_rhw q[4]={0};uint16_t ix[]={0,1,2,2,1,3};uint8_t pixels[16*16*4];
    for(unsigned i=0;i<4;i++){q[i].x=(i&1)?16:0;q[i].y=(i&2)?16:0;q[i].rhw=1;q[i].color=0x80804020;q[i].u=q[i].v=.5;}
    mr_draw_state s={.texture=t,.texture_color_only=1,.color_write_mask=15,.alpha_test_ref=-1,.cull_mode=1};
    for(int mode=0;mode<3;mode++){
        s.texture_color_only=mode!=1;s.blend=mode==2?MR_BLEND_SRC_ALPHA_ADD:MR_BLEND_NONE;mr_clear(c,0);
        assert(!mr_draw_rhw(c,&s,MR_TRIANGLE_LIST,q,sizeof *q,4,ix,6));
        assert(!mr_read_framebuffer(c,pixels,sizeof pixels));
        int expected[]={24,32,32,mode==1?16:128};
        if(mode==2)for(unsigned i=0;i<4;i++)expected[i]=(int)lround(expected[i]*128/255.);
        for(unsigned i=0;i<4;i++){int actual=pixels[(8*16+8)*4+i];if(abs(actual-expected[i])>1){fprintf(stderr,"mode%d channel%u actual%d expected%d\n",mode,i,actual,expected[i]);return 1;}}
    }
    /* The same captured draws use D3DTADDRESS_BORDER, zero border color,
     * bilinear min/mag and linear mip selection. Outside UVs must add no RGB,
     * while wrap/clamp must still sample the edge texel. */
    mr_program_sampler sampler={.address_u=4,.address_v=4,.min_filter=2,.mag_filter=2,.mip_filter=2};
    s.sampler=&sampler;s.texture_color_only=1;s.blend=MR_BLEND_SRC_ALPHA_ADD;
    for(int address=1;address<=4;address++){
        if(address==2)continue;
        sampler.address_u=sampler.address_v=address;
        for(int outside=0;outside<2;outside++){
            for(unsigned i=0;i<4;i++)q[i].u=q[i].v=outside?2.f:.5f;
            mr_clear(c,0);assert(!mr_draw_rhw(c,&s,MR_TRIANGLE_LIST,q,sizeof *q,4,ix,6));
            assert(!mr_read_framebuffer(c,pixels,sizeof pixels));
            int expected[]={12,16,16};
            for(unsigned i=0;i<3;i++){
                int want=(address==4&&outside)?0:expected[i],actual=pixels[(8*16+8)*4+i];
                if(abs(actual-want)>1){fprintf(stderr,"address%d outside%d channel%u actual%d expected%d\n",address,outside,i,actual,want);return 1;}
            }
        }
    }
    sampler.border_color=0xffffffffu;
    assert(mr_draw_rhw(c,&s,MR_TRIANGLE_LIST,q,sizeof *q,4,ix,6)==MR_ERR_UNSUPPORTED);
    mr_destroy(c);puts("PASS: diffuse alpha, legacy alpha, additive blending, captured transparent border and wrap/clamp GPU pixels; unsupported border fails explicitly.");return 0;
}}
