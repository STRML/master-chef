#include "../metalrenderer.m"
#include <assert.h>

int main(void) { @autoreleasepool {
    mr_program_state s={0};
    s.alpha_test_ref=-1;s.color_write_mask=15;s.cull_mode=1;
    s.fixed_stages[0]=(mr_fixed_stage){.color_op=2,.color_arg1=0,.color_arg2=2,
        .alpha_op=2,.alpha_arg1=0,.alpha_arg2=2,.result_arg=1};
    s.fixed_stages[1]=(mr_fixed_stage){.color_op=3,.color_arg1=2,.color_arg2=1,
        .alpha_op=3,.alpha_arg1=2,.alpha_arg2=1,.result_arg=1,
        .texcoord_index=0x10001,.texture_transform_flags=2};
    s.fixed_stages[2].color_op=1;
    ms_shader vs={0};vs.outputs[vs.output_count++]=(ms_semantic){10,0,0};
    assert(!mr_fixed_stage_uses_texture(&s.fixed_stages[0]));
    assert(!mr_fixed_stage_uses_texture(&s.fixed_stages[1]));
    NSString *source=make_fixed_fragment(&vs,&s,@"dependency_fragment");
    if(!source){fprintf(stderr,"fragment: %s\n",mr_last_error());return 1;}
    assert([source rangeOfString:@"texture2d"].location==NSNotFound);
    NSError *error=nil;id<MTLDevice> device=MTLCreateSystemDefaultDevice();assert(device);
    id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
    if(!library){fprintf(stderr,"compile: %s\n",error.localizedDescription.UTF8String);return 1;}
    mr_context *c=mr_create(32,32);assert(c);
    mr_vertex_fixed_rhw q[4]={0};
    for(int i=0;i<4;i++){q[i].x=(i&1)?32:0;q[i].y=(i&2)?32:0;q[i].rhw=1;q[i].color=0xff20c040;}
    uint16_t indices[]={0,1,2,2,1,3};uint8_t pixels[32*32*4];
    assert(mr_draw_fixed_rhw(c,&s,MR_TRIANGLE_LIST,q,sizeof q[0],4,indices,6)==0);
    assert(mr_read_framebuffer(c,pixels,sizeof pixels)==0);
    size_t center=(16*32+16)*4;
    assert(pixels[center]==64&&pixels[center+1]==192&&pixels[center+2]==32&&pixels[center+3]==255);
    /* A genuinely consumed missing coordinate still fails rather than being invented. */
    s.fixed_stages[1].texcoord_index=1;s.fixed_stages[1].texture_transform_flags=0;
    s.fixed_stages[1].color_op=2;
    assert(mr_fixed_stage_uses_texture(&s.fixed_stages[1]));
    assert(make_fixed_fragment(&vs,&s,@"missing_coordinate")==nil);
    for(unsigned op=13;op<=15;op+=2)assert(mr_fixed_op_uses_texture(op,0,1,1));
    assert(mr_fixed_op_uses_texture(25,2,1,1));
    assert(mr_fixed_op_uses_texture(26,2,1,1));
    assert(!mr_fixed_op_uses_texture(4,2,1,1));
    mr_destroy(c);
    puts("PASS: unused texture operands require no texture or TEXCOORD; live dependencies retained; GPU pixel matches.");
    return 0;
} }
