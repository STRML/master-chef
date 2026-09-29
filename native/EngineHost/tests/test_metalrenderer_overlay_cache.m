/* Production cache/descriptor regression with fake Metal compiler objects.
 * Exercises world->HUD and HUD->world reuse without a GPU or game assets.
 * clang -O2 -fobjc-arc -fblocks test_metalrenderer_overlay_cache.m
 *   -framework Foundation -framework Metal -o /tmp/halo-overlay-cache
 */
#include "../metalrenderer.m"
#include <assert.h>

@interface CacheTestPipeline : NSObject
@property MTLBlendFactor sourceAlpha;
@end
@implementation CacheTestPipeline
@end
@interface CacheTestFunction : NSObject
@property MTLFunctionType functionType;
@end
@implementation CacheTestFunction
@end
@interface CacheTestLibrary : NSObject
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
@end
@implementation CacheTestLibrary
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    CacheTestFunction *function=[CacheTestFunction new];
    function.functionType=([name hasPrefix:@"v_"]||[name hasPrefix:@"mr_vs_"])?MTLFunctionTypeVertex:MTLFunctionTypeFragment;
    return (id<MTLFunction>)function;
}
@end
@interface CacheTestDevice : NSObject
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error;
@end
@implementation CacheTestDevice
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
    (void)source; (void)options; (void)error; return (id<MTLLibrary>)[CacheTestLibrary new];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)error; CacheTestPipeline *pipeline=[CacheTestPipeline new];
    pipeline.sourceAlpha=descriptor.colorAttachments[0].sourceAlphaBlendFactor;
    return (id<MTLRenderPipelineState>)pipeline;
}
@end

int ms_translate_shader(const uint32_t *tokens,size_t bytes,int stage,const char *entry,ms_shader *out) {
    (void)tokens; (void)bytes; (void)stage; (void)entry;
    memset(out,0,sizeof *out); out->source=strdup("// fake compiler input\n");
    out->source_bytes=strlen(out->source); return 0;
}
int ms_translate_shader_mapped(const uint32_t *tokens,size_t bytes,int stage,const char *entry,const int *types,int count,ms_shader *out) {
    (void)types; (void)count; return ms_translate_shader(tokens,bytes,stage,entry,out);
}
void ms_shader_destroy(ms_shader *shader) { free(shader->source); memset(shader,0,sizeof *shader); }

static void verify_pair(id<MTLRenderPipelineState> world,id<MTLRenderPipelineState> hud) {
    assert(world&&hud&&world!=hud);
    assert(((CacheTestPipeline *)world).sourceAlpha==MTLBlendFactorSourceAlpha);
    assert(((CacheTestPipeline *)hud).sourceAlpha==MTLBlendFactorOne);
}
static void check_order(int hud_first) {
    mr_shared *shared=calloc(1,sizeof *shared); assert(shared);
    shared->dev=(id<MTLDevice>)[CacheTestDevice new]; shared->lib=(id<MTLLibrary>)[CacheTestLibrary new];
    mr_context world={.s=shared},hud={.s=shared,.overlay_target=1};
    mr_context *first=hud_first?&hud:&world,*second=hud_first?&world:&hud;
    for(int depth=0;depth<2;depth++) {
        id<MTLRenderPipelineState> a=pso_for2(first,MR_BLEND_SRC_ALPHA,15,depth);
        id<MTLRenderPipelineState> b=pso_for2(second,MR_BLEND_SRC_ALPHA,15,depth);
        verify_pair(hud_first?b:a,hud_first?a:b);
        assert(a==pso_for2(first,MR_BLEND_SRC_ALPHA,15,depth));
        assert(b==pso_for2(second,MR_BLEND_SRC_ALPHA,15,depth));
    }
    const uint32_t tokens[]={0xfffe0101,0xffff};
    const uint8_t declaration[]={255,0,0,0,17,0,0,0};
    mr_program_state state={0}; state.vertex_tokens=tokens; state.vertex_token_bytes=sizeof tokens;
    state.pixel_tokens=tokens; state.pixel_token_bytes=sizeof tokens;
    state.declaration=declaration; state.declaration_bytes=sizeof declaration;
    state.blend=MR_BLEND_SRC_ALPHA; state.color_write_mask=15; state.alpha_test_ref=-1;
    mr_program_cache *a=program_for(first,&state,16),*b=program_for(second,&state,16);
    assert(a&&b); verify_pair(hud_first?b->pipeline:a->pipeline,hud_first?a->pipeline:b->pipeline);
    assert(a==program_for(first,&state,16)&&b==program_for(second,&state,16)&&shared->program_count==2);
    /* Translations belong to the pipeline compiler's cache, not the entries. */
    for(unsigned i=0;i<shared->program_count;i++) shared->programs[i].pipeline=nil;
    /* Objective-C strong members need release before freeing C allocation. */
    shared->dev=nil; shared->lib=nil;
    /* Pipeline arrays are deliberately cleaned using their byte extent so this
     * fixture also compiles against the pre-fix two-dimensional cache. */
    size_t count=sizeof shared->pso/sizeof(id<MTLRenderPipelineState>);
    id<MTLRenderPipelineState> __strong *p=(id<MTLRenderPipelineState> __strong *)&shared->pso;
    id<MTLRenderPipelineState> __strong *d=(id<MTLRenderPipelineState> __strong *)&shared->pso_d;
    for(size_t i=0;i<count;i++) { p[i]=nil; d[i]=nil; }
    free(shared);
}
int main(void) {
    @autoreleasepool { check_order(0); check_order(1); }
    puts("PASS: production basic/depth/programmable caches isolate world and HUD alpha in either creation order; no GPU.");
}
