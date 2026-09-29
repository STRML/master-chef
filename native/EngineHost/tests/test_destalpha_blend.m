/* Offline production descriptor/cache test: no MTL device, shader compilation,
 * command buffer, or GPU work. The fake device captures pipeline descriptors. */
#include "../metalrenderer.m"
#include <assert.h>
#include <math.h>
@interface DescriptorDevice : NSObject
@property(nonatomic,strong) MTLRenderPipelineDescriptor *captured;
@property(nonatomic) unsigned calls;
@end
@implementation DescriptorDevice
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)error; self.captured=descriptor; self.calls++;
    return (id<MTLRenderPipelineState>)[NSObject new];
}
@end
static double factor(MTLBlendFactor f,double destinationAlpha) {
    if(f==MTLBlendFactorOne)return 1;
    assert(f==MTLBlendFactorDestinationAlpha);return destinationAlpha;
}
static void check(MTLRenderPipelineColorAttachmentDescriptor *ca,int mask) {
    assert(ca.blendingEnabled&&ca.rgbBlendOperation==MTLBlendOperationAdd&&ca.alphaBlendOperation==MTLBlendOperationAdd);
    assert(ca.sourceRGBBlendFactor==MTLBlendFactorDestinationAlpha&&ca.destinationRGBBlendFactor==MTLBlendFactorOne);
    assert(ca.sourceAlphaBlendFactor==MTLBlendFactorDestinationAlpha&&ca.destinationAlphaBlendFactor==MTLBlendFactorOne);
    assert(ca.writeMask==(mask==7?(MTLColorWriteMaskRed|MTLColorWriteMaskGreen|MTLColorWriteMaskBlue):MTLColorWriteMaskAll));
    /* Independent D3D DESTALPHA/ONE ADD expectations. Unequal src/dst alpha
     * catches accidental SRCALPHA or overlay source-over alpha handling. */
    const double src[]={.2,.4,.6,.25},dst[]={.1,.15,.2,.5};
    const double expected[]={.2,.35,.5,.625};
    for(unsigned i=0;i<4;i++) {
        double out=src[i]*factor(i==3?ca.sourceAlphaBlendFactor:ca.sourceRGBBlendFactor,dst[3])+dst[i]*factor(i==3?ca.destinationAlphaBlendFactor:ca.destinationRGBBlendFactor,dst[3]);
        if(i==3&&mask==7)out=dst[3];
        assert(fabs(out-(i==3&&mask==7?.5:expected[i]))<1e-12);
    }
    assert(factor(ca.sourceRGBBlendFactor,0)==0&&factor(ca.sourceRGBBlendFactor,1)==1);
}
int main(void){@autoreleasepool{
    assert(MR_BLEND_MODULATE2==7&&MR_BLEND_DEST_ALPHA_ADD==8&&MR_BLEND_COUNT==9);
    for(int overlay=0;overlay<2;overlay++)for(int mask=7;mask<=15;mask+=8){
        MTLRenderPipelineColorAttachmentDescriptor *ca=[MTLRenderPipelineColorAttachmentDescriptor new];
        configure_program_color(ca,MR_BLEND_DEST_ALPHA_ADD,mask,overlay);check(ca,mask);
        /* Existing source-over overlay behavior remains distinct. */
        ca=[MTLRenderPipelineColorAttachmentDescriptor new];configure_program_color(ca,MR_BLEND_SRC_ALPHA,mask,overlay);
        assert(ca.sourceAlphaBlendFactor==(overlay?MTLBlendFactorOne:MTLBlendFactorSourceAlpha));
    }
    mr_context *ctx=calloc(1,sizeof *ctx);DescriptorDevice *device=[DescriptorDevice new];ctx->dev=(id<MTLDevice>)device;
    for(int depth=0;depth<2;depth++)for(int mask=7;mask<=15;mask+=8){
        id<MTLRenderPipelineState> first=pso_for2(ctx,MR_BLEND_DEST_ALPHA_ADD,mask,depth);assert(first);check(device.captured.colorAttachments[0],mask);
        unsigned calls=device.calls;assert(pso_for2(ctx,MR_BLEND_DEST_ALPHA_ADD,mask,depth)==first&&device.calls==calls);
    }
    assert(device.calls==4);
    /* Release fake cache resources without production teardown/GPU calls. */
    for(int b=0;b<MR_BLEND_COUNT;b++)for(int m=0;m<16;m++){ctx->pso[b][m]=nil;ctx->pso_d[b][m]=nil;}ctx->dev=nil;free(ctx);
    puts("PASS: both production Metal builders, new cache rows/depth variants, exact DESTALPHA/ONE RGB+alpha ADD equations, RGB-only alpha preservation, overlay semantics unchanged; no GPU execution.");
}}
