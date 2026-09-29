#include "../metalrenderer.m"
#include <assert.h>
int main(void){@autoreleasepool{
    mr_context *c=mr_create(16,16);assert(c);unsigned failures=0;
    mr_vertex_fixed_rhw q[4]={0};uint16_t ix[]={0,1,2,2,1,3};uint8_t pixels[16*16*4];
    for(unsigned i=0;i<4;i++){q[i].x=(i&1)?16:0;q[i].y=(i&2)?16:0;q[i].rhw=1;q[i].color=0xffa0c030;}
    mr_program_state s={0};s.color_write_mask=15;s.alpha_test_ref=-1;s.cull_mode=1;
    s.texture_factor=0x40205080;s.fixed_stages[1].color_op=1;
    s.fixed_stages[0]=(mr_fixed_stage){.color_arg1=3,.color_arg2=0,.alpha_op=2,.alpha_arg1=0,.result_arg=1};
    const unsigned operations[]={18,20,21};const double a[]={32/255.,80/255.,128/255.},b[]={160/255.,192/255.,48/255.};
    for(unsigned i=0;i<3;i++){
        unsigned op=operations[i];s.fixed_stages[0].color_op=op;mr_clear(c,0);
        assert(!mr_draw_fixed_rhw(c,&s,MR_TRIANGLE_LIST,q,sizeof *q,4,ix,6));
        assert(!mr_read_framebuffer(c,pixels,sizeof pixels));
        for(unsigned rgb=0;rgb<3;rgb++){
            /* Microsoft D3DTEXTUREOP equations, independently computed. */
            double value=op==18?a[rgb]+(64/255.)*b[rgb]:op==20?a[rgb]+(1-64/255.)*b[rgb]:(1-a[rgb])*b[rgb]+64/255.;
            int expected=(int)lround(fmin(1.,fmax(0.,value))*255.);
            int actual=pixels[(8*16+8)*4+(2-rgb)];
            if(abs(actual-expected)>1){fprintf(stderr,"op%u rgb%u actual%d expected%d\n",op,rgb,actual,expected);failures++;}
        }
    }
    mr_destroy(c);printf("%s: Direct3D modulate/add operations18/20/21 match GPU pixels (%u failures).\n",failures?"FAIL":"PASS",failures);return failures?1:0;
}}
