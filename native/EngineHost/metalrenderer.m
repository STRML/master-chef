#include "texture_mips.h"
/* metalrenderer.m - see metalrenderer.h. Metal + Foundation only (macOS / visionOS). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <os/lock.h>
#include "metalrenderer.h"
#include "metalshader.h"
#include "radial_fog.h"
#include "halo_settings.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define MR_MAX_VERTS 65536u
#define MR_MAX_INDICES (3u * 65536u)
#define MR_MAX_TEX 4096
#define MR_MAX_PROGRAMS 4096
#define MR_MAX_FIXED_RHW_PROGRAMS 2048
#define MR_PROGRAM_SLOTS 8192u                   /* > 2 * MR_MAX_PROGRAMS, power of two */
#define MR_FIXED_RHW_SLOTS 4096u                 /* > 2 * MR_MAX_FIXED_RHW_PROGRAMS, power of two */
#define MR_MAX_PROGRAM_SAMPLERS 256
#define MR_MAX_VERTEX_STRIDE 256
#define MR_MAX_CACHED_TEXTURES 1024u
#ifndef MR_MAX_CACHED_TEXTURE_BYTES
#define MR_MAX_CACHED_TEXTURE_BYTES (UINT64_C(512) * 1024u * 1024u)
#endif
#define MR_KEY_SLOTS 16384u                      /* > 2 * MR_MAX_TEX, power of two */

static const char *g_err = "";
static char g_errbuf[1024];
static NSString *const kSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VIn { float4 pos; float4 color; float2 uv; float2 pad; };\n"  /* 48 bytes, matches host VIn */
"struct U { float2 size; int tex_enabled; int alpha_only; int alpha_ref; int color_only; };\n"
"struct VOut { float4 pos [[position]]; float2 uv; float4 color; };\n"
"vertex VOut v_main(uint vid [[vertex_id]], const device VIn *v [[buffer(0)]], constant U &u [[buffer(1)]]) {\n"
"    VIn i = v[vid]; VOut o;\n"
"    float rhw = abs(i.pos.w) > 1.0e-20 ? i.pos.w : 1.0; float w = 1.0 / rhw;\n"
"    float2 ndc = float2(i.pos.x / u.size.x * 2.0 - 1.0, 1.0 - i.pos.y / u.size.y * 2.0);\n"
"    o.pos = float4(ndc * w, i.pos.z * w, w);\n"      /* perspective-correct interpolation of uv/colour */
"    o.uv = i.uv; o.color = i.color; return o;\n"
"}\n"
"fragment float4 f_main(VOut in [[stage_in]], constant U &u [[buffer(1)]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {\n"
"    float4 c = in.color;\n"
"    if (u.tex_enabled) { float4 t = tex.sample(s, in.uv); c = u.alpha_only ? float4(c.rgb, c.a * t.a) : u.color_only ? float4(c.rgb * t.rgb, c.a) : c * t; }\n"
"    if (u.alpha_ref >= 0 && c.a * 255.0 < float(u.alpha_ref)) discard_fragment();\n"
"    return c;\n"
"}\n";

typedef struct { float pos[4]; float color[4]; float uv[2]; float pad[2]; } VIn;   /* 48 bytes */
typedef struct { float size[2]; int tex_enabled, alpha_only, alpha_ref, color_only; } U;   /* 24 bytes */

/* Pipeline cache entries. `state` (MR_PL_*) and the fields it publishes
 * change only under the pipeline compiler's lock; an entry never moves or
 * goes away, so a READY entry is read without the lock. vs/ps point at
 * translations the compiler owns; ps is an empty shader when there is none. */
typedef struct mr_program_cache {
    uint64_t key;
    int state, background;          /* background: built by a worker and not drawn yet */
    const ms_shader *vs, *ps;
    int has_pixel_shader;
    int extras;                     /* fragment expects the MRPixelExtras buffer at index 1 (alpha test / fog) */
    uint32_t streams;               /* bit s set: vertex buffer index 1+s is bound from stream s */
    id<MTLRenderPipelineState> pipeline;
    const struct mr_recipe *recipe; /* the manifest recipe it was queued from, if any */
    char *error; uint64_t failed_at; int attempts;
} mr_program_cache;

typedef struct mr_custom_rhw_cache {
    int src, dst, op, mask, overlay;
    id<MTLRenderPipelineState> pipeline;
} mr_custom_rhw_cache;

typedef struct mr_fixed_rhw_cache {
    uint64_t key;
    int state, background;
    const ms_shader *ps;            /* translated pixel shader when has_ps, else empty */
    int has_ps, extras;
    id<MTLRenderPipelineState> pipeline;
    const struct mr_recipe *recipe;
    char *error; uint64_t failed_at; int attempts;
} mr_fixed_rhw_cache;

typedef struct mr_depth_stencil_cache {
    int depth_enable, depth_compare, depth_write;
    int stencil_enable, stencil_fail, stencil_depth_fail, stencil_pass, stencil_compare;
    uint32_t stencil_read_mask, stencil_write_mask;
    id<MTLDepthStencilState> state;
} mr_depth_stencil_cache;

typedef struct mr_sampler_cache {
    uint8_t address, min_filter, mag_filter, mip_filter;
    uint8_t max_anisotropy, max_mip_level;
    id<MTLSamplerState> state;
} mr_sampler_cache;

/* State the open draw encoder already holds. Metal keeps an encoder's state
 * from draw to draw, and consecutive D3D draws mostly repeat it (one viewport
 * for a whole pass, runs of one pipeline, the same textures), so a repeated
 * value is not sent again. Forgotten whenever a new encoder starts. */
typedef struct mr_bound_state {
    id<MTLRenderPipelineState> pipeline;
    id<MTLDepthStencilState> depth;
    int stencil_ref, cull, winding;     /* -1 unknown */
    int viewport_valid, scissor_valid;
    MTLViewport viewport; MTLScissorRect scissor;
    id<MTLTexture> textures[16];
    id<MTLSamplerState> samplers[16];
} mr_bound_state;

/* One Metal device/queue/library and all caches are shared by every D3D
 * surface; a context is a render target view onto that shared state so
 * render-target textures can be sampled directly and one command buffer
 * carries the whole frame in submission order. */
typedef struct mr_shared {
    id<MTLDevice> dev; id<MTLCommandQueue> queue; id<MTLLibrary> lib;
    id<MTLRenderPipelineState> pso[2][MR_BLEND_COUNT][16];   /* [overlay][blend][write mask] */
    id<MTLRenderPipelineState> pso_d[2][MR_BLEND_COUNT][16]; /* retained ABI split; all draw passes now attach depth */
    id<MTLSamplerState> samplers[4];         /* bit0 linear, bit1 clamp */
    mr_sampler_cache program_samplers[MR_MAX_PROGRAM_SAMPLERS];
    uint32_t program_sampler_count;
    id<MTLTexture> white;
    id<MTLTexture> black2d, blackcube, black3d;   /* D3D samples an unbound stage as opaque black */
    id<MTLTexture> textures[MR_MAX_TEX];
    uint64_t texture_keys[MR_MAX_TEX], texture_use[MR_MAX_TEX], texture_bytes[MR_MAX_TEX], texture_tick;
    uint64_t cached_texture_bytes;
    uint32_t cached_texture_count;
    uint32_t key_slots[MR_KEY_SLOTS];        /* open-addressed key -> texture id */
    id<MTLCommandBuffer> pending_cb;
    id<MTLRenderCommandEncoder> pending_encoder;
    id<MTLCommandBuffer> last_cb;
    mr_depth_stencil_cache depth_stencil_states[64];
    uint32_t depth_stencil_count;
    mr_program_cache programs[MR_MAX_PROGRAMS];
    uint32_t program_count;
    uint32_t program_index[MR_PROGRAM_SLOTS];          /* open-addressed key -> programs index + 1 */
    struct mr_context *clear_list;          /* contexts holding clears not yet encoded */
    mr_bound_state bound;                   /* what pending_encoder already has set */
    mr_fixed_rhw_cache fixed_rhw_programs[MR_MAX_FIXED_RHW_PROGRAMS];
    uint32_t fixed_rhw_program_count;
    uint32_t fixed_rhw_index[MR_FIXED_RHW_SLOTS];
    struct mr_compiler *compiler;           /* shader/pipeline build service, created on first use */
    mr_custom_rhw_cache custom_rhw[32];
    uint32_t custom_rhw_count;
    struct mr_context *pending_target;      /* context whose target the open encoder draws into */
    id<MTLBuffer> arena; size_t arena_used, arena_size;   /* transient vertex/index storage for the pending command buffer */
    id<MTLRenderPipelineState> fxaa_pipeline; MTLPixelFormat fxaa_format; int fxaa_failed;   /* publish-time anti-aliasing */
    uint32_t contexts;
} mr_shared;
static mr_shared *g_shared;
/* The D3D bridge resolves all sampler IDs before encoding a draw. Protect
 * those IDs from cache eviction while later stages upload their textures.
 * GPU command-buffer retention only protects draws already encoded. */
static unsigned texture_binding_depth;
static uint64_t texture_binding_floor;
static uint64_t texture_cache_hits, texture_cache_uploads, texture_cache_evictions;
void mr_texture_bindings_begin(void) {
    if (!texture_binding_depth++) texture_binding_floor=g_shared?g_shared->texture_tick:0;
}
void mr_texture_bindings_end(void) { if(texture_binding_depth) --texture_binding_depth; }
void mr_texture_cache_counters(uint64_t *hits,uint64_t *uploads,uint64_t *evictions) {
    if(hits)*hits=texture_cache_hits;if(uploads)*uploads=texture_cache_uploads;if(evictions)*evictions=texture_cache_evictions;
}
/* Why the most recent committed command buffer failed, if it did. */
static char g_commit_err[256];
const char *mr_last_commit_error(void) { return g_commit_err; }
struct mr_context {
    mr_shared *s;
    id<MTLTexture> target; id<MTLTexture> depth_target;
    uint32_t target_id;                     /* shared texture-table slot aliasing the color target */
    int w, h, vx, vy, vw, vh;
    int overlay_target;
    uint64_t tris;
    /* A clear used to be a render pass of its own: a Metal encoder on the
     * CPU, and on the GPU a full store of the attachment that the next pass
     * loaded straight back. Clears now wait here and become the load action
     * of the next pass that draws into this target. Anything else that
     * touches the target (sampling, blits, readback, commit) encodes them
     * first, so every consumer still sees them in submission order. */
    int pending_clear;                      /* bit 0 colour, 1 depth, 2 stencil */
    uint32_t pending_clear_calls;           /* clear calls behind pending_clear, for the traffic stats */
    MTLClearColor clear_color; double clear_depth; uint32_t clear_stencil;
    struct mr_context *next_clear;
};

void mr_set_overlay_target(mr_context *c) { if(c)c->overlay_target=1; }

static _Atomic int g_fast_paths = -1;
int mr_fast_paths_enabled(void) {
    int enabled = atomic_load_explicit(&g_fast_paths, memory_order_relaxed);
    if (enabled < 0) {
        const char *e = getenv("HALO_DRAW_FASTPATH");
        int wanted = e && !strcmp(e, "1");
        atomic_compare_exchange_strong_explicit(&g_fast_paths, &enabled, wanted,
                                                memory_order_relaxed, memory_order_relaxed);
        enabled = atomic_load_explicit(&g_fast_paths, memory_order_relaxed);
    }
    return enabled;
}
static void encode_pending_clears(mr_shared *s);
void mr_set_fast_paths(int enabled) {
    if (g_shared) encode_pending_clears(g_shared);
    atomic_store_explicit(&g_fast_paths, enabled ? 1 : 0, memory_order_relaxed);
}
static _Atomic uint64_t g_resident_bytes, g_arena_vertex_bytes, g_folded_clears;
void mr_draw_traffic_stats(uint64_t *resident_bytes, uint64_t *arena_vertex_bytes, uint64_t *folded_clears) {
    if (resident_bytes) *resident_bytes = atomic_load_explicit(&g_resident_bytes, memory_order_relaxed);
    if (arena_vertex_bytes) *arena_vertex_bytes = atomic_load_explicit(&g_arena_vertex_bytes, memory_order_relaxed);
    if (folded_clears) *folded_clears = atomic_load_explicit(&g_folded_clears, memory_order_relaxed);
}

static void key_slot_insert(mr_context *c, uint64_t key, uint32_t id);
static void key_slot_remove(mr_context *c, uint64_t key);
static int mr_primitive_supported(int prim);
static MTLPrimitiveType mr_primitive_metal(int prim);
static uint32_t mr_primitive_shapes(int prim, uint32_t count);
static int mr_no_aniso(void);
static id<MTLTexture> ps_sampler_texture(mr_context *c, const mr_program_state *state, int stage, int declared);
/* Off unless halo_settings.c is linked (the app and the probe link it). */
__attribute__((weak)) int halo_settings_radial_fog(void) { return 0; }

static struct mr_compiler *compiler_for(mr_shared *s);
static void mr_pipeline_store_open(struct mr_compiler *cc);
static void compiler_prewarm_builtin(struct mr_compiler *cc);
/* Background pipeline workers report into their own buffer; the engine
 * thread (no sink) keeps writing the process-wide mr_last_error(). */
static _Thread_local char *mr_err_sink;
static _Thread_local size_t mr_err_sink_size;
static void set_errorf(const char *format, ...) {
    va_list args; va_start(args, format);
    if (mr_err_sink) { vsnprintf(mr_err_sink, mr_err_sink_size, format, args); va_end(args); return; }
    vsnprintf(g_errbuf, sizeof g_errbuf, format, args); va_end(args); g_err = g_errbuf;
}

const char *mr_last_error(void) { return g_err; }
int mr_width(const mr_context *c) { return c ? c->w : 0; }
int mr_height(const mr_context *c) { return c ? c->h : 0; }
uint64_t mr_triangles_drawn(const mr_context *c) { return c ? c->tris : 0; }
uint32_t mr_cached_texture_count(const mr_context *c) { return c ? c->s->cached_texture_count : 0; }
uint64_t mr_cached_texture_bytes(const mr_context *c) { return c ? c->s->cached_texture_bytes : 0; }

static int make_target(mr_context *c, int w, int h) {
    @autoreleasepool {
    mr_shared *s = c->s;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [s->dev newTextureWithDescriptor:td];
    if (!target) { g_err = "render target allocation failed"; return -1; }
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:w height:h mipmapped:NO];
    dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth_target = [s->dev newTextureWithDescriptor:dd];
    if (!depth_target) { g_err = "depth target allocation failed"; return -1; }
    c->target = target; c->depth_target = depth_target;
    /* Alias the color target in the shared texture table (uncached, never evicted). */
    if (!c->target_id) { for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!s->textures[i]) { c->target_id = i; break; } }
    if (!c->target_id) { g_err = "texture table full"; return -1; }
    s->textures[c->target_id] = target; s->texture_keys[c->target_id] = 0; s->texture_bytes[c->target_id] = 0;
    c->w = w; c->h = h; c->vx = 0; c->vy = 0; c->vw = w; c->vh = h; return 0;
    }
}

static void configure_program_color(MTLRenderPipelineColorAttachmentDescriptor *ca, int blend, int src, int dst, int op, int mask, int overlay);
static id<MTLRenderPipelineState> pso_for2(mr_context *c, int blend, int mask, int depth) {
    @autoreleasepool {
    if (blend < 0 || blend >= MR_BLEND_COUNT) blend = 0; mask &= 0xF;
    /* HUD alpha accumulates coverage; world alpha retains D3D blend semantics.
     * These immutable pipelines are shared across targets, so target kind is
     * part of the key just like blend mode and color mask. */
    int overlay = !!c->overlay_target;
    id<MTLRenderPipelineState> cached = depth ? c->s->pso_d[overlay][blend][mask] : c->s->pso[overlay][blend][mask];
    if (cached) return cached;
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [c->s->lib newFunctionWithName:@"v_main"];
    pd.fragmentFunction = [c->s->lib newFunctionWithName:@"f_main"];
    /* Every draw encoder owns one color+depth pass so many D3D draws can share
     * a command buffer. A disabled depth state still compares always and never
     * writes, matching a color-only pass. */
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    configure_program_color(pd.colorAttachments[0], blend, 0, 0, 0, mask, c->overlay_target);
    NSError *e = nil; id<MTLRenderPipelineState> p = [c->s->dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!p) { g_err = "pipeline creation failed"; NSLog(@"metalrenderer: %@", e); }
    if (depth) c->s->pso_d[overlay][blend][mask] = p; else c->s->pso[overlay][blend][mask] = p;
    return p;
    }
}
static id<MTLRenderPipelineState> pso_for(mr_context *c, int blend, int mask) { return pso_for2(c, blend, mask, 0); }
/* Pretransformed draws with raw D3D blend factors/operations. */
static id<MTLRenderPipelineState> pso_custom(mr_context *c, int src, int dst, int op, int mask) {
    @autoreleasepool {
    mask &= 0xF;
    for (uint32_t i = 0; i < c->s->custom_rhw_count; ++i) { mr_custom_rhw_cache *e = &c->s->custom_rhw[i];
        if (e->src == src && e->dst == dst && e->op == op && e->mask == mask && e->overlay == c->overlay_target) return e->pipeline; }
    if (c->s->custom_rhw_count >= 32) { g_err = "custom blend pipeline cache is full"; return nil; }
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [c->s->lib newFunctionWithName:@"v_main"]; pd.fragmentFunction = [c->s->lib newFunctionWithName:@"f_main"];
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8; pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    configure_program_color(pd.colorAttachments[0], MR_BLEND_CUSTOM, src, dst, op, mask, c->overlay_target);
    NSError *e = nil; id<MTLRenderPipelineState> p = [c->s->dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!p) { set_errorf("custom blend pipeline failed: %s", e.localizedDescription.UTF8String); return nil; }
    mr_custom_rhw_cache *slot = &c->s->custom_rhw[c->s->custom_rhw_count++];
    slot->src = src; slot->dst = dst; slot->op = op; slot->mask = mask; slot->overlay = c->overlay_target; slot->pipeline = p;
    return p;
    }
}

static int ensure_command_buffer(mr_shared *s) {
    if (s->pending_cb) return 0;
    @autoreleasepool {
        MTLCommandBufferDescriptor *descriptor = [[MTLCommandBufferDescriptor alloc] init];
        descriptor.retainedReferences = YES; /* per-draw buffers/textures live through GPU completion */
        s->pending_cb = [s->queue commandBufferWithDescriptor:descriptor];
        if (!s->pending_cb) { g_err = "command buffer allocation failed"; return -1; }
    }
    return 0;
}
static void bound_forget(mr_shared *s);
static void end_pending_encoder(mr_shared *s) {
    if (s->pending_encoder) { [s->pending_encoder endEncoding]; s->pending_encoder = nil; s->pending_target = NULL; bound_forget(s); }
}
/* Transient storage for one command buffer: draws copy vertices/indices into
 * a shared arena instead of allocating a Metal buffer per draw. The arena
 * leaves the context at commit and returns to a small pool once the GPU has
 * finished reading it. A fresh 32 MB buffer every frame faulted in and
 * zero-filled every page it used afresh, inside the draw copies. */
#define MR_ARENA_BYTES (32u * 1024u * 1024u)
#define MR_ARENA_POOL 3   /* the frames that can be in flight */
static id<MTLBuffer> g_arena_pool[MR_ARENA_POOL];
static unsigned g_arena_pool_count;
static os_unfair_lock g_arena_pool_lock = OS_UNFAIR_LOCK_INIT;
static id<MTLBuffer> arena_pool_take(void) {
    id<MTLBuffer> b = nil;
    os_unfair_lock_lock(&g_arena_pool_lock);
    if (g_arena_pool_count) { b = g_arena_pool[--g_arena_pool_count]; g_arena_pool[g_arena_pool_count] = nil; }
    os_unfair_lock_unlock(&g_arena_pool_lock);
    return b;
}
static void arena_pool_give(id<MTLBuffer> b) {
    if (!b || b.length != MR_ARENA_BYTES) return;
    os_unfair_lock_lock(&g_arena_pool_lock);
    if (g_arena_pool_count < MR_ARENA_POOL) g_arena_pool[g_arena_pool_count++] = b;
    os_unfair_lock_unlock(&g_arena_pool_lock);
}
/* Before cb is committed: its arena goes back to the pool when cb completes. */
static void arena_retire(id<MTLBuffer> used, id<MTLCommandBuffer> cb) {
    if (!used || !cb || used.length != MR_ARENA_BYTES) return;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) { (void)finished; arena_pool_give(used); }];
}
static id<MTLBuffer> arena_alloc(mr_shared *s, size_t bytes, size_t *offset) {
    @autoreleasepool {
    size_t need = (bytes + 255u) & ~(size_t)255u;
    if (!s->arena || s->arena_used + need > s->arena_size) {
        size_t size = need > MR_ARENA_BYTES ? need : MR_ARENA_BYTES;
        arena_retire(s->arena, s->pending_cb);
        id<MTLBuffer> next = size == MR_ARENA_BYTES ? arena_pool_take() : nil;
        s->arena = next ? next : [s->dev newBufferWithLength:size options:MTLResourceStorageModeShared];
        if (!s->arena) { g_err = "arena allocation failed"; return nil; }
        s->arena_size = size; s->arena_used = 0;
    }
    *offset = s->arena_used; s->arena_used += need; return s->arena;
    }
}
static id<MTLBuffer> arena_copy(mr_shared *s, const void *bytes, size_t length, size_t *offset) {
    id<MTLBuffer> b = arena_alloc(s, length, offset); if (!b) return nil;
    memcpy((uint8_t *)b.contents + *offset, bytes, length); return b;
}

static mr_shared *shared_state(void);
void *mr_buffer_create(const void *bytes, size_t length) {
    @autoreleasepool {
    mr_shared *s = shared_state();
    if (!s || !bytes || !length) return NULL;
    id<MTLBuffer> b = [s->dev newBufferWithBytes:bytes length:length options:MTLResourceStorageModeShared];
    return b ? (void *)CFBridgingRetain(b) : NULL;
    }
}
void mr_buffer_release(void *buffer) { if (buffer) CFRelease(buffer); }
const void *mr_buffer_contents(const void *buffer) { return buffer ? ((__bridge id<MTLBuffer>)buffer).contents : NULL; }
size_t mr_buffer_length(const void *buffer) { return buffer ? ((__bridge id<MTLBuffer>)buffer).length : 0; }
/* The resident copy that holds `length` bytes at `offset`, if it can be bound
 * there: vertex fetch needs a four-byte aligned buffer offset. */
static id<MTLBuffer> resident_range(const void *buffer, size_t offset, size_t length) {
    if (!buffer || !mr_fast_paths_enabled() || (offset & 3u)) return nil;
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)buffer;
    size_t size = b.length;
    return offset <= size && length <= size - offset ? b : nil;
}
static void bound_forget(mr_shared *s) {
    mr_bound_state *b = &s->bound;
    b->pipeline = nil; b->depth = nil; b->stencil_ref = b->cull = b->winding = -1;
    b->viewport_valid = b->scissor_valid = 0;
    for (int i = 0; i < 16; ++i) { b->textures[i] = nil; b->samplers[i] = nil; }
}
/* The per-draw fixed state, in the order the draws always set it. With the
 * fast paths off every value is sent, as before; the record is kept either
 * way so the switch can change mid-encoder. */
static void bind_draw_state(mr_shared *s, id<MTLRenderCommandEncoder> enc, id<MTLRenderPipelineState> pipeline,
                            id<MTLDepthStencilState> depth, int stencil_enable, uint32_t stencil_ref,
                            MTLViewport viewport, MTLScissorRect scissor, int cull_none, int clockwise) {
    mr_bound_state *b = &s->bound; int all = !mr_fast_paths_enabled();
    if (all || b->pipeline != pipeline) { [enc setRenderPipelineState:pipeline]; b->pipeline = pipeline; }
    if (all || b->depth != depth) { [enc setDepthStencilState:depth]; b->depth = depth; }
    if (stencil_enable && (all || b->stencil_ref != (int)(stencil_ref & 255u))) { [enc setStencilReferenceValue:stencil_ref & 255u]; b->stencil_ref = (int)(stencil_ref & 255u); }
    if (all || !b->viewport_valid || memcmp(&b->viewport, &viewport, sizeof viewport)) { [enc setViewport:viewport]; b->viewport = viewport; b->viewport_valid = 1; }
    if (all || !b->scissor_valid || memcmp(&b->scissor, &scissor, sizeof scissor)) { [enc setScissorRect:scissor]; b->scissor = scissor; b->scissor_valid = 1; }
    if (all || b->cull != !cull_none) { [enc setCullMode:cull_none ? MTLCullModeNone : MTLCullModeBack]; b->cull = !cull_none; }
    if (!cull_none && (all || b->winding != !!clockwise)) { [enc setFrontFacingWinding:clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise]; b->winding = !!clockwise; }
}
static void bind_fragment_texture(mr_shared *s, id<MTLRenderCommandEncoder> enc, id<MTLTexture> texture, id<MTLSamplerState> sampler, int index) {
    mr_bound_state *b = &s->bound; int all = !mr_fast_paths_enabled();
    if (all || b->textures[index] != texture) { [enc setFragmentTexture:texture atIndex:index]; b->textures[index] = texture; }
    if (all || b->samplers[index] != sampler) { [enc setFragmentSamplerState:sampler atIndex:index]; b->samplers[index] = sampler; }
}
static void clear_list_remove(mr_context *c) {
    for (mr_context **link = &c->s->clear_list; *link; link = &(*link)->next_clear)
        if (*link == c) { *link = c->next_clear; break; }
    c->next_clear = NULL; c->pending_clear = 0; c->pending_clear_calls = 0;
}
/* Attachments of a pass into c: the given clears, everything else loaded.
 * Only attachments named in `mask` are attached (bit 0 colour, 1 depth,
 * 2 stencil), matching the clear-only passes this replaces. */
static void describe_pass(mr_context *c, MTLRenderPassDescriptor *rp, int mask, int clears) {
    if (mask & 1) {
        rp.colorAttachments[0].texture = c->target;
        rp.colorAttachments[0].loadAction = (clears & 1) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (clears & 1) rp.colorAttachments[0].clearColor = c->clear_color;
    }
    if (mask & 2) {
        rp.depthAttachment.texture = c->depth_target;
        rp.depthAttachment.loadAction = (clears & 2) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (clears & 2) rp.depthAttachment.clearDepth = c->clear_depth;
    }
    if (mask & 4) {
        rp.stencilAttachment.texture = c->depth_target;
        rp.stencilAttachment.loadAction = (clears & 4) ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        if (clears & 4) rp.stencilAttachment.clearStencil = c->clear_stencil;
    }
}
/* Encode c's waiting clears as a pass of their own, for a consumer that is
 * not a draw into c. */
static void encode_clear_pass(mr_context *c, MTLRenderPassDescriptor *rp);
static void encode_context_clears(mr_context *c) {
    if (!c || !c->pending_clear) return;
    int clears = c->pending_clear;
    atomic_fetch_add_explicit(&g_folded_clears, c->pending_clear_calls - 1, memory_order_relaxed);
    clear_list_remove(c);
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        describe_pass(c, rp, clears, clears);
        encode_clear_pass(c, rp);
    }
}
static void encode_pending_clears(mr_shared *s) {
    while (s->clear_list) encode_context_clears(s->clear_list);
}
static id<MTLRenderCommandEncoder> begin_draw_encoder(mr_context *c) {
    mr_shared *s = c->s;
    /* A previously acquired target texture ID may be sampled by this draw
     * without another mr_target_texture() call. Flush other targets before
     * any draw consumes them, even when its own encoder was already open.
     * Clears of c itself can still become this pass's load actions. */
    for (mr_context *other = s->clear_list, *next; other; other = next) {
        next = other->next_clear;
        if (other != c) encode_context_clears(other);
    }
    if (s->pending_encoder && s->pending_target == c) return s->pending_encoder;
    end_pending_encoder(s);
    @autoreleasepool {
        if (ensure_command_buffer(s)) return nil;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        int clears = c->pending_clear;
        describe_pass(c, rp, 7, clears);
        s->pending_encoder = [s->pending_cb renderCommandEncoderWithDescriptor:rp];
        if (!s->pending_encoder) { g_err = "render encoder allocation failed"; return nil; }
        s->pending_target = c;
        bound_forget(s);
        if (clears) { atomic_fetch_add_explicit(&g_folded_clears, c->pending_clear_calls, memory_order_relaxed); clear_list_remove(c); }
    }
    return s->pending_encoder;
}

static int flush_pending(mr_context *c, int wait) {
    @autoreleasepool {
    mr_shared *s = c->s;
    encode_pending_clears(s);
    end_pending_encoder(s);
    if (s->pending_cb) { arena_retire(s->arena, s->pending_cb); [s->pending_cb commit]; s->last_cb = s->pending_cb; s->pending_cb = nil; s->arena = nil; s->arena_used = s->arena_size = 0; }
    if (wait && s->last_cb) {
        id<MTLCommandBuffer> completed = s->last_cb;
        [completed waitUntilCompleted];
        if (completed.status == MTLCommandBufferStatusError) {
            set_errorf("GPU command buffer failed: %s", completed.error.localizedDescription.UTF8String);
            s->last_cb = nil;
            return MR_ERR_DEVICE;
        }
        s->last_cb = nil;
    }
    return MR_OK;
    }
}
/* Make a context's color target safe to sample: end any encoder drawing into it. */
uint32_t mr_target_texture(mr_context *c) {
    if (!c || !c->target_id) return 0;
    encode_context_clears(c);
    if (c->s->pending_encoder && c->s->pending_target == c) end_pending_encoder(c->s);
    return c->target_id;
}

static mr_shared *shared_state(void) {
    @autoreleasepool {
    if (g_shared) return g_shared;
    mr_shared *s = calloc(1, sizeof *s);
    if (!s) { g_err = "shared state allocation failed"; return NULL; }
    s->dev = MTLCreateSystemDefaultDevice();
    if (!s->dev) { g_err = "no Metal device"; free(s); return NULL; }
    s->queue = [s->dev newCommandQueue];
    if (!s->queue) { g_err = "queue allocation failed"; free(s); return NULL; }
    NSError *e = nil; s->lib = [s->dev newLibraryWithSource:kSrc options:nil error:&e];
    if (!s->lib) { g_err = "shader compile failed"; NSLog(@"metalrenderer: %@", e); free(s); return NULL; }
    for (int i = 0; i < 4; i++) {
        MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = (i & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.sAddressMode = sd.tAddressMode = (i & 2) ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
        s->samplers[i] = [s->dev newSamplerStateWithDescriptor:sd];
    }
    uint32_t px = 0xFFFFFFFFu;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    td.storageMode = MTLStorageModeShared; s->white = [s->dev newTextureWithDescriptor:td];
    if (!s->white) { g_err = "white texture allocation failed"; free(s); return NULL; }
    [s->white replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&px bytesPerRow:4];
    uint32_t black = 0xFF000000u;
    MTLTextureDescriptor *bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
    bd.storageMode = MTLStorageModeShared; s->black2d = [s->dev newTextureWithDescriptor:bd];
    [s->black2d replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
    MTLTextureDescriptor *cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
    cd.storageMode = MTLStorageModeShared; s->blackcube = [s->dev newTextureWithDescriptor:cd];
    for (NSUInteger face = 0; face < 6; ++face) [s->blackcube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:face withBytes:&black bytesPerRow:4 bytesPerImage:4];
    MTLTextureDescriptor *vd = [[MTLTextureDescriptor alloc] init]; vd.textureType = MTLTextureType3D; vd.pixelFormat = MTLPixelFormatBGRA8Unorm; vd.width = vd.height = vd.depth = 1; vd.storageMode = MTLStorageModeShared;
    s->black3d = [s->dev newTextureWithDescriptor:vd];
    [s->black3d replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&black bytesPerRow:4 bytesPerImage:4];
    g_shared = s;
    struct mr_compiler *cc = compiler_for(s);   /* pipelines earlier sessions used, rebuilt off the engine thread */
    if (cc) { mr_pipeline_store_open(cc); compiler_prewarm_builtin(cc); }
    return s;
    }
}

mr_context *mr_create(int w, int h) {
    @autoreleasepool {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { g_err = "bad size"; return NULL; }
    mr_shared *s = shared_state(); if (!s) return NULL;
    mr_context *c = calloc(1, sizeof *c);
    if (!c) { g_err = "context allocation failed"; return NULL; }
    c->s = s; s->contexts++;
    if (!pso_for(c, 0, 0xF)) { mr_destroy(c); return NULL; }
    if (make_target(c, w, h)) { mr_destroy(c); return NULL; }
    mr_clear(c, 0xFF000000u); mr_clear_depth(c, 1.0f);
    return c;
    }
}

void mr_destroy(mr_context *c) {
    @autoreleasepool {
    if (!c) return;
    mr_shared *s = c->s;
    if (s->pending_encoder && s->pending_target == c) end_pending_encoder(s);
    flush_pending(c, 1);
    if (c->target_id) { s->textures[c->target_id] = nil; s->texture_keys[c->target_id] = 0; c->target_id = 0; }
    c->target = nil; c->depth_target = nil;
    if (s->contexts) s->contexts--;
    free(c);
    }
}
int mr_resize(mr_context *c, int w, int h) { @autoreleasepool { if (!c || w <= 0 || h <= 0 || w > 8192 || h > 8192) { g_err = "bad size"; return -1; } if (flush_pending(c, 1)) return -1; return make_target(c, w, h); } }
void mr_set_viewport(mr_context *c, int x, int y, int w, int h) {
    if (!c) return; if (x < 0) x = 0; if (y < 0) y = 0; if (x + w > c->w) w = c->w - x; if (y + h > c->h) h = c->h - y;
    c->vx = x; c->vy = y; c->vw = w > 0 ? w : 0; c->vh = h > 0 ? h : 0;
}

static void encode_clear_pass(mr_context *c, MTLRenderPassDescriptor *rp) {
    mr_shared *s = c->s;
    end_pending_encoder(s);
    if (ensure_command_buffer(s)) return;
    [[s->pending_cb renderCommandEncoderWithDescriptor:rp] endEncoding];
}
/* Record a clear of `attachment` on c (its value already stored). */
static void defer_clear(mr_context *c, int attachment) {
    mr_shared *s = c->s;
    /* Draws already encoded into c come first; its next pass starts cleared. */
    if (s->pending_encoder && s->pending_target == c) end_pending_encoder(s);
    if (!c->pending_clear) { c->next_clear = s->clear_list; s->clear_list = c; }
    c->pending_clear |= attachment; c->pending_clear_calls++;
}
void mr_clear(mr_context *c, uint32_t col) {
    if (!c) return;
    MTLClearColor color = MTLClearColorMake(((col >> 16) & 255) / 255.0, ((col >> 8) & 255) / 255.0, (col & 255) / 255.0, ((col >> 24) & 255) / 255.0);
    if (mr_fast_paths_enabled()) { c->clear_color = color; defer_clear(c, 1); return; }
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = c->target; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = color;
        encode_clear_pass(c, rp);
    }
}

void mr_clear_depth(mr_context *c, float depth) {
    if (!c) return;
    if (depth < 0.0f) depth = 0.0f; else if (depth > 1.0f) depth = 1.0f;
    if (mr_fast_paths_enabled()) { c->clear_depth = depth; defer_clear(c, 2); return; }
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = c->depth_target; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = depth;
        encode_clear_pass(c, rp);
    }
}

void mr_clear_stencil(mr_context *c, uint32_t value) {
    if (!c) return;
    if (mr_fast_paths_enabled()) { c->clear_stencil = value & 255u; defer_clear(c, 4); return; }
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.stencilAttachment.texture = c->depth_target; rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.clearStencil = value & 255u;
        encode_clear_pass(c, rp);
    }
}

uint32_t mr_texture_create(mr_context *c, int w, int h, const void *bgra, size_t pitch) {
    @autoreleasepool {
    if (!c || w <= 0 || h <= 0 || w > 8192 || h > 8192 || !bgra || pitch < (size_t)w * 4) { g_err = "bad texture args"; return 0; }
    uint32_t texture_id = 0; for (uint32_t i = 1; i < MR_MAX_TEX; i++) if (!c->s->textures[i]) { texture_id = i; break; }
    if (!texture_id) { g_err = "texture table full"; return 0; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [c->s->dev newTextureWithDescriptor:td]; if (!t) { g_err = "texture allocation failed"; return 0; }
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bgra bytesPerRow:pitch];
    c->s->textures[texture_id] = t; return texture_id;
    }
}
int mr_texture_update(mr_context *c, uint32_t texture_id, const void *bgra, size_t pitch) {
    @autoreleasepool {
    if (!c || texture_id >= MR_MAX_TEX || !c->s->textures[texture_id] || !bgra) { g_err = "bad texture texture_id"; return -1; }
    if (flush_pending(c, 1)) return -1; /* do not overwrite bytes still sampled by queued draws */
    id<MTLTexture> t = c->s->textures[texture_id]; if (pitch < t.width * 4) { g_err = "pitch too small"; return -1; }
    [t replaceRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0 withBytes:bgra bytesPerRow:pitch]; return 0;
    }
}
void mr_texture_destroy(mr_context *c, uint32_t texture_id) { if (c && texture_id < MR_MAX_TEX) {
    if (c->s->texture_keys[texture_id]) { key_slot_remove(c, c->s->texture_keys[texture_id]); c->s->cached_texture_bytes -= c->s->texture_bytes[texture_id]; c->s->cached_texture_count--; }
    c->s->textures[texture_id] = nil; c->s->texture_keys[texture_id] = c->s->texture_use[texture_id] = c->s->texture_bytes[texture_id] = 0;
} }

static int evict_oldest_cached_texture(mr_context *c) {
    uint32_t victim = 0; uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (c->s->texture_keys[i] && (!texture_binding_depth || c->s->texture_use[i] <= texture_binding_floor) && c->s->texture_use[i] < oldest) { oldest = c->s->texture_use[i]; victim = i; }
    if (!victim) return 0;
    texture_cache_evictions++;
    key_slot_remove(c, c->s->texture_keys[victim]);
    c->s->cached_texture_bytes -= c->s->texture_bytes[victim]; c->s->cached_texture_count--;
    /* Pending command buffers retain the old texture object until their GPU
     * work completes, so dropping the cache's reference is safe without a wait. */
    c->s->textures[victim] = nil; c->s->texture_keys[victim] = c->s->texture_use[victim] = c->s->texture_bytes[victim] = 0;
    return 1;
}

static int make_cached_texture_room(mr_context *c, uint64_t bytes) {
    while (c->s->cached_texture_count &&
           (c->s->cached_texture_count >= MR_MAX_CACHED_TEXTURES || bytes > MR_MAX_CACHED_TEXTURE_BYTES ||
            c->s->cached_texture_bytes > MR_MAX_CACHED_TEXTURE_BYTES - bytes))
        if (!evict_oldest_cached_texture(c)) { g_err="texture cache pinned by current draw"; return 0; }
    return 1;
}

static uint64_t mip_chain_bytes(uint32_t width, uint32_t height, uint32_t slices) {
    uint64_t bytes = 0;
    for (;;) {
        bytes += (uint64_t)width * height * 4u * slices;
        if (width == 1 && height == 1) return bytes;
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
    }
}

/* Cached textures are immutable, so create their lower levels once at upload.
 * This avoids a Metal command buffer per texture and keeps transient driver
 * allocations bounded during a large level load. */
static int upload_mip_chain(id<MTLTexture> texture, NSUInteger slice,
                            const uint8_t *base, uint32_t width, uint32_t height, size_t pitch) {
    const uint8_t *source = base;
    uint8_t *owned_source = NULL;
    size_t source_pitch = pitch;
    uint32_t source_width = width, source_height = height;
    for (NSUInteger level = 1; level < texture.mipmapLevelCount; ++level) {
        uint32_t next_width = source_width > 1 ? source_width >> 1 : 1;
        uint32_t next_height = source_height > 1 ? source_height >> 1 : 1;
        size_t next_pitch = (size_t)next_width * 4u;
        uint8_t *next = malloc(next_pitch * next_height);
        if (!next) { free(owned_source); g_err = "mipmap allocation failed"; return -1; }
        halo_texture_mip(source, source_width, source_height, source_pitch, next);
        [texture replaceRegion:MTLRegionMake2D(0, 0, next_width, next_height)
                    mipmapLevel:level slice:slice withBytes:next bytesPerRow:next_pitch
                  bytesPerImage:next_pitch * next_height];
        free(owned_source); owned_source = next; source = next; source_pitch = next_pitch;
        source_width = next_width; source_height = next_height;
    }
    free(owned_source); return 0;
}

static uint32_t key_slot_hash(uint64_t key) { key ^= key >> 29; key *= UINT64_C(0xBF58476D1CE4E5B9); key ^= key >> 32; return (uint32_t)key & (MR_KEY_SLOTS - 1u); }
static void key_slot_insert(mr_context *c, uint64_t key, uint32_t id) {
    uint32_t h = key_slot_hash(key);
    for (uint32_t n = 0; n < MR_KEY_SLOTS; ++n, h = (h + 1u) & (MR_KEY_SLOTS - 1u)) {
        uint32_t occupant = c->s->key_slots[h];
        if (!occupant || !c->s->textures[occupant] || c->s->texture_keys[occupant] != key) { if (!occupant || !c->s->textures[occupant]) { c->s->key_slots[h] = id; return; } continue; }
        c->s->key_slots[h] = id; return;
    }
}
static void key_slot_remove(mr_context *c, uint64_t key) {
    uint32_t h = key_slot_hash(key);
    for (uint32_t n = 0; n < MR_KEY_SLOTS; ++n, h = (h + 1u) & (MR_KEY_SLOTS - 1u)) {
        uint32_t occupant = c->s->key_slots[h];
        if (!occupant) return;
        if (c->s->texture_keys[occupant] == key) { c->s->key_slots[h] = 0; /* re-insert the rest of the probe run */
            uint32_t j = (h + 1u) & (MR_KEY_SLOTS - 1u);
            while (c->s->key_slots[j]) { uint32_t moved = c->s->key_slots[j]; c->s->key_slots[j] = 0; if (c->s->textures[moved] && c->s->texture_keys[moved]) key_slot_insert(c, c->s->texture_keys[moved], moved); j = (j + 1u) & (MR_KEY_SLOTS - 1u); }
            return; }
    }
}
uint32_t mr_texture_find_cached(mr_context *c, uint64_t key) {
    if (!c || !key) return 0;
    uint32_t h = key_slot_hash(key);
    for (uint32_t n = 0; n < MR_KEY_SLOTS; ++n, h = (h + 1u) & (MR_KEY_SLOTS - 1u)) {
        uint32_t i = c->s->key_slots[h];
        if (!i) return 0;
        if (c->s->texture_keys[i] == key && c->s->textures[i]) { c->s->texture_use[i] = ++c->s->texture_tick; texture_cache_hits++; return i; }
    }
    return 0;
}

static uint32_t texture_create_cached_impl(mr_context *c, uint64_t key, int w, int h, const void *bgra, size_t pitch, int mipmapped);
uint32_t mr_texture_create_cached(mr_context *c, uint64_t key, int w, int h, const void *bgra, size_t pitch) {
    return texture_create_cached_impl(c, key, w, h, bgra, pitch, 1);
}
uint32_t mr_texture_create_cached_nomip(mr_context *c, uint64_t key, int w, int h, const void *bgra, size_t pitch) {
    return texture_create_cached_impl(c, key, w, h, bgra, pitch, 0);
}
static uint32_t texture_create_cached_impl(mr_context *c, uint64_t key, int w, int h, const void *bgra, size_t pitch, int mipmapped) {
    @autoreleasepool {
    uint32_t hit = mr_texture_find_cached(c, key); if (hit) return hit;
    if (!c || !key || w <= 0 || h <= 0 || w > 8192 || h > 8192 || !bgra || pitch < (size_t)w * 4) { g_err = "bad cached texture args"; return 0; }
    uint64_t bytes = mipmapped ? mip_chain_bytes((uint32_t)w, (uint32_t)h, 1) : (uint64_t)w * h * 4u;
    if (bytes > MR_MAX_CACHED_TEXTURE_BYTES) { g_err = "cached texture exceeds memory limit"; return 0; }
    if (!make_cached_texture_room(c, bytes)) return 0;
    uint32_t texture_id = 0;
    for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!c->s->textures[i]) { texture_id = i; break; }
    if (!texture_id && evict_oldest_cached_texture(c)) for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!c->s->textures[i]) { texture_id = i; break; }
    if (!texture_id) { g_err = "texture table full"; return 0; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:mipmapped ? YES : NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [c->s->dev newTextureWithDescriptor:td]; if (!t) { g_err = "cached texture allocation failed"; return 0; }
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:bgra bytesPerRow:pitch];
    if (mipmapped && upload_mip_chain(t, 0, bgra, (uint32_t)w, (uint32_t)h, pitch)) return 0;
    c->s->textures[texture_id] = t; c->s->texture_keys[texture_id] = key; c->s->texture_use[texture_id] = ++c->s->texture_tick; c->s->texture_bytes[texture_id] = bytes;
    c->s->cached_texture_bytes += bytes; c->s->cached_texture_count++; texture_cache_uploads++;
    key_slot_insert(c, key, texture_id);
    return texture_id;
    }
}

uint32_t mr_texture_create_cube_cached(mr_context *c, uint64_t key, int edge, const void *faces[6], size_t pitch) {
    @autoreleasepool {
    uint32_t hit = mr_texture_find_cached(c, key); if (hit) return hit;
    if (!c || !key || edge <= 0 || edge > 8192 || !faces || pitch < (size_t)edge * 4) { g_err = "bad cached cube texture args"; return 0; }
    for (int face = 0; face < 6; ++face) if (!faces[face]) { g_err = "missing cube texture face"; return 0; }
    uint64_t bytes = mip_chain_bytes((uint32_t)edge, (uint32_t)edge, 6);
    if (bytes > MR_MAX_CACHED_TEXTURE_BYTES) { g_err = "cached cube texture exceeds memory limit"; return 0; }
    if (!make_cached_texture_room(c, bytes)) return 0;
    uint32_t texture_id = 0;
    for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!c->s->textures[i]) { texture_id = i; break; }
    if (!texture_id && evict_oldest_cached_texture(c)) for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!c->s->textures[i]) { texture_id = i; break; }
    if (!texture_id) { g_err = "texture table full"; return 0; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:edge mipmapped:YES];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [c->s->dev newTextureWithDescriptor:td]; if (!texture) { g_err = "cached cube texture allocation failed"; return 0; }
    for (NSUInteger face = 0; face < 6; ++face) {
        [texture replaceRegion:MTLRegionMake2D(0, 0, edge, edge) mipmapLevel:0 slice:face withBytes:faces[face] bytesPerRow:pitch bytesPerImage:pitch * (size_t)edge];
        if (upload_mip_chain(texture, face, faces[face], (uint32_t)edge, (uint32_t)edge, pitch)) return 0;
    }
    c->s->textures[texture_id] = texture; c->s->texture_keys[texture_id] = key; c->s->texture_use[texture_id] = ++c->s->texture_tick; c->s->texture_bytes[texture_id] = bytes;
    c->s->cached_texture_bytes += bytes; c->s->cached_texture_count++; texture_cache_uploads++;
    key_slot_insert(c, key, texture_id);
    return texture_id;
    }
}

uint32_t mr_texture_create_volume_cached(mr_context *c, uint64_t key, int w, int h, int d, const void *bgra, size_t pitch, size_t slice_pitch) {
    @autoreleasepool {
    uint32_t hit = mr_texture_find_cached(c, key); if (hit) return hit;
    if (!c || !key || w <= 0 || h <= 0 || d <= 0 || w > 2048 || h > 2048 || d > 2048 || !bgra || pitch < (size_t)w * 4 || slice_pitch < pitch * (size_t)h) { g_err = "bad cached volume texture args"; return 0; }
    uint64_t bytes = (uint64_t)w * h * d * 4u;
    if (bytes > MR_MAX_CACHED_TEXTURE_BYTES) { g_err = "cached volume texture exceeds memory limit"; return 0; }
    if (!make_cached_texture_room(c, bytes)) return 0;
    uint32_t texture_id = 0;
    for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!c->s->textures[i]) { texture_id = i; break; }
    if (!texture_id && evict_oldest_cached_texture(c)) for (uint32_t i = 1; i < MR_MAX_TEX; ++i) if (!c->s->textures[i]) { texture_id = i; break; }
    if (!texture_id) { g_err = "texture table full"; return 0; }
    MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
    td.textureType = MTLTextureType3D; td.pixelFormat = MTLPixelFormatBGRA8Unorm; td.width = w; td.height = h; td.depth = d; td.mipmapLevelCount = 1;
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [c->s->dev newTextureWithDescriptor:td]; if (!t) { g_err = "cached volume texture allocation failed"; return 0; }
    [t replaceRegion:MTLRegionMake3D(0, 0, 0, w, h, d) mipmapLevel:0 slice:0 withBytes:bgra bytesPerRow:pitch bytesPerImage:slice_pitch];
    c->s->textures[texture_id] = t; c->s->texture_keys[texture_id] = key; c->s->texture_use[texture_id] = ++c->s->texture_tick; c->s->texture_bytes[texture_id] = bytes;
    c->s->cached_texture_bytes += bytes; c->s->cached_texture_count++; texture_cache_uploads++;
    key_slot_insert(c, key, texture_id);
    return texture_id;
    }
}

static id<MTLDepthStencilState> depth_stencil_state_for(mr_context *c,
    int depth_enable,int depth_compare,int depth_write,int stencil_enable,
    int stencil_fail,int stencil_depth_fail,int stencil_pass,int stencil_compare,
    uint32_t stencil_read_mask,uint32_t stencil_write_mask);
static id<MTLSamplerState> sampler_for(mr_context *c, const mr_program_sampler *sampler);
/* Whether every index is below `limit`: a maximum over the whole array, which
 * vectorises, instead of a loop that stops at the first bad index. Same answer. */
static int indices_below(const uint16_t *indices, uint32_t count, uint32_t limit) {
    uint16_t high = 0;
    for (uint32_t i = 0; i < count; ++i) high = indices[i] > high ? indices[i] : high;
    return !count || high < limit;
}

int mr_draw_rhw(mr_context *c, const mr_draw_state *st, int prim, const void *verts, size_t stride, uint32_t nv, const uint16_t *idx, uint32_t ni) {
    @autoreleasepool {
    if (!c || !st || !verts || stride < sizeof(mr_vertex_rhw) || nv == 0) { g_err = "bad draw args"; return MR_ERR_ARGS; }
    if (st->blend < 0 || st->blend >= MR_BLEND_COUNT || (st->color_write_mask & ~15) || st->cull_mode < 0 || st->cull_mode > 3) { g_err = "unsupported draw state"; return MR_ERR_ARGS; }
    if (nv > MR_MAX_VERTS || ni > MR_MAX_INDICES) { g_err = "draw exceeds buffer bounds"; return MR_ERR_BOUNDS; }
    if (!mr_primitive_supported(prim)) { g_err = "unsupported primitive type"; return MR_ERR_PRIMITIVE; }
    uint32_t count = idx ? ni : nv;
    if (count < 3) return MR_OK;
    if (idx && !indices_below(idx, ni, nv)) { g_err = "index out of range"; return MR_ERR_BOUNDS; }
    id<MTLTexture> tex = c->s->white;
    if (st->texture) { if (st->texture >= MR_MAX_TEX || !c->s->textures[st->texture]) { g_err = "unknown texture id"; return MR_ERR_TEXTURE; } tex = c->s->textures[st->texture]; }
    id<MTLSamplerState> texture_sampler=st->sampler ? sampler_for(c,st->sampler) :
        c->s->samplers[(st->linear_filter ? 1 : 0) | (st->address_clamp ? 2 : 0)];
    if(!texture_sampler)return MR_ERR_UNSUPPORTED;
    id<MTLRenderPipelineState> pso = st->blend == MR_BLEND_CUSTOM ? pso_custom(c, st->blend_src, st->blend_dst, st->blend_op, st->color_write_mask)
                                                                  : pso_for2(c, st->blend, st->color_write_mask, st->depth_enable);
    if (!pso) return MR_ERR_DEVICE;
    if (c->vw <= 0 || c->vh <= 0) return MR_OK;

    size_t vertex_offset = 0, index_offset = 0;
    id<MTLBuffer> vertex_buffer = arena_alloc(c->s, (size_t)nv * sizeof(VIn), &vertex_offset);
    id<MTLBuffer> index_buffer = idx ? arena_copy(c->s, idx, (size_t)ni * 2, &index_offset) : nil;
    if (!vertex_buffer || (idx && !index_buffer)) { g_err = "draw buffer allocation failed"; return MR_ERR_DEVICE; }
    VIn *dst = (VIn *)((uint8_t *)vertex_buffer.contents + vertex_offset); const uint8_t *src = verts;
    for (uint32_t i = 0; i < nv; i++) {
        mr_vertex_rhw v; memcpy(&v, src + (size_t)i * stride, sizeof v);
        dst[i].pos[0] = v.x; dst[i].pos[1] = v.y; dst[i].pos[2] = v.z; dst[i].pos[3] = v.rhw; dst[i].uv[0] = v.u; dst[i].uv[1] = v.v; dst[i].pad[0] = dst[i].pad[1] = 0;
        dst[i].color[0] = ((v.color >> 16) & 255) / 255.f; dst[i].color[1] = ((v.color >> 8) & 255) / 255.f;
        dst[i].color[2] = (v.color & 255) / 255.f; dst[i].color[3] = ((v.color >> 24) & 255) / 255.f;
    }
    U u = { { (float)c->w, (float)c->h }, st->texture ? 1 : 0, st->texture_alpha_only ? 1 : 0, st->alpha_test_ref < 0 ? -1 : (st->alpha_test_ref > 255 ? 255 : st->alpha_test_ref), st->texture_color_only ? 1 : 0 };
    id<MTLRenderCommandEncoder> enc = begin_draw_encoder(c); if (!enc) return MR_ERR_DEVICE;
    /* Encoders are shared by many D3D draws.  Bind even the disabled state so
     * depth/stencil from the preceding draw cannot leak into this one. */
    id<MTLDepthStencilState> ds = depth_stencil_state_for(c,st->depth_enable,st->depth_compare,st->depth_write,st->stencil_enable,
        st->stencil_fail,st->stencil_depth_fail,st->stencil_pass,st->stencil_compare,st->stencil_read_mask,st->stencil_write_mask);
    if (!ds) { [enc setRenderPipelineState:pso]; c->s->bound.pipeline = pso; return MR_ERR_STATE; }
    bind_draw_state(c->s, enc, pso, ds, st->stencil_enable, st->stencil_ref,
        (MTLViewport){ 0, 0, (double)c->w, (double)c->h, 0.0, 1.0 },
        (MTLScissorRect){ (NSUInteger)c->vx, (NSUInteger)c->vy, (NSUInteger)c->vw, (NSUInteger)c->vh },
        st->cull_mode == 0 || st->cull_mode == 1, st->cull_mode == 3);
    [enc setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:0];
    [enc setVertexBytes:&u length:sizeof u atIndex:1];
    [enc setFragmentBytes:&u length:sizeof u atIndex:1];
    bind_fragment_texture(c->s, enc, tex, texture_sampler, 0);
    MTLPrimitiveType pt = mr_primitive_metal(prim);
    if (idx) [enc drawIndexedPrimitives:pt indexCount:ni indexType:MTLIndexTypeUInt16 indexBuffer:index_buffer indexBufferOffset:index_offset];
    else [enc drawPrimitives:pt vertexStart:0 vertexCount:nv];
    c->tris += mr_primitive_shapes(prim, count);
    return MR_OK;
    }
}

static uint64_t hash_more(uint64_t hash, const void *data, size_t bytes) {
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) { hash ^= p[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}

static int shader_has_output(const ms_shader *shader, int usage, int index) {
    for (int i = 0; i < shader->output_count; ++i)
        if (shader->outputs[i].usage == usage && shader->outputs[i].index == index) return 1;
    return 0;
}

static NSString *d3dcolor_literal(uint32_t color) {
    return [NSString stringWithFormat:@"float4(%#.9gf, %#.9gf, %#.9gf, %#.9gf)",
            ((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0,
            (color & 255) / 255.0, ((color >> 24) & 255) / 255.0];
}

static NSString *fixed_arg(uint32_t arg, int stage, const mr_fixed_stage *fixed_stage,
                           const ms_shader *vs, int texcoord, int *uses_texture) {
    const uint32_t base = arg & 15u;
    NSString *value = nil;
    switch (base) {
        case 0: value = shader_has_output(vs, 10, 0) ? @"input.color0" : @"float4(1.0)"; break; /* D3DTA_DIFFUSE */
        case 1: value = @"current"; break;
        case 2:
            if (!shader_has_output(vs, 5, texcoord)) return nil;
            value = [NSString stringWithFormat:@"tex%d", stage]; *uses_texture = 1; break;
        case 3: value = @"fixed.factor"; break;
        case 4: value = shader_has_output(vs, 10, 1) ? @"input.color1" : @"float4(0.0)"; break;
        case 5: value = @"temporary"; break;
        case 6: value = d3dcolor_literal(fixed_stage->constant); break;
        default: return nil;
    }
    if (arg & 0x10u) value = [NSString stringWithFormat:@"(1.0 - (%@))", value];
    if (arg & 0x20u) value = [NSString stringWithFormat:@"float4((%@).a)", value];
    if (arg & ~0x3Fu) return nil;
    return value;
}

static NSString *fixed_part(NSString *value, int alpha) {
    return [NSString stringWithFormat:alpha ? @"(%@).a" : @"(%@).rgb", value];
}

static NSString *fixed_op(uint32_t op, NSString *a0, NSString *a1, NSString *a2,
                          NSString *diffuse, NSString *texture, NSString *factor,
                          NSString *current, int alpha) {
    NSString *x0 = fixed_part(a0, alpha), *x1 = fixed_part(a1, alpha), *x2 = fixed_part(a2, alpha);
    NSString *d = fixed_part(diffuse, alpha), *t = fixed_part(texture, alpha);
    NSString *f = fixed_part(factor, alpha), *cur = fixed_part(current, alpha);
    switch (op) {
        case 1: return cur;
        case 2: return x1;
        case 3: return x2;
        case 4: return [NSString stringWithFormat:@"(%@ * %@)", x1, x2];
        case 5: return [NSString stringWithFormat:@"(%@ * %@ * 2.0)", x1, x2];
        case 6: return [NSString stringWithFormat:@"(%@ * %@ * 4.0)", x1, x2];
        case 7: return [NSString stringWithFormat:@"(%@ + %@)", x1, x2];
        case 8: return [NSString stringWithFormat:@"(%@ + %@ - 0.5)", x1, x2];
        case 9: return [NSString stringWithFormat:@"((%@ + %@ - 0.5) * 2.0)", x1, x2];
        case 10: return [NSString stringWithFormat:@"(%@ - %@)", x1, x2];
        case 11: return [NSString stringWithFormat:@"(%@ + %@ * (1.0 - %@))", x1, x2, x1];
        case 12: return [NSString stringWithFormat:@"(%@ * (%@).a + %@ * (1.0 - (%@).a))", x1, diffuse, x2, diffuse];
        case 13: return [NSString stringWithFormat:@"(%@ * (%@).a + %@ * (1.0 - (%@).a))", x1, texture, x2, texture];
        case 14: return [NSString stringWithFormat:@"(%@ * (%@).a + %@ * (1.0 - (%@).a))", x1, factor, x2, factor];
        case 15: return [NSString stringWithFormat:@"(%@ + %@ * (1.0 - (%@).a))", x1, x2, texture];
        case 16: return [NSString stringWithFormat:@"(%@ * (%@).a + %@ * (1.0 - (%@).a))", x1, current, x2, current];
        case 18: if (!alpha) return [NSString stringWithFormat:@"(%@ + (%@).a * %@)", x1, a1, x2]; break;
        case 19: if (!alpha) return [NSString stringWithFormat:@"(%@ * %@ + (%@).a)", x1, x2, a1]; break;
        case 20: if (!alpha) return [NSString stringWithFormat:@"(%@ + (1.0 - (%@).a) * %@)", x1, a1, x2]; break;
        case 21: if (!alpha) return [NSString stringWithFormat:@"((1.0 - %@) * %@ + (%@).a)", x1, x2, a1]; break;
        case 24: {
            NSString *dot = [NSString stringWithFormat:@"dot((%@).rgb * 2.0 - 1.0, (%@).rgb * 2.0 - 1.0)", a1, a2];
            return alpha ? dot : [NSString stringWithFormat:@"float3(%@)", dot];
        }
        case 25: return [NSString stringWithFormat:@"(%@ * %@ + %@)", x1, x2, x0];
        case 26: return [NSString stringWithFormat:@"(%@ * %@ + (1.0 - %@) * %@)", x0, x1, x0, x2];
    }
    (void)d; (void)t; (void)f;
    return nil;
}

static int fixed_stage_uses_texture(const mr_fixed_stage *stage) {
    return mr_fixed_stage_uses_texture(stage);
}

static NSString *make_fixed_fragment(const ms_shader *vs, const mr_program_state *state, NSString *entry) {
    NSMutableString *s = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\n"];
    [s appendString:@"struct MRFixedInput {\n"];
    for (int color = 0; color < 2; ++color) if (shader_has_output(vs, 10, color))
        [s appendFormat:@" float4 color%d [[user(color%d)]];\n", color, color];
    for (int tex = 0; tex < 8; ++tex) if (shader_has_output(vs, 5, tex))
        [s appendFormat:@" float4 texcoord%d [[user(texcoord%d)]];\n", tex, tex];
    int fog = state->fog_enable && shader_has_output(vs, 11, 0);
    if (fog) [s appendString:@" float4 fog [[user(fog)]];\n"];
    /* The opt-in path reads fog_color at CPU byte 32. A Metal float3 would
     * align padding to byte 32 and place fog_color at byte 48. Preserve the
     * original source/layout for existing pipelines with the switch off. */
    if (state->radial_fog)
        [s appendString:@"};\nstruct MRFixedUniforms { float4 factor; float alpha_ref; float pad0; float pad1; float pad2; float4 fog_color; };\n"];
    else [s appendString:@"};\nstruct MRFixedUniforms { float4 factor; float alpha_ref; float3 padding; float4 fog_color; };\n"];
    [s appendFormat:@"fragment float4 %@ (MRFixedInput input [[stage_in]], constant MRFixedUniforms &fixed [[buffer(1)]]", entry];
    for (int stage = 0; stage < 8; ++stage) {
        const mr_fixed_stage *fs = &state->fixed_stages[stage];
        if (fs->color_op == 1) break;
        if (fixed_stage_uses_texture(fs)) {
            NSString *kind = state->samplers[stage].type == MR_SAMPLER_CUBE ? @"texturecube<float>" : @"texture2d<float>";
            if (state->samplers[stage].type != MR_SAMPLER_2D && state->samplers[stage].type != MR_SAMPLER_CUBE) { set_errorf("fixed stage %d uses unsupported texture type", stage); return nil; }
            [s appendFormat:@", %@ texture%d [[texture(%d)]], sampler sampler%d [[sampler(%d)]]", kind, stage, stage, stage, stage];
        }
    }
    [s appendString:@") {\n"];
    [s appendFormat:@" float4 diffuse = %@; float4 current = diffuse; float4 temporary = float4(0.0);\n",
        shader_has_output(vs, 10, 0) ? @"input.color0" : @"float4(1.0)"];
    for (int stage = 0; stage < 8; ++stage) {
        const mr_fixed_stage *fs = &state->fixed_stages[stage];
        if (fs->color_op == 1) break;
        int texcoord = (int)(fs->texcoord_index & 0xFFFFu), uses = fixed_stage_uses_texture(fs);
        if (uses) {
            if ((fs->texcoord_index & 0xFFFF0000u) || fs->texture_transform_flags) { set_errorf("fixed stage %d uses generated/transformed coordinates", stage); return nil; }
            if (!shader_has_output(vs, 5, texcoord)) { set_errorf("fixed stage %d requires missing TEXCOORD%d", stage, texcoord); return nil; }
            [s appendFormat:state->samplers[stage].type == MR_SAMPLER_CUBE ? @" float4 tex%d = texture%d.sample(sampler%d, input.texcoord%d.xyz);\n" : @" float4 tex%d = texture%d.sample(sampler%d, input.texcoord%d.xy);\n", stage, stage, stage, texcoord];
        } else [s appendFormat:@" float4 tex%d = float4(1.0);\n", stage];
        int arg_uses = 0;
        unsigned cm=mr_fixed_op_arg_mask(fs->color_op), am=mr_fixed_op_arg_mask(fs->alpha_op);
        NSString *ca0 = (cm&1) ? fixed_arg(fs->color_arg0, stage, fs, vs, texcoord, &arg_uses) : @"float4(0.0)";
        NSString *ca1 = (cm&2) ? fixed_arg(fs->color_arg1, stage, fs, vs, texcoord, &arg_uses) : @"float4(0.0)";
        NSString *ca2 = (cm&4) ? fixed_arg(fs->color_arg2, stage, fs, vs, texcoord, &arg_uses) : @"float4(0.0)";
        NSString *aa0 = (am&1) ? fixed_arg(fs->alpha_arg0, stage, fs, vs, texcoord, &arg_uses) : @"float4(0.0)";
        NSString *aa1 = (am&2) ? fixed_arg(fs->alpha_arg1, stage, fs, vs, texcoord, &arg_uses) : @"float4(0.0)";
        NSString *aa2 = (am&4) ? fixed_arg(fs->alpha_arg2, stage, fs, vs, texcoord, &arg_uses) : @"float4(0.0)";
        if (!ca0 || !ca1 || !ca2 || !aa0 || !aa1 || !aa2) { set_errorf("fixed stage %d has unsupported argument", stage); return nil; }
        NSString *tex = [NSString stringWithFormat:@"tex%d", stage];
        NSString *rgb = fixed_op(fs->color_op, ca0, ca1, ca2, @"diffuse", tex, @"fixed.factor", @"current", 0);
        NSString *alpha = fs->alpha_op == 1 ? @"current.a" : fixed_op(fs->alpha_op, aa0, aa1, aa2, @"diffuse", tex, @"fixed.factor", @"current", 1);
        if (!rgb || !alpha) { set_errorf("fixed stage %d uses unsupported color/alpha operation %u/%u", stage, fs->color_op, fs->alpha_op); return nil; }
        NSString *destination = ((fs->result_arg & 15u) == 5) ? @"temporary" :
                                ((fs->result_arg & 15u) == 0 || (fs->result_arg & 15u) == 1 ? @"current" : nil);
        if (!destination) { set_errorf("fixed stage %d has unsupported result argument", stage); return nil; }
        [s appendFormat:@" %@ = saturate(float4(%@, %@));\n", destination, rgb, alpha];
    }
    if (fog) [s appendString:@" current.rgb = mix(fixed.fog_color.rgb, current.rgb, saturate(input.fog.x));\n"];
    if (state->alpha_test_ref >= 0) {
        NSString *pass = nil;
        switch (state->alpha_test_func) {
            case 1: pass = @"false"; break;
            case 2: pass = @"current.a * 255.0 < fixed.alpha_ref"; break;
            case 3: pass = @"current.a * 255.0 == fixed.alpha_ref"; break;
            case 4: pass = @"current.a * 255.0 <= fixed.alpha_ref"; break;
            case 5: pass = @"current.a * 255.0 > fixed.alpha_ref"; break;
            case 6: pass = @"current.a * 255.0 != fixed.alpha_ref"; break;
            case 7: pass = @"current.a * 255.0 >= fixed.alpha_ref"; break;
            case 8: pass = @"true"; break;
            default: set_errorf("%s", "invalid alpha comparison function"); return nil;
        }
        [s appendFormat:@" if (!(%@)) discard_fragment();\n", pass];
    }
    [s appendString:@" return current;\n}\n"];
    return s;
}

static MTLVertexFormat vertex_format(uint8_t type, size_t *bytes) {
    switch (type) {
        case 0: *bytes = 4; return MTLVertexFormatFloat;
        case 1: *bytes = 8; return MTLVertexFormatFloat2;
        case 2: *bytes = 12; return MTLVertexFormatFloat3;
        case 3: *bytes = 16; return MTLVertexFormatFloat4;
        case 4: *bytes = 4; return MTLVertexFormatUChar4Normalized_BGRA;
        case 5: *bytes = 4; return MTLVertexFormatUChar4;
        case 6: *bytes = 4; return MTLVertexFormatShort2;
        case 7: *bytes = 8; return MTLVertexFormatShort4;
        case 8: *bytes = 4; return MTLVertexFormatUChar4Normalized;
        case 9: *bytes = 4; return MTLVertexFormatShort2Normalized;
        case 10: *bytes = 8; return MTLVertexFormatShort4Normalized;
        case 11: *bytes = 4; return MTLVertexFormatUShort2Normalized;
        case 12: *bytes = 8; return MTLVertexFormatUShort4Normalized;
        case 15: *bytes = 4; return MTLVertexFormatHalf2;
        case 16: *bytes = 8; return MTLVertexFormatHalf4;
        default: *bytes = 0; return MTLVertexFormatInvalid;
    }
}

/* Every declaration stream s binds vertex buffer index 1+s (index 0 holds the
 * shader constants). Stream zero uses the draw's stride; other streams use
 * the strides supplied in mr_program_state. */
static MTLVertexDescriptor *make_vertex_descriptor(const ms_shader *vs, const void *bytes, size_t length, size_t stride,
                                                   const mr_program_state *state, uint32_t *used_streams) {
    if (!bytes || length < 8 || (length & 7) || stride == 0 || stride > MR_MAX_VERTEX_STRIDE) { set_errorf("%s", "invalid D3D vertex declaration"); return nil; }
    const uint8_t *decl = bytes; int ended = 0; uint32_t used = 1;
    MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
    vd.layouts[1].stride = stride; vd.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex; vd.layouts[1].stepRate = 1;
    for (int a = 0; a < vs->attribute_count; ++a) {
        const ms_semantic *wanted = &vs->attributes[a]; int found = 0;
        if (wanted->location < 0 || wanted->location >= 16) { set_errorf("%s", "MojoShader emitted an invalid vertex attribute"); return nil; }
        for (size_t offset = 0; offset + 8 <= length; offset += 8) {
            uint16_t stream = (uint16_t)(decl[offset] | (decl[offset + 1] << 8));
            uint16_t byte_offset = (uint16_t)(decl[offset + 2] | (decl[offset + 3] << 8));
            uint8_t type = decl[offset + 4], method = decl[offset + 5], usage = decl[offset + 6], usage_index = decl[offset + 7];
            if (stream == 0xFF && type == 17) { ended = 1; break; }
            if (usage != wanted->usage || usage_index != wanted->index) continue;
            if (stream >= 16 || method != 0) { set_errorf("%s", "only default-method declarations with streams 0..15 are supported"); return nil; }
            size_t stream_stride = stream ? state->stream_stride[stream] : stride;
            if (stream && (!stream_stride || stream_stride > MR_MAX_VERTEX_STRIDE)) { set_errorf("declaration stream %u has no bound stride", stream); return nil; }
            size_t element_bytes = 0; MTLVertexFormat format = vertex_format(type, &element_bytes);
            if (format == MTLVertexFormatInvalid) { set_errorf("unsupported D3D declaration type %u", type); return nil; }
            if ((size_t)byte_offset + element_bytes > stream_stride) { set_errorf("%s", "vertex declaration exceeds stride"); return nil; }
            MTLVertexAttributeDescriptor *attribute = vd.attributes[wanted->location];
            attribute.format = format; attribute.offset = byte_offset; attribute.bufferIndex = 1 + stream; found = 1;
            if (stream) { used |= 1u << stream; vd.layouts[1 + stream].stride = stream_stride; vd.layouts[1 + stream].stepFunction = MTLVertexStepFunctionPerVertex; vd.layouts[1 + stream].stepRate = 1; }
            break;
        }
        if (!found) { set_errorf("declaration is missing shader semantic usage=%d index=%d", wanted->usage, wanted->index); return nil; }
    }
    for (size_t offset = 0; offset + 8 <= length; offset += 8)
        if (decl[offset] == 0xFF && decl[offset + 1] == 0 && decl[offset + 4] == 17) ended = 1;
    if (!ended) { set_errorf("%s", "vertex declaration has no terminator"); return nil; }
    if (used_streams) *used_streams = used;
    return vd;
}

static MTLBlendFactor d3d_blend_factor(int factor, int alpha) {
    switch (factor) {
        case 1: return MTLBlendFactorZero;
        case 2: return MTLBlendFactorOne;
        case 3: return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
        case 4: return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha;
        case 6: return MTLBlendFactorOneMinusSourceAlpha;
        case 7: return MTLBlendFactorDestinationAlpha;
        case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
        case 10: return alpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorOneMinusDestinationColor;
        case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 14: return alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
        case 15: return alpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
        default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation d3d_blend_operation(int op) {
    switch (op) {
        case 2: return MTLBlendOperationSubtract;
        case 3: return MTLBlendOperationReverseSubtract;
        case 4: return MTLBlendOperationMin;
        case 5: return MTLBlendOperationMax;
        default: return MTLBlendOperationAdd;
    }
}
/* src/dst/op are raw D3DBLEND/D3DBLENDOP values, consulted only for MR_BLEND_CUSTOM. */
static void configure_program_color(MTLRenderPipelineColorAttachmentDescriptor *ca, int blend, int src, int dst, int op, int mask, int overlay) {
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.writeMask = ((mask & 1) ? MTLColorWriteMaskRed : 0) | ((mask & 2) ? MTLColorWriteMaskGreen : 0) |
                   ((mask & 4) ? MTLColorWriteMaskBlue : 0) | ((mask & 8) ? MTLColorWriteMaskAlpha : 0);
    if (blend != MR_BLEND_NONE) {
        ca.blendingEnabled = YES; ca.rgbBlendOperation = MTLBlendOperationAdd; ca.alphaBlendOperation = MTLBlendOperationAdd;
        if (blend == MR_BLEND_CUSTOM) {
            ca.rgbBlendOperation = ca.alphaBlendOperation = d3d_blend_operation(op);
            ca.sourceRGBBlendFactor = d3d_blend_factor(src, 0); ca.destinationRGBBlendFactor = d3d_blend_factor(dst, 0);
            ca.sourceAlphaBlendFactor = d3d_blend_factor(src, 1); ca.destinationAlphaBlendFactor = d3d_blend_factor(dst, 1);
        } else if (blend == MR_BLEND_SRC_ALPHA) {
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha; ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            ca.sourceAlphaBlendFactor = overlay ? MTLBlendFactorOne : MTLBlendFactorSourceAlpha; ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        } else if (blend == MR_BLEND_MODULATE) {
            ca.sourceRGBBlendFactor = MTLBlendFactorDestinationColor; ca.destinationRGBBlendFactor = MTLBlendFactorZero;
            ca.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha; ca.destinationAlphaBlendFactor = MTLBlendFactorZero;
        } else if (blend == MR_BLEND_MODULATE2) {
            ca.sourceRGBBlendFactor = MTLBlendFactorDestinationColor; ca.destinationRGBBlendFactor = MTLBlendFactorSourceColor;
            ca.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha; ca.destinationAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        } else if (blend == MR_BLEND_SRC_ALPHA_ZERO || blend == MR_BLEND_SRC_ALPHA_ADD) {
            ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha; ca.destinationRGBBlendFactor = blend == MR_BLEND_SRC_ALPHA_ADD ? MTLBlendFactorOne : MTLBlendFactorZero;
            ca.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha; ca.destinationAlphaBlendFactor = blend == MR_BLEND_SRC_ALPHA_ADD ? MTLBlendFactorOne : MTLBlendFactorZero;
        } else if (blend == MR_BLEND_DEST_ALPHA_ADD) {
            ca.sourceRGBBlendFactor = MTLBlendFactorDestinationAlpha; ca.destinationRGBBlendFactor = MTLBlendFactorOne;
            ca.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha; ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
        } else if (blend == MR_BLEND_PREMULTIPLIED) {
            ca.sourceRGBBlendFactor = MTLBlendFactorOne; ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            ca.sourceAlphaBlendFactor = MTLBlendFactorOne; ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        } else {
            ca.sourceRGBBlendFactor = MTLBlendFactorOne; ca.destinationRGBBlendFactor = MTLBlendFactorOne;
            ca.sourceAlphaBlendFactor = MTLBlendFactorOne; ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
        }
    }
}

/* Fragment buffer 1 payloads shared by the programmable and fixed paths. */
static void pixel_extras(const mr_program_state *state, float out[8]) {
    out[0] = state->alpha_test_ref < 0 ? 0.f : (float)(state->alpha_test_ref > 255 ? 255 : state->alpha_test_ref);
    out[1] = state->fog_enable ? 1.f : 0.f; out[2] = out[3] = 0.f;
    out[4] = ((state->fog_color >> 16) & 255) / 255.f; out[5] = ((state->fog_color >> 8) & 255) / 255.f;
    out[6] = (state->fog_color & 255) / 255.f; out[7] = ((state->fog_color >> 24) & 255) / 255.f;
}
static void fixed_uniforms(const mr_program_state *state, float out[12]) {
    out[0] = ((state->texture_factor >> 16) & 255) / 255.f; out[1] = ((state->texture_factor >> 8) & 255) / 255.f;
    out[2] = (state->texture_factor & 255) / 255.f; out[3] = ((state->texture_factor >> 24) & 255) / 255.f;
    out[4] = state->alpha_test_ref < 0 ? -1.f : (float)(state->alpha_test_ref > 255 ? 255 : state->alpha_test_ref);
    out[5] = out[6] = out[7] = 0.f;
    out[8] = ((state->fog_color >> 16) & 255) / 255.f; out[9] = ((state->fog_color >> 8) & 255) / 255.f;
    out[10] = (state->fog_color & 255) / 255.f; out[11] = ((state->fog_color >> 24) & 255) / 255.f;
}

typedef struct FixedRHWIn {
    float pos[4], color[4], color1[4], uv[8][2];
    float fog[4];
} FixedRHWIn;

static id<MTLSamplerState> sampler_for(mr_context *c, const mr_program_sampler *sampler);
static int mr_tex_fallback(void);
/* Halo draws its menu rules and reticule ticks as lines, which only reached
 * the renderer once the front end started going through the panorama path. */
static int mr_primitive_supported(int prim) {
    return prim == MR_TRIANGLE_LIST || prim == MR_TRIANGLE_STRIP ||
           prim == MR_LINE_LIST || prim == MR_LINE_STRIP || prim == MR_POINT_LIST;
}
static MTLPrimitiveType mr_primitive_metal(int prim) {
    switch (prim) {
        case MR_LINE_LIST: return MTLPrimitiveTypeLine;
        case MR_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
        case MR_POINT_LIST: return MTLPrimitiveTypePoint;
        case MR_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
        default: return MTLPrimitiveTypeTriangle;
    }
}
/* Shapes drawn, for the triangle counter. */
static uint32_t mr_primitive_shapes(int prim, uint32_t count) {
    switch (prim) {
        case MR_POINT_LIST: return count;
        case MR_LINE_LIST: return count / 2;
        case MR_LINE_STRIP: return count ? count - 1 : 0;
        case MR_TRIANGLE_STRIP: return count >= 2 ? count - 2 : 0;
        default: return count / 3;
    }
}
static int pack_float_uniforms(const ms_shader *shader, const float *registers, uint32_t register_count, float packed[256][4]);
static NSString *inject_pixel_extras(const char *source, size_t bytes, int func, int fog);

/* ---------------------------------------------------------------------------
 * Pipelines without engine-thread stalls.
 *
 * Build75 built every pipeline at the first draw that needed it, on the
 * engine thread: MojoShader translation, one MSL library holding that
 * pipeline's vertex and fragment functions (entry points named after the
 * pipeline key), then the pipeline. The b30 headset session did 479 of them
 * for 12.4 s; each first sight of an effect was a visible hitch. Measured on
 * a Mac: a translated pixel shader costs 60-80 ms as a library of its own,
 * nearly all of it parsing <metal_texture>; the pipeline itself is a few ms
 * cold. And because entry names carried the pipeline key, every blend,
 * write-mask, declaration or target variant of one shader pair paid for the
 * whole library again.
 *
 * So:
 *  - a function is compiled once per distinct source text, alone, and named
 *    after that text, so every variant of a shader pair reuses it, and
 *    Metal's own caches recognise it in later launches (Metal keys compiled
 *    code on the library a function came from: batching several functions
 *    into one library is 15-20x cheaper cold, but the same function compiled
 *    in another batch next session misses both Metal's cache and the
 *    archive, so functions are never batched);
 *  - the game's shaders are registered when it creates them, and background
 *    workers translate and compile them before any draw;
 *  - every pipeline built is recorded (a manifest of recipes in the Caches
 *    directory) and rebuilt by the workers as soon as the same shaders are
 *    created in a later session, so a second session starts warm. Metal's
 *    own per-app cache then serves each function and pipeline in well under
 *    a millisecond. An MTLBinaryArchive keyed to the device and OS is
 *    available (HALO_PIPELINE_ARCHIVE=1) but off: with Metal's cache warm it
 *    saved nothing measurable, and Metal's archive serializer crashed in
 *    testing (always when appending to a loaded archive, and once, under
 *    load, writing a fresh one);
 *  - a draw whose pipeline is not ready builds it, or waits for the worker
 *    already building that one pipeline. Draws are never skipped: a skipped
 *    draw is a visible pop, and with several bearings rendered from one engine
 *    frame a pipeline finishing between bearings would draw an object in one
 *    bearing and not its neighbour.
 *
 * The functions are the same source the engine thread compiled before, the
 * vertex and fragment in separate libraries; only the entry names differ. Every
 * captured Halo pixel shader compiles on its own (469/469 in the 2026-09-14
 * audit), and MojoShader emits the headers each shader needs.
 *
 * Switches: HALO_PIPELINE_WORKERS (3; 0 builds everything at the draw, as
 * before, still sharing functions), HALO_SHADER_PREWARM=0 (no compiling at
 * shader creation), HALO_PIPELINE_CACHE=0 (no manifest), HALO_PIPELINE_CACHE_DIR,
 * HALO_PIPELINE_ARCHIVE=1, HALO_PIPELINE_TRACE=1 (log every build).
 */
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define MR_PH "MRENTRYPLACEHOLDER"      /* entry name while a source is keyed; replaced by mr_vs_/mr_fs_<hash> */
#define MR_FN_BUCKETS 4096u
#define MR_XLAT_BUCKETS 1024u
#define MR_REG_BUCKETS 1024u
#define MR_RECIPE_DECL 520              /* 64 declaration elements and the end marker, as d3d9.c copies them */
#define MR_RECIPE_MAX 8192u
#define MR_MANIFEST_MAGIC 0x4D505648u   /* "HVPM" */
#define MR_MANIFEST_VERSION 2u
#define MR_ARCHIVE_MAX_BYTES (UINT64_C(768) * 1024u * 1024u)
#define MR_RETRY_NS (UINT64_C(2000000000))

enum { MR_FN_QUEUED = 1, MR_FN_COMPILING, MR_FN_READY, MR_FN_FAILED };
enum { MR_PL_QUEUED = 1, MR_PL_BUILDING, MR_PL_READY, MR_PL_FAILED };
enum { MR_RECIPE_PROGRAM = 1, MR_RECIPE_FIXED = 2 };

/* Everything a pipeline build reads, without the per-draw values. Shaders are
 * referenced by content key and supplied by the game's shader creation. */
typedef struct mr_recipe {
    uint32_t kind; int32_t clip_space, overlay, has_ps;
    uint64_t key, vertex_key, pixel_key;
    uint32_t stride, declaration_bytes, stream_stride[16];
    int32_t blend, blend_src, blend_dst, blend_op, color_write_mask;
    int32_t alpha_enabled, alpha_func, fog_enable;
    uint8_t sampler_type[16], sampler_bound[16];
    mr_fixed_stage fixed_stages[8];
    uint8_t declaration[MR_RECIPE_DECL];
    int32_t radial_fog;                 /* v2 extension; v1 prefix is unchanged */
} mr_recipe;
#define MR_RECIPE_V1_BYTES 1080u
_Static_assert(offsetof(mr_recipe, radial_fog) == MR_RECIPE_V1_BYTES, "manifest v1 compatible prefix");
_Static_assert(sizeof(mr_recipe) == 1088, "manifest record layout");

typedef struct mr_fn {                  /* one compiled vertex or fragment function */
    struct mr_fn *next, *queue_next;
    uint64_t hash; int stage, state, attempts;
    uint64_t failed_at;
    char name[32];
    char *text;                         /* named source while queued for a worker */
    id<MTLFunction> fn;
    char *error;
} mr_fn;

typedef struct mr_xlat {                /* one MojoShader translation */
    struct mr_xlat *next;
    uint64_t key; int ok;
    ms_shader sh;                       /* sh.source uses MR_PH as the entry name */
} mr_xlat;

typedef struct mr_registered {          /* a shader the game created, tokens copied */
    struct mr_registered *next;
    uint64_t key; int stage;
    size_t bytes;
    uint32_t tokens[];
} mr_registered;

typedef struct { mr_registered *shader; uint32_t variants; } mr_translate_task;

typedef struct mr_compiler {
    pthread_mutex_t lock;
    pthread_cond_t changed;             /* a function or pipeline finished; a worker went idle */
    pthread_cond_t work;                /* workers: something was queued */
    mr_shared *s;
    mr_fn *fns[MR_FN_BUCKETS];
    mr_xlat *xlats[MR_XLAT_BUCKETS];
    mr_registered *registered[MR_REG_BUCKETS];
    mr_registered **pixel_shaders; unsigned pixel_shader_count, pixel_shader_cap;
    /* worker queues */
    mr_fn *fn_head, *fn_tail; unsigned fn_queued;
    mr_translate_task *translate; unsigned translate_head, translate_count, translate_cap;
    uint32_t *prepare; unsigned prepare_head, prepare_count, prepare_cap;
    uint32_t *build; unsigned build_head, build_count, build_cap;
    int workers, workers_wanted, idle, prewarm, radial_fog_prewarm;
    uint32_t variants_seen;             /* bit (alpha func * 2 + fog): injected pixel-shader variants in use */
    /* manifest */
    mr_recipe *recipes; uint32_t recipe_count; uint8_t *recipe_queued;
    _Atomic uint32_t recipes_recorded;
    /* persistence (runtime only; see mr_pipeline_store_open) */
    int store_open, manifest_fd;
    char dir[1024];
    dispatch_queue_t io;
    /* Metal cannot serialize an archive it loaded from a file once anything
     * is added to it (the write fails inside Metal and crashes; reproduced on
     * macOS 26), so the loaded archive is only read, and every pipeline this
     * session builds or loads goes into a fresh collector that replaces the
     * file once the prewarm has drained. */
    id<MTLBinaryArchive> archive, collector;
    NSURL *archive_url;
    unsigned archive_unsaved, archive_total; int archive_save_pending;
} mr_compiler;

/* Counters for the device report. */
static _Atomic uint64_t mr_stat_engine_builds, mr_stat_engine_build_ns, mr_stat_engine_functions,
    mr_stat_waits, mr_stat_wait_ns, mr_stat_bg_pipelines, mr_stat_bg_functions, mr_stat_bg_ns,
    mr_stat_prewarm_hits, mr_stat_archive_hits, mr_stat_archive_stored, mr_stat_shaders, mr_stat_recipes;
#define MR_ADD(counter, value) atomic_fetch_add_explicit(&(counter), (uint64_t)(value), memory_order_relaxed)
#define MR_GET(counter) atomic_load_explicit(&(counter), memory_order_relaxed)
static _Thread_local uint64_t mr_tl_wait_ns;   /* engine thread: time spent waiting inside one build */
static uint64_t mr_name_salt;                  /* tests: defeat Metal's own cache for cold measurements */
static const ms_shader mr_no_shader;

static uint64_t mr_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static int mr_env_int(const char *name, int fallback) {
    const char *e = getenv(name); return (e && e[0]) ? atoi(e) : fallback;
}
/* HALO_PIPELINE_TRACE=1 logs every build: where, what, how long. */
static int mr_pipeline_trace(void) {
    static _Atomic int v = -1; int t = atomic_load_explicit(&v, memory_order_relaxed);
    if (t < 0) { t = mr_env_int("HALO_PIPELINE_TRACE", 0) != 0; atomic_store_explicit(&v, t, memory_order_relaxed); }
    return t;
}

void mr_program_compile_stats(uint64_t *count, uint64_t *ns) {
    if (count) *count = MR_GET(mr_stat_engine_builds);
    if (ns) *ns = MR_GET(mr_stat_engine_build_ns);
}
void mr_pipeline_stats(mr_pipeline_stats_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    out->engine_builds = MR_GET(mr_stat_engine_builds); out->engine_build_ns = MR_GET(mr_stat_engine_build_ns);
    out->engine_functions = MR_GET(mr_stat_engine_functions);
    out->waits = MR_GET(mr_stat_waits); out->wait_ns = MR_GET(mr_stat_wait_ns);
    out->background_pipelines = MR_GET(mr_stat_bg_pipelines); out->background_functions = MR_GET(mr_stat_bg_functions);
    out->background_ns = MR_GET(mr_stat_bg_ns); out->prewarm_hits = MR_GET(mr_stat_prewarm_hits);
    out->archive_hits = MR_GET(mr_stat_archive_hits); out->archive_stored = MR_GET(mr_stat_archive_stored);
    out->shaders_registered = MR_GET(mr_stat_shaders); out->recipes_loaded = MR_GET(mr_stat_recipes);
}

/* ---- source text ---- */
static uint64_t text_hash(int stage, const char *text) {
    uint64_t h = hash_more(UINT64_C(1469598103934665603), &stage, sizeof stage);
    return hash_more(h, text, strlen(text));
}
static void fn_name(char out[32], int stage, uint64_t hash) {
    snprintf(out, 32, "mr_%s_%016llx", stage == MS_STAGE_VERTEX ? "vs" : "fs", (unsigned long long)(hash ^ mr_name_salt));
}
static char *text_named(const char *text, const char *name) {
    size_t ph = strlen(MR_PH), nl = strlen(name), count = 0, len = strlen(text);
    for (const char *p = strstr(text, MR_PH); p; p = strstr(p + ph, MR_PH)) count++;
    char *out = malloc(len + count * (nl > ph ? nl - ph : 0) + 1); if (!out) return NULL;
    char *w = out; const char *r = text;
    for (const char *p = strstr(r, MR_PH); p; p = strstr(r, MR_PH)) {
        memcpy(w, r, (size_t)(p - r)); w += p - r; memcpy(w, name, nl); w += nl; r = p + ph;
    }
    strcpy(w, r); return out;
}
/* ---- functions ---- */
static mr_fn *fn_find(mr_compiler *cc, uint64_t hash, int stage) {
    for (mr_fn *f = cc->fns[hash & (MR_FN_BUCKETS - 1u)]; f; f = f->next) if (f->hash == hash && f->stage == stage) return f;
    return NULL;
}
static mr_fn *fn_insert(mr_compiler *cc, uint64_t hash, int stage) {
    mr_fn *f = calloc(1, sizeof *f); if (!f) return NULL;
    f->hash = hash; f->stage = stage; fn_name(f->name, stage, hash);
    f->next = cc->fns[hash & (MR_FN_BUCKETS - 1u)]; cc->fns[hash & (MR_FN_BUCKETS - 1u)] = f;
    return f;
}
static void fn_dequeue(mr_compiler *cc, mr_fn *f) {
    mr_fn **link = &cc->fn_head, *prev = NULL;
    while (*link && *link != f) { prev = *link; link = &(*link)->queue_next; }
    if (!*link) return;
    *link = f->queue_next; if (cc->fn_tail == f) cc->fn_tail = prev;
    f->queue_next = NULL; cc->fn_queued--;
}
static id<MTLFunction> fn_compile(mr_shared *s, const char *named, const char *name, char *err, size_t errn) {
    @autoreleasepool {
    NSString *source = [[NSString alloc] initWithUTF8String:named];
    NSError *error = nil;
    id<MTLLibrary> library = source ? [s->dev newLibraryWithSource:source options:nil error:&error] : nil;
    if (!library) { snprintf(err, errn, "%s", error ? error.localizedDescription.UTF8String : "source is not UTF-8"); return nil; }
    id<MTLFunction> fn = [library newFunctionWithName:[NSString stringWithUTF8String:name]];
    if (!fn) snprintf(err, errn, "translated Metal entry point is missing");
    return fn;
    }
}
static void compiler_start_workers_locked(mr_compiler *cc);
/* Queue a function for a worker; nothing if it is known already. */
static void fn_enqueue(mr_compiler *cc, int stage, const char *text) {
    if (!text) return;
    uint64_t hash = text_hash(stage, text); char name[32]; fn_name(name, stage, hash);
    pthread_mutex_lock(&cc->lock);
    if (fn_find(cc, hash, stage)) { pthread_mutex_unlock(&cc->lock); return; }
    pthread_mutex_unlock(&cc->lock);
    char *named = text_named(text, name); if (!named) return;
    pthread_mutex_lock(&cc->lock);
    mr_fn *f = fn_find(cc, hash, stage);
    if (f || !(f = fn_insert(cc, hash, stage))) { pthread_mutex_unlock(&cc->lock); free(named); return; }
    f->text = named; f->state = MR_FN_QUEUED;
    if (cc->fn_tail) cc->fn_tail->queue_next = f; else cc->fn_head = f;
    cc->fn_tail = f; cc->fn_queued++;
    compiler_start_workers_locked(cc); pthread_cond_signal(&cc->work);
    pthread_mutex_unlock(&cc->lock);
}
/* The function for `text`, compiled now if nobody has it (a function still
 * queued for a worker is taken over); one another thread is compiling is
 * waited for. `engine`: the caller is the engine thread (accounting). */
static id<MTLFunction> fn_require(mr_compiler *cc, int stage, const char *text, int engine, char *err, size_t errn) {
    uint64_t hash = text_hash(stage, text); char name[32]; fn_name(name, stage, hash);
    int claimed = 0; uint64_t waited = 0;
    pthread_mutex_lock(&cc->lock);
    mr_fn *f = fn_find(cc, hash, stage);
    if (!f) {
        if (!(f = fn_insert(cc, hash, stage))) { pthread_mutex_unlock(&cc->lock); snprintf(err, errn, "function cache allocation failed"); return nil; }
        f->state = MR_FN_COMPILING; claimed = 1;
    }
    while (!claimed) {
        if (f->state == MR_FN_READY) { id<MTLFunction> fn = f->fn; pthread_mutex_unlock(&cc->lock); if (waited) { MR_ADD(mr_stat_waits, 1); MR_ADD(mr_stat_wait_ns, waited); } return fn; }
        if (f->state == MR_FN_FAILED) {
            if (f->attempts < 3 && mr_now() - f->failed_at > MR_RETRY_NS) { f->state = MR_FN_COMPILING; claimed = 1; break; }
            snprintf(err, errn, "%s", f->error ? f->error : "compile failed"); pthread_mutex_unlock(&cc->lock); return nil;
        }
        if (f->state == MR_FN_QUEUED) { fn_dequeue(cc, f); free(f->text); f->text = NULL; f->state = MR_FN_COMPILING; claimed = 1; break; }
        uint64_t t = mr_now(); pthread_cond_wait(&cc->changed, &cc->lock); uint64_t dt = mr_now() - t;
        if (engine) { waited += dt; mr_tl_wait_ns += dt; }
    }
    pthread_mutex_unlock(&cc->lock);
    if (waited) { MR_ADD(mr_stat_waits, 1); MR_ADD(mr_stat_wait_ns, waited); }
    uint64_t started = mr_now();
    char *named = text_named(text, name);
    id<MTLFunction> fn = named ? fn_compile(cc->s, named, name, err, errn) : nil;
    if (!named) snprintf(err, errn, "shader source allocation failed");
    free(named);
    if (engine) MR_ADD(mr_stat_engine_functions, 1); else { MR_ADD(mr_stat_bg_functions, 1); MR_ADD(mr_stat_bg_ns, mr_now() - started); }
    pthread_mutex_lock(&cc->lock);
    if (fn) { f->fn = fn; f->state = MR_FN_READY; free(f->error); f->error = NULL; }
    else { f->state = MR_FN_FAILED; free(f->error); f->error = strdup(err); f->failed_at = mr_now(); f->attempts++; }
    pthread_cond_broadcast(&cc->changed);
    pthread_mutex_unlock(&cc->lock);
    return fn;
}
/* Worker: compile the oldest queued function. Called and returns with the
 * lock held. */
static void fn_run_queued(mr_compiler *cc) {
    mr_fn *f = cc->fn_head; fn_dequeue(cc, f);
    f->state = MR_FN_COMPILING;
    char *text = f->text; f->text = NULL;
    pthread_mutex_unlock(&cc->lock);
    uint64_t started = mr_now(); char err[1024] = "";
    id<MTLFunction> fn = fn_compile(cc->s, text, f->name, err, sizeof err);
    free(text);
    MR_ADD(mr_stat_bg_functions, 1); MR_ADD(mr_stat_bg_ns, mr_now() - started);
    if (mr_pipeline_trace()) NSLog(@"[pipelines] worker compiled %s in %.1f ms%s", f->name, (mr_now() - started) / 1e6, fn ? "" : " (failed)");
    pthread_mutex_lock(&cc->lock);
    if (fn) { f->fn = fn; f->state = MR_FN_READY; }
    else { f->state = MR_FN_FAILED; free(f->error); f->error = strdup(err); f->failed_at = mr_now(); f->attempts++; }
    pthread_cond_broadcast(&cc->changed);
}

/* ---- translations ---- */
static const mr_xlat *xlat_get(mr_compiler *cc, int stage, const uint32_t *tokens, size_t bytes, uint64_t token_key, const int *sampler_types, int radial_fog) {
    if (stage != MS_STAGE_VERTEX) radial_fog = 0;
    if (!token_key) token_key = hash_more(UINT64_C(1469598103934665603), tokens, tokens ? bytes : 0);
    /* ps_1_x declares no sampler kinds; the bound kinds are forced into the
     * translation (metalshader.c), so they are part of its identity. */
    int mapped = sampler_types && stage == MS_STAGE_PIXEL && tokens && bytes >= 4 && ((tokens[0] >> 8) & 0xFFu) < 2;
    uint64_t key = hash_more(UINT64_C(1469598103934665603), &stage, sizeof stage);
    key = hash_more(key, &token_key, sizeof token_key); key = hash_more(key, &mapped, sizeof mapped);
    if (mapped) key = hash_more(key, sampler_types, 16 * sizeof(int));
    /* Keep the original translation identity for mode 0. The rewrite and
     * its normalized unindexed FOG output belong only to opt-in variants. */
    if (radial_fog) key = hash_more(key, &radial_fog, sizeof radial_fog);
    pthread_mutex_lock(&cc->lock);
    for (mr_xlat *x = cc->xlats[key & (MR_XLAT_BUCKETS - 1u)]; x; x = x->next) if (x->key == key) { pthread_mutex_unlock(&cc->lock); return x; }
    pthread_mutex_unlock(&cc->lock);
    mr_xlat *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->key = key;
    uint32_t *radial_tokens = NULL; size_t radial_bytes = 0;
    if (radial_fog && halo_radial_fog_rewrite(tokens, bytes, radial_fog, &radial_tokens, &radial_bytes) < 0) {
        free(x); return NULL;
    }
    x->ok = !ms_translate_shader_mapped(radial_tokens ? radial_tokens : tokens,
                                       radial_tokens ? radial_bytes : bytes, stage, MR_PH,
                                       mapped ? sampler_types : NULL, mapped ? 16 : 0, &x->sh);
    /* MojoShader labels VS 1.x oFog with RASTOUT register number 1 although
     * its emitted semantic is the unindexed [[user(fog)]]. Correct only the
     * rewritten variant, so HALO_RADIAL_FOG=0 preserves Build78 behavior. */
    if (x->ok && radial_tokens) for (int i = 0; i < x->sh.output_count; ++i)
        if (x->sh.outputs[i].usage == 11) x->sh.outputs[i].index = 0;
    free(radial_tokens);
    pthread_mutex_lock(&cc->lock);
    for (mr_xlat *y = cc->xlats[key & (MR_XLAT_BUCKETS - 1u)]; y; y = y->next)
        if (y->key == key) { pthread_mutex_unlock(&cc->lock); ms_shader_destroy(&x->sh); free(x); return y; }
    x->next = cc->xlats[key & (MR_XLAT_BUCKETS - 1u)]; cc->xlats[key & (MR_XLAT_BUCKETS - 1u)] = x;
    pthread_mutex_unlock(&cc->lock);
    return x;
}
static char *xlat_text(const mr_xlat *x) {
    char *t = malloc(x->sh.source_bytes + 1); if (!t) return NULL;
    memcpy(t, x->sh.source, x->sh.source_bytes); t[x->sh.source_bytes] = 0; return t;
}
static char *nsstring_text(NSString *s) { return s ? strdup(s.UTF8String) : NULL; }

/* The texts a pipeline needs. For MojoShader pixel shaders, `extras` injects
 * D3D's fixed alpha test and vertex fog; `unmapped` is the retry that lets
 * MojoShader infer ps_1_x sampler kinds from the instructions. */
typedef struct mr_texts { const mr_xlat *vs, *ps; char *vs_text, *fs_text; int extras, variant; } mr_texts;
static void texts_free(mr_texts *t) { free(t->vs_text); free(t->fs_text); memset(t, 0, sizeof *t); }
static void sampler_types_of(const mr_program_state *state, int out[16]) {
    for (int stage = 0; stage < 16; ++stage) out[stage] = state->samplers[stage].texture ? state->samplers[stage].type : -1;
}
static int pixel_text(mr_compiler *cc, const mr_program_state *state, const ms_shader *vs, int unmapped, mr_texts *t) {
    int sampler_types[16]; sampler_types_of(state, sampler_types);
    t->ps = xlat_get(cc, MS_STAGE_PIXEL, state->pixel_tokens, state->pixel_token_bytes, state->pixel_key, unmapped ? NULL : sampler_types, 0);
    if (!t->ps) { set_errorf("%s", "translation cache allocation failed"); return -1; }
    if (!t->ps->ok) { set_errorf("MojoShader pixel translation failed: %s", t->ps->sh.error); return -1; }
    int alpha = state->alpha_test_ref >= 0, fog = vs && state->fog_enable && shader_has_output(vs, 11, 0);
    t->extras = alpha || fog; t->variant = (alpha ? state->alpha_test_func : 0) * 2 + fog;
    if (t->extras) t->fs_text = nsstring_text(inject_pixel_extras(t->ps->sh.source, t->ps->sh.source_bytes, alpha ? state->alpha_test_func : 0, fog));
    else t->fs_text = xlat_text(t->ps);
    return t->fs_text ? 0 : -1;
}
static int program_texts(mr_compiler *cc, const mr_program_state *state, int unmapped, mr_texts *t) {
    memset(t, 0, sizeof *t);
    t->vs = xlat_get(cc, MS_STAGE_VERTEX, state->vertex_tokens, state->vertex_token_bytes, state->vertex_key, NULL, state->radial_fog);
    if (!t->vs) { set_errorf("%s", "translation cache allocation failed"); return -1; }
    if (!t->vs->ok) { set_errorf("MojoShader vertex translation failed: %s", t->vs->sh.error); return -1; }
    if (!(t->vs_text = xlat_text(t->vs))) return -1;
    if (state->pixel_tokens) return pixel_text(cc, state, &t->vs->sh, unmapped, t);
    return (t->fs_text = nsstring_text(make_fixed_fragment(&t->vs->sh, state, @MR_PH))) ? 0 : -1;
}
static int fixed_rhw_fog_enabled(const mr_program_state *state) {
    return state->fog_enable && state->radial_fog;
}
static NSString *fixed_rhw_vertex_source(int clip_space, int fog) {
    NSMutableString *vertex = [NSMutableString stringWithString:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct MRFixedRHWVertex { float4 pos; float4 color; float4 color1; float2 uv[8];"];
    [vertex appendString:fog ? @" float4 fog; };\n" : @" };\n"];
    [vertex appendString:@"struct MRFixedRHWUniforms { float2 size; };\n"
         "struct MRFixedRHWOutput { float4 pos [[position]]; float4 color0 [[user(color0)]]; float4 color1 [[user(color1)]];\n"];
    for (int i = 0; i < 8; ++i) [vertex appendFormat:@" float4 texcoord%d [[user(texcoord%d)]];\n", i, i];
    if (fog) [vertex appendString:@" float4 fog [[user(fog)]];\n"];
    [vertex appendString:@"};\nvertex MRFixedRHWOutput " MR_PH "(uint vid [[vertex_id]], const device MRFixedRHWVertex *v [[buffer(0)]], constant MRFixedRHWUniforms &u [[buffer(1)]]) {\n"
                          " MRFixedRHWVertex i=v[vid]; MRFixedRHWOutput o;\n"];
    if (clip_space) [vertex appendString:@" o.pos=i.pos;\n"];
    else [vertex appendString:@" float rhw=abs(i.pos.w)>1.0e-20?i.pos.w:1.0; float w=1.0/rhw;\n"
                              " float2 ndc=float2(i.pos.x/u.size.x*2.0-1.0,1.0-i.pos.y/u.size.y*2.0); o.pos=float4(ndc*w,i.pos.z*w,w);\n"];
    [vertex appendString:@" o.color0=i.color; o.color1=i.color1;\n"];
    if (fog) [vertex appendString:@" o.fog=i.fog;\n"];
    for (int i = 0; i < 8; ++i) [vertex appendFormat:@" o.texcoord%d=float4(i.uv[%d],0.0,1.0);\n", i, i];
    [vertex appendString:@" return o; }\n"];
    return vertex;
}
static int fixed_rhw_texts(mr_compiler *cc, const mr_program_state *state, int clip_space, mr_texts *t) {
    memset(t, 0, sizeof *t);
    int fog = fixed_rhw_fog_enabled(state);
    if (!(t->vs_text = nsstring_text(fixed_rhw_vertex_source(clip_space, fog)))) return -1;
    /* The CPU transformed fixed-function path carries its per-vertex range
     * fog factor; ordinary RHW draws keep fog disabled as before. */
    ms_shader fake = {0};
    if (fog) fake.outputs[fake.output_count++] = (ms_semantic){ 11, 0, -1 };
    if (state->pixel_tokens) return pixel_text(cc, state, &fake, 0, t);
    fake.outputs[fake.output_count++] = (ms_semantic){ 10, 0, 0 }; /* COLOR0 */
    fake.outputs[fake.output_count++] = (ms_semantic){ 10, 1, 1 }; /* COLOR1 (specular) */
    for (int i = 0; i < 8; ++i) fake.outputs[fake.output_count++] = (ms_semantic){ 5, i, i + 2 };
    return (t->fs_text = nsstring_text(make_fixed_fragment(&fake, state, @MR_PH))) ? 0 : -1;
}

/* ---- pipelines ---- */
static void archive_add(mr_compiler *cc, MTLRenderPipelineDescriptor *pd);
static id<MTLRenderPipelineState> pipeline_create(mr_compiler *cc, MTLRenderPipelineDescriptor *pd, char *err, size_t errn) {
    pthread_mutex_lock(&cc->lock); id<MTLBinaryArchive> archive = cc->archive, collector = cc->collector; pthread_mutex_unlock(&cc->lock);
    NSError *error = nil; id<MTLRenderPipelineState> pipeline = nil;
    if (archive) {
        pd.binaryArchives = @[archive];
        pipeline = [cc->s->dev newRenderPipelineStateWithDescriptor:pd options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error];
        if (pipeline) MR_ADD(mr_stat_archive_hits, 1);
        else if (mr_pipeline_trace()) NSLog(@"[pipelines] archive miss: %@", error.localizedDescription);
        error = nil;
    }
    if (!pipeline) pipeline = [cc->s->dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) { snprintf(err, errn, "%s", error ? error.localizedDescription.UTF8String : "unknown error"); return nil; }
    if (collector) archive_add(cc, pd);
    return pipeline;
}
static uint32_t slot_hash(uint64_t key) { key ^= key >> 31; key *= UINT64_C(0x9E3779B97F4A7C15); return (uint32_t)(key >> 32); }
static mr_program_cache *program_lookup(mr_shared *s, uint64_t key) {
    for (uint32_t h = slot_hash(key) & (MR_PROGRAM_SLOTS - 1u), n = 0; n < MR_PROGRAM_SLOTS; ++n, h = (h + 1u) & (MR_PROGRAM_SLOTS - 1u)) {
        uint32_t i = s->program_index[h]; if (!i) return NULL;
        if (s->programs[i - 1].key == key) return &s->programs[i - 1];
    }
    return NULL;
}
static mr_program_cache *program_insert(mr_shared *s, uint64_t key) {
    if (s->program_count >= MR_MAX_PROGRAMS) return NULL;
    uint32_t h = slot_hash(key) & (MR_PROGRAM_SLOTS - 1u);
    while (s->program_index[h]) h = (h + 1u) & (MR_PROGRAM_SLOTS - 1u);
    mr_program_cache *e = &s->programs[s->program_count];
    e->key = key; e->state = 0; e->background = 0; e->vs = e->ps = &mr_no_shader; e->pipeline = nil; e->recipe = NULL; e->error = NULL; e->attempts = 0;
    s->program_index[h] = ++s->program_count;
    return e;
}
static mr_fixed_rhw_cache *fixed_rhw_lookup(mr_shared *s, uint64_t key) {
    for (uint32_t h = slot_hash(key) & (MR_FIXED_RHW_SLOTS - 1u), n = 0; n < MR_FIXED_RHW_SLOTS; ++n, h = (h + 1u) & (MR_FIXED_RHW_SLOTS - 1u)) {
        uint32_t i = s->fixed_rhw_index[h]; if (!i) return NULL;
        if (s->fixed_rhw_programs[i - 1].key == key) return &s->fixed_rhw_programs[i - 1];
    }
    return NULL;
}
static mr_fixed_rhw_cache *fixed_rhw_insert(mr_shared *s, uint64_t key) {
    if (s->fixed_rhw_program_count >= MR_MAX_FIXED_RHW_PROGRAMS) return NULL;
    uint32_t h = slot_hash(key) & (MR_FIXED_RHW_SLOTS - 1u);
    while (s->fixed_rhw_index[h]) h = (h + 1u) & (MR_FIXED_RHW_SLOTS - 1u);
    mr_fixed_rhw_cache *e = &s->fixed_rhw_programs[s->fixed_rhw_program_count];
    e->key = key; e->state = 0; e->background = 0; e->ps = &mr_no_shader; e->pipeline = nil; e->recipe = NULL; e->error = NULL; e->attempts = 0;
    s->fixed_rhw_index[h] = ++s->fixed_rhw_program_count;
    return e;
}

static uint64_t program_key(const mr_program_state *state, size_t stride, int overlay) {
    uint64_t key = UINT64_C(1469598103934665603);
    if (state->vertex_key) key = hash_more(key, &state->vertex_key, sizeof state->vertex_key);
    else key = hash_more(key, state->vertex_tokens, state->vertex_token_bytes);
    if (state->pixel_tokens) {
        if (state->pixel_key) key = hash_more(key, &state->pixel_key, sizeof state->pixel_key);
        else key = hash_more(key, state->pixel_tokens, state->pixel_token_bytes);
    }
    key = hash_more(key, state->declaration, state->declaration_bytes); key = hash_more(key, &stride, sizeof stride);
    key = hash_more(key, state->stream_stride, sizeof state->stream_stride);
    key = hash_more(key, &overlay, sizeof overlay);
    key = hash_more(key, &state->blend, sizeof state->blend); key = hash_more(key, &state->color_write_mask, sizeof state->color_write_mask);
    if (state->blend == MR_BLEND_CUSTOM) { key = hash_more(key, &state->blend_src, sizeof state->blend_src); key = hash_more(key, &state->blend_dst, sizeof state->blend_dst); key = hash_more(key, &state->blend_op, sizeof state->blend_op); }
    int alpha_enabled = state->alpha_test_ref >= 0; key = hash_more(key, &alpha_enabled, sizeof alpha_enabled);
    if (alpha_enabled) key = hash_more(key, &state->alpha_test_func, sizeof state->alpha_test_func);
    key = hash_more(key, &state->fog_enable, sizeof state->fog_enable);
    if (state->radial_fog) key = hash_more(key, &state->radial_fog, sizeof state->radial_fog);
    int sampler_types[16]; sampler_types_of(state, sampler_types);
    key = hash_more(key, sampler_types, sizeof sampler_types);
    if (!state->pixel_tokens) key = hash_more(key, state->fixed_stages, sizeof state->fixed_stages);
    return key ? key : 1;
}
static uint64_t fixed_rhw_key(const mr_program_state *state, int overlay, int clip_space) {
    uint64_t key = UINT64_C(1469598103934665603);
    int has_ps = state->pixel_tokens != NULL;
    if (has_ps && state->pixel_key) key = hash_more(key, &state->pixel_key, sizeof state->pixel_key);
    else if (has_ps) key = hash_more(key, state->pixel_tokens, state->pixel_token_bytes);
    else key = hash_more(key, state->fixed_stages, sizeof state->fixed_stages);
    key = hash_more(key, &has_ps, sizeof has_ps);
    key = hash_more(key, &state->blend, sizeof state->blend);
    if (state->blend == MR_BLEND_CUSTOM) { key = hash_more(key, &state->blend_src, sizeof state->blend_src); key = hash_more(key, &state->blend_dst, sizeof state->blend_dst); key = hash_more(key, &state->blend_op, sizeof state->blend_op); }
    key = hash_more(key, &state->color_write_mask, sizeof state->color_write_mask);
    int sampler_types[16]; sampler_types_of(state, sampler_types);
    key = hash_more(key, sampler_types, sizeof sampler_types);
    int alpha_enabled = state->alpha_test_ref >= 0;
    key = hash_more(key, &alpha_enabled, sizeof alpha_enabled);
    if (alpha_enabled) key = hash_more(key, &state->alpha_test_func, sizeof state->alpha_test_func);
    key = hash_more(key, &overlay, sizeof overlay);
    key = hash_more(key, &clip_space, sizeof clip_space);
    if (state->radial_fog) {
        key = hash_more(key, &state->fog_enable, sizeof state->fog_enable);
        key = hash_more(key, &state->radial_fog, sizeof state->radial_fog);
    }
    return key ? key : 1;
}

/* Build a programmable pipeline into `out` (vs, ps, has_pixel_shader, extras,
 * streams, pipeline). Engine thread (`engine`) or worker; errors go to
 * set_errorf. `variant` receives the injected pixel variant for prewarming. */
static int program_build(mr_compiler *cc, const mr_program_state *state, size_t stride, int overlay, int engine,
                         mr_program_cache *out, int *variant) {
    @autoreleasepool {
    mr_texts t;
    if (program_texts(cc, state, 0, &t)) { texts_free(&t); return -1; }
    char err[1024] = "";
    id<MTLFunction> vertex = fn_require(cc, MS_STAGE_VERTEX, t.vs_text, engine, err, sizeof err);
    id<MTLFunction> fragment = vertex ? fn_require(cc, MS_STAGE_PIXEL, t.fs_text, engine, err, sizeof err) : nil;
    if (vertex && !fragment && state->pixel_tokens) {
        /* ps_1_x carries no sampler declarations, so the bound texture kinds are
         * forced into the translation. When a stage's bound kind disagrees with
         * the kind the instruction samples (a 2D texture on a texm3x3 stage,
         * say), the generated Metal has no matching sample overload. Retry
         * letting MojoShader infer every sampler kind from the instructions. */
        mr_texts retry = {0}; retry.vs = t.vs;
        if (!pixel_text(cc, state, &t.vs->sh, 1, &retry)) {
            char retry_err[1024] = "";
            fragment = fn_require(cc, MS_STAGE_PIXEL, retry.fs_text, engine, retry_err, sizeof retry_err);
            if (fragment) { free(t.fs_text); t.fs_text = retry.fs_text; retry.fs_text = NULL; t.ps = retry.ps; t.extras = retry.extras; t.variant = retry.variant; }
            else snprintf(err, sizeof err, "%s", retry_err);
        }
        free(retry.fs_text);
    }
    if (!vertex || !fragment) { set_errorf("Metal shader compilation failed: %s", err); texts_free(&t); return -1; }
    uint32_t streams = 0;
    MTLVertexDescriptor *vd = make_vertex_descriptor(&t.vs->sh, state->declaration, state->declaration_bytes, stride, state, &streams);
    if (!vd) { texts_free(&t); return -1; }
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vertex; pd.fragmentFunction = fragment; pd.vertexDescriptor = vd;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    configure_program_color(pd.colorAttachments[0], state->blend, state->blend_src, state->blend_dst, state->blend_op, state->color_write_mask, overlay);
    id<MTLRenderPipelineState> pipeline = pipeline_create(cc, pd, err, sizeof err);
    if (!pipeline) { set_errorf("Metal programmable pipeline failed: %s", err); texts_free(&t); return -1; }
    out->vs = &t.vs->sh; out->ps = t.ps ? &t.ps->sh : &mr_no_shader;
    out->has_pixel_shader = state->pixel_tokens != NULL; out->extras = state->pixel_tokens ? t.extras : 0;
    out->streams = streams; out->pipeline = pipeline;
    if (variant) *variant = state->pixel_tokens && t.extras ? t.variant : 0;
    texts_free(&t); return 0;
    }
}
static int fixed_rhw_build(mr_compiler *cc, const mr_program_state *state, int overlay, int clip_space, int engine,
                           mr_fixed_rhw_cache *out, int *variant) {
    @autoreleasepool {
    mr_texts t;
    if (fixed_rhw_texts(cc, state, clip_space, &t)) { texts_free(&t); return -1; }
    char err[1024] = "";
    id<MTLFunction> vertex = fn_require(cc, MS_STAGE_VERTEX, t.vs_text, engine, err, sizeof err);
    id<MTLFunction> fragment = vertex ? fn_require(cc, MS_STAGE_PIXEL, t.fs_text, engine, err, sizeof err) : nil;
    if (!vertex || !fragment) { set_errorf("fixed RHW Metal shader compile failed: %s", err); texts_free(&t); return -1; }
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = vertex; pd.fragmentFunction = fragment;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    configure_program_color(pd.colorAttachments[0], state->blend, state->blend_src, state->blend_dst, state->blend_op, state->color_write_mask, overlay);
    id<MTLRenderPipelineState> pipeline = pipeline_create(cc, pd, err, sizeof err);
    if (!pipeline) { set_errorf("fixed RHW Metal pipeline failed: %s", err); texts_free(&t); return -1; }
    out->ps = t.ps ? &t.ps->sh : &mr_no_shader; out->has_ps = state->pixel_tokens != NULL;
    out->extras = state->pixel_tokens ? t.extras : 0; out->pipeline = pipeline;
    if (variant) *variant = state->pixel_tokens && t.extras ? t.variant : 0;
    texts_free(&t); return 0;
    }
}

/* ---- recipes (the manifest) ---- */
static void recipe_from_state(mr_recipe *r, int kind, const mr_program_state *state, size_t stride, int overlay, int clip_space, uint64_t key) {
    memset(r, 0, sizeof *r);
    r->kind = (uint32_t)kind; r->clip_space = clip_space; r->overlay = overlay; r->has_ps = state->pixel_tokens != NULL;
    r->key = key; r->vertex_key = kind == MR_RECIPE_PROGRAM ? state->vertex_key : 0; r->pixel_key = r->has_ps ? state->pixel_key : 0;
    r->stride = (uint32_t)stride;
    if (kind == MR_RECIPE_PROGRAM) { r->declaration_bytes = (uint32_t)state->declaration_bytes; memcpy(r->declaration, state->declaration, state->declaration_bytes); }
    memcpy(r->stream_stride, state->stream_stride, sizeof r->stream_stride);
    r->blend = state->blend; r->blend_src = state->blend_src; r->blend_dst = state->blend_dst; r->blend_op = state->blend_op;
    r->color_write_mask = state->color_write_mask; r->alpha_enabled = state->alpha_test_ref >= 0;
    r->alpha_func = state->alpha_test_func; r->fog_enable = state->fog_enable; r->radial_fog = state->radial_fog;
    for (int i = 0; i < 16; ++i) { r->sampler_type[i] = state->samplers[i].type; r->sampler_bound[i] = state->samplers[i].texture != 0; }
    memcpy(r->fixed_stages, state->fixed_stages, sizeof r->fixed_stages);
}
static int recipe_recordable(int kind, const mr_program_state *state) {
    if (kind == MR_RECIPE_PROGRAM && (!state->vertex_key || state->declaration_bytes > MR_RECIPE_DECL)) return 0;
    return !state->pixel_tokens || state->pixel_key;
}
static const mr_registered *registered_find(mr_compiler *cc, int stage, uint64_t key) {
    for (mr_registered *r = cc->registered[key & (MR_REG_BUCKETS - 1u)]; r; r = r->next) if (r->key == key && r->stage == stage) return r;
    return NULL;
}
/* Rebuild the draw state a recipe was recorded from. Caller holds the lock. */
static int recipe_state_locked(mr_compiler *cc, const mr_recipe *r, mr_program_state *state) {
    memset(state, 0, sizeof *state);
    if (r->kind == MR_RECIPE_PROGRAM) {
        const mr_registered *vs = registered_find(cc, MS_STAGE_VERTEX, r->vertex_key); if (!vs) return -1;
        state->vertex_tokens = vs->tokens; state->vertex_token_bytes = vs->bytes; state->vertex_key = r->vertex_key;
        state->declaration = r->declaration; state->declaration_bytes = r->declaration_bytes;
    }
    if (r->has_ps) {
        const mr_registered *ps = registered_find(cc, MS_STAGE_PIXEL, r->pixel_key); if (!ps) return -1;
        state->pixel_tokens = ps->tokens; state->pixel_token_bytes = ps->bytes; state->pixel_key = r->pixel_key;
    }
    memcpy(state->stream_stride, r->stream_stride, sizeof state->stream_stride);
    for (int i = 0; i < 16; ++i) { state->samplers[i].type = r->sampler_type[i]; state->samplers[i].texture = r->sampler_bound[i] ? 1u : 0u; }
    memcpy(state->fixed_stages, r->fixed_stages, sizeof state->fixed_stages);
    state->blend = r->blend; state->blend_src = r->blend_src; state->blend_dst = r->blend_dst; state->blend_op = r->blend_op;
    state->color_write_mask = r->color_write_mask; state->alpha_test_ref = r->alpha_enabled ? 0 : -1;
    state->alpha_test_func = r->alpha_func; state->fog_enable = r->fog_enable; state->radial_fog = r->radial_fog;
    return 0;
}
static void push_u32(uint32_t **items, unsigned *head, unsigned *count, unsigned *cap, uint32_t value) {
    if (*head + *count == *cap) {
        if (*head) { memmove(*items, *items + *head, *count * sizeof **items); *head = 0; }
        else { unsigned next = *cap ? *cap * 2 : 256; uint32_t *grown = realloc(*items, next * sizeof **items); if (!grown) return; *items = grown; *cap = next; }
    }
    (*items)[*head + (*count)++] = value;
}
/* Queue every recipe whose shaders have all been created. Caller holds the lock. */
static void recipes_scan_locked(mr_compiler *cc, uint64_t shader_key) {
    for (uint32_t i = 0; i < cc->recipe_count; ++i) {
        const mr_recipe *r = &cc->recipes[i];
        /* Keep experimental recipes on disk when the switch is disabled,
         * but avoid spending worker time compiling variants it cannot draw. */
        if (r->radial_fog && !cc->radial_fog_prewarm) continue;
        if (cc->recipe_queued[i] || (shader_key && r->vertex_key != shader_key && r->pixel_key != shader_key)) continue;
        if (r->kind == MR_RECIPE_PROGRAM && !registered_find(cc, MS_STAGE_VERTEX, r->vertex_key)) continue;
        if (r->has_ps && !registered_find(cc, MS_STAGE_PIXEL, r->pixel_key)) continue;
        cc->recipe_queued[i] = 1;
        push_u32(&cc->prepare, &cc->prepare_head, &cc->prepare_count, &cc->prepare_cap, i);
    }
    if (cc->prepare_count) { compiler_start_workers_locked(cc); pthread_cond_signal(&cc->work); }
}

/* ---- workers ---- */
static void *compiler_worker(void *arg);
static void compiler_start_workers_locked(mr_compiler *cc) {
    while (cc->workers < cc->workers_wanted) {
        pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_set_qos_class_np(&attr, QOS_CLASS_UTILITY, 0);
        pthread_t thread; int failed = pthread_create(&thread, &attr, compiler_worker, cc);
        pthread_attr_destroy(&attr);
        if (failed) { cc->workers_wanted = cc->workers; break; }
        cc->workers++;
    }
}
static void note_variant(mr_compiler *cc, int variant) {
    if (variant <= 0 || variant >= 32) return;
    pthread_mutex_lock(&cc->lock);
    uint32_t bit = 1u << variant;
    if (!(cc->variants_seen & bit)) {
        cc->variants_seen |= bit;
        /* A new alpha-test/fog combination: compile it for every pixel shader. */
        if (cc->prewarm && cc->workers_wanted) {
            for (unsigned i = 0; i < cc->pixel_shader_count; ++i) {
                if (cc->translate_head + cc->translate_count == cc->translate_cap) {
                    if (cc->translate_head) { memmove(cc->translate, cc->translate + cc->translate_head, cc->translate_count * sizeof *cc->translate); cc->translate_head = 0; }
                    else { unsigned next = cc->translate_cap ? cc->translate_cap * 2 : 256; mr_translate_task *grown = realloc(cc->translate, next * sizeof *grown); if (!grown) break; cc->translate = grown; cc->translate_cap = next; }
                }
                cc->translate[cc->translate_head + cc->translate_count++] = (mr_translate_task){ cc->pixel_shaders[i], bit };
            }
            compiler_start_workers_locked(cc); pthread_cond_signal(&cc->work);
        }
    }
    pthread_mutex_unlock(&cc->lock);
}
static void worker_translate(mr_compiler *cc, mr_translate_task task) {
    mr_registered *r = task.shader;
    pthread_mutex_lock(&cc->lock); recipes_scan_locked(cc, r->key); pthread_mutex_unlock(&cc->lock);
    if (!task.variants) return;
    const mr_xlat *x = xlat_get(cc, r->stage, r->tokens, r->bytes, r->key, NULL, 0);
    if (!x || !x->ok) return;
    if (r->stage == MS_STAGE_VERTEX) {
        char *t = xlat_text(x); fn_enqueue(cc, MS_STAGE_VERTEX, t); free(t);
        /* Shader creation happens before the first draw. With the experiment
         * enabled, warm each eligible radial variant here as well as recipes. */
        if (cc->radial_fog_prewarm) for (int mode = HALO_RADIAL_FOG_DEPTH; mode <= HALO_RADIAL_FOG_VIEW_PLANE; ++mode) {
            if (!halo_radial_fog_terms(r->tokens, r->bytes, mode)) continue;
            const mr_xlat *radial = xlat_get(cc, r->stage, r->tokens, r->bytes, r->key, NULL, mode);
            if (radial && radial->ok) { char *text = xlat_text(radial); fn_enqueue(cc, MS_STAGE_VERTEX, text); free(text); }
        }
        return;
    }
    for (int v = 0; v < 18; ++v) if (task.variants & (1u << v)) {
        char *t = v ? nsstring_text(inject_pixel_extras(x->sh.source, x->sh.source_bytes, v >> 1, v & 1)) : xlat_text(x);
        fn_enqueue(cc, MS_STAGE_PIXEL, t); free(t);
    }
}
static void worker_prepare(mr_compiler *cc, uint32_t index) {
    const mr_recipe *r = &cc->recipes[index];
    mr_program_state state;
    pthread_mutex_lock(&cc->lock);
    int ok = !recipe_state_locked(cc, r, &state);
    pthread_mutex_unlock(&cc->lock);
    if (!ok) return;
    uint64_t key = r->kind == MR_RECIPE_PROGRAM ? program_key(&state, r->stride, r->overlay) : fixed_rhw_key(&state, r->overlay, r->clip_space);
    if (key != r->key) return;                  /* recorded by a build that keyed pipelines differently */
    mr_shared *s = cc->s; int queued = 0;
    pthread_mutex_lock(&cc->lock);
    if (r->kind == MR_RECIPE_PROGRAM) {
        if (!program_lookup(s, key) && s->program_count < MR_MAX_PROGRAMS * 3 / 4) {
            mr_program_cache *e = program_insert(s, key); if (e) { e->state = MR_PL_QUEUED; e->recipe = r; queued = 1; }
        }
    } else if (!fixed_rhw_lookup(s, key) && s->fixed_rhw_program_count < MR_MAX_FIXED_RHW_PROGRAMS * 3 / 4) {
        mr_fixed_rhw_cache *e = fixed_rhw_insert(s, key); if (e) { e->state = MR_PL_QUEUED; e->recipe = r; queued = 1; }
    }
    pthread_mutex_unlock(&cc->lock);
    if (!queued) return;
    mr_texts t; int failed = r->kind == MR_RECIPE_PROGRAM ? program_texts(cc, &state, 0, &t) : fixed_rhw_texts(cc, &state, r->clip_space, &t);
    if (!failed) { fn_enqueue(cc, MS_STAGE_VERTEX, t.vs_text); fn_enqueue(cc, MS_STAGE_PIXEL, t.fs_text); }
    texts_free(&t);
    pthread_mutex_lock(&cc->lock);
    push_u32(&cc->build, &cc->build_head, &cc->build_count, &cc->build_cap, index);
    pthread_cond_signal(&cc->work);
    pthread_mutex_unlock(&cc->lock);
}
static void (*mr_test_before_background_build)(uint64_t key);   /* tests: hold a worker inside one build */
static void worker_build(mr_compiler *cc, uint32_t index) {
    const mr_recipe *r = &cc->recipes[index]; mr_shared *s = cc->s;
    mr_program_state state; mr_program_cache *pe = NULL; mr_fixed_rhw_cache *fe = NULL;
    pthread_mutex_lock(&cc->lock);
    if (r->kind == MR_RECIPE_PROGRAM) pe = program_lookup(s, r->key); else fe = fixed_rhw_lookup(s, r->key);
    int claimed = ((pe && pe->state == MR_PL_QUEUED) || (fe && fe->state == MR_PL_QUEUED)) && !recipe_state_locked(cc, r, &state);
    if (claimed) { if (pe) pe->state = MR_PL_BUILDING; else fe->state = MR_PL_BUILDING; }
    pthread_mutex_unlock(&cc->lock);
    if (!claimed) return;
    if (mr_test_before_background_build) mr_test_before_background_build(r->key);
    uint64_t started = mr_now();
    mr_program_cache pbuilt = {0}; mr_fixed_rhw_cache fbuilt = {0};
    int failed = pe ? program_build(cc, &state, r->stride, r->overlay, 0, &pbuilt, NULL)
                    : fixed_rhw_build(cc, &state, r->overlay, r->clip_space, 0, &fbuilt, NULL);
    MR_ADD(mr_stat_bg_pipelines, 1); MR_ADD(mr_stat_bg_ns, mr_now() - started);
    if (mr_pipeline_trace()) NSLog(@"[pipelines] worker built %s %016llx in %.1f ms%s", pe ? "program" : "fixed", (unsigned long long)r->key, (mr_now() - started) / 1e6, failed ? " (failed)" : "");
    pthread_mutex_lock(&cc->lock);
    if (pe) {
        if (!failed) { pe->vs = pbuilt.vs; pe->ps = pbuilt.ps; pe->has_pixel_shader = pbuilt.has_pixel_shader; pe->extras = pbuilt.extras; pe->streams = pbuilt.streams; pe->pipeline = pbuilt.pipeline; pe->state = MR_PL_READY; }
        else pe->state = MR_PL_FAILED;          /* the engine thread rebuilds it at first draw */
        pe->background = 1;
    } else {
        if (!failed) { fe->ps = fbuilt.ps; fe->has_ps = fbuilt.has_ps; fe->extras = fbuilt.extras; fe->pipeline = fbuilt.pipeline; fe->state = MR_PL_READY; }
        else fe->state = MR_PL_FAILED;
        fe->background = 1;
    }
    pbuilt.pipeline = nil; fbuilt.pipeline = nil;
    pthread_cond_broadcast(&cc->changed);
    pthread_mutex_unlock(&cc->lock);
}
static void *compiler_worker(void *arg) {
    mr_compiler *cc = arg;
    pthread_setname_np("halo.pipelines");
    char err[1024]; mr_err_sink = err; mr_err_sink_size = sizeof err;
    pthread_mutex_lock(&cc->lock);
    for (;;) {
        if (cc->translate_count) {
            mr_translate_task task = cc->translate[cc->translate_head]; cc->translate_head++; cc->translate_count--;
            pthread_mutex_unlock(&cc->lock); @autoreleasepool { worker_translate(cc, task); } pthread_mutex_lock(&cc->lock); continue;
        }
        if (cc->prepare_count) {
            uint32_t index = cc->prepare[cc->prepare_head]; cc->prepare_head++; cc->prepare_count--;
            pthread_mutex_unlock(&cc->lock); @autoreleasepool { worker_prepare(cc, index); } pthread_mutex_lock(&cc->lock); continue;
        }
        if (cc->fn_queued) { fn_run_queued(cc); continue; }
        if (cc->build_count) {
            uint32_t index = cc->build[cc->build_head]; cc->build_head++; cc->build_count--;
            pthread_mutex_unlock(&cc->lock); @autoreleasepool { worker_build(cc, index); } pthread_mutex_lock(&cc->lock); continue;
        }
        cc->idle++; pthread_cond_broadcast(&cc->changed);
        pthread_cond_wait(&cc->work, &cc->lock);
        cc->idle--;
    }
    return NULL;
}
static mr_compiler *compiler_for(mr_shared *s) {
    if (s->compiler) return s->compiler;
    mr_compiler *cc = calloc(1, sizeof *cc); if (!cc) return NULL;
    pthread_mutex_init(&cc->lock, NULL); pthread_cond_init(&cc->changed, NULL); pthread_cond_init(&cc->work, NULL);
    cc->s = s; cc->manifest_fd = -1;
    /* Utility QoS: the compiles run beside the engine, not in front of it. */
    int workers = mr_env_int("HALO_PIPELINE_WORKERS", 3); cc->workers_wanted = workers < 0 ? 0 : workers > 6 ? 6 : workers;
    cc->prewarm = mr_env_int("HALO_SHADER_PREWARM", 1) != 0;
    cc->radial_fog_prewarm = halo_settings_radial_fog();
    s->compiler = cc; return cc;
}
/* Tests: block until every queue is empty and every worker is idle. */
static void compiler_drain(mr_compiler *cc) {
    pthread_mutex_lock(&cc->lock);
    while (cc->workers && (cc->translate_count || cc->prepare_count || cc->fn_queued || cc->build_count || cc->idle < cc->workers))
        pthread_cond_wait(&cc->changed, &cc->lock);
    pthread_mutex_unlock(&cc->lock);
}

void mr_shader_created(int pixel, const uint32_t *tokens, size_t bytes, uint64_t key) {
    if (!tokens || bytes < 8 || bytes > 1024u * 1024u || (bytes & 3)) return;
    mr_shared *s = shared_state(); if (!s) return;
    mr_compiler *cc = compiler_for(s); if (!cc || !cc->workers_wanted) return;
    int stage = pixel ? MS_STAGE_PIXEL : MS_STAGE_VERTEX;
    if (!key) key = hash_more(UINT64_C(1469598103934665603), tokens, bytes);
    pthread_mutex_lock(&cc->lock);
    if (registered_find(cc, stage, key)) { pthread_mutex_unlock(&cc->lock); return; }
    mr_registered *r = malloc(sizeof *r + bytes);
    if (!r) { pthread_mutex_unlock(&cc->lock); return; }
    r->key = key; r->stage = stage; r->bytes = bytes; memcpy(r->tokens, tokens, bytes);
    r->next = cc->registered[key & (MR_REG_BUCKETS - 1u)]; cc->registered[key & (MR_REG_BUCKETS - 1u)] = r;
    MR_ADD(mr_stat_shaders, 1);
    if (stage == MS_STAGE_PIXEL) {
        if (cc->pixel_shader_count == cc->pixel_shader_cap) {
            unsigned next = cc->pixel_shader_cap ? cc->pixel_shader_cap * 2 : 256;
            mr_registered **grown = realloc(cc->pixel_shaders, next * sizeof *grown);
            if (grown) { cc->pixel_shaders = grown; cc->pixel_shader_cap = next; }
        }
        if (cc->pixel_shader_count < cc->pixel_shader_cap) cc->pixel_shaders[cc->pixel_shader_count++] = r;
    }
    /* Translate and compile it now (plus the alpha-test/fog variants already
     * in use), and queue any recorded pipeline it completes. */
    uint32_t variants = cc->prewarm ? (stage == MS_STAGE_PIXEL ? (1u | cc->variants_seen) : 1u) : 0;
    if (cc->translate_head + cc->translate_count == cc->translate_cap) {
        if (cc->translate_head) { memmove(cc->translate, cc->translate + cc->translate_head, cc->translate_count * sizeof *cc->translate); cc->translate_head = 0; }
        else { unsigned next = cc->translate_cap ? cc->translate_cap * 2 : 256; mr_translate_task *grown = realloc(cc->translate, next * sizeof *grown); if (grown) { cc->translate = grown; cc->translate_cap = next; } }
    }
    if (cc->translate_head + cc->translate_count < cc->translate_cap)
        cc->translate[cc->translate_head + cc->translate_count++] = (mr_translate_task){ r, variants };
    compiler_start_workers_locked(cc); pthread_cond_signal(&cc->work);
    pthread_mutex_unlock(&cc->lock);
}

/* ---- persistence: manifest of recipes, binary archive of compiled pipelines ---- */
static void store_append(mr_compiler *cc, const mr_recipe *r) {
    if (!cc->store_open) return;
    pthread_mutex_lock(&cc->lock); uint32_t loaded = cc->recipe_count; pthread_mutex_unlock(&cc->lock);
    if (atomic_fetch_add_explicit(&cc->recipes_recorded, 1, memory_order_relaxed) + loaded >= MR_RECIPE_MAX) return;
    mr_recipe *copy = malloc(sizeof *copy); if (!copy) return;
    memcpy(copy, r, sizeof *copy);
    dispatch_async(cc->io, ^{
        if (cc->manifest_fd >= 0 && write(cc->manifest_fd, copy, sizeof *copy) != (ssize_t)sizeof *copy) NSLog(@"metalrenderer: pipeline manifest write failed");
        free(copy);
    });
}
static void archive_schedule_save(mr_compiler *cc);
/* io queue. `force`: write even while the workers are still prewarming. */
static void archive_save(mr_compiler *cc, int force) {
    cc->archive_save_pending = 0;
    if (!cc->collector || !cc->archive_unsaved) return;
    if (!force) {
        /* Replace last session's archive only with one holding at least what
         * the prewarm rebuilds from the manifest. */
        pthread_mutex_lock(&cc->lock); int busy = cc->prepare_count || cc->build_count; pthread_mutex_unlock(&cc->lock);
        if (busy) { archive_schedule_save(cc); return; }
    }
    @autoreleasepool {
    NSString *path = cc->archive_url.path, *tmp = [path stringByAppendingString:@".tmp"];
    NSError *error = nil;
    if ([cc->collector serializeToURL:[NSURL fileURLWithPath:tmp] error:&error] && rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation) == 0) cc->archive_unsaved = 0;
    else { unlink(tmp.fileSystemRepresentation); NSLog(@"metalrenderer: pipeline archive save failed: %@", error); }
    }
}
static void archive_schedule_save(mr_compiler *cc) {    /* io queue */
    if (cc->archive_save_pending) return;
    cc->archive_save_pending = 1;   /* one write per quiet 15 s, not one per pipeline */
    int64_t delay_ms = mr_env_int("HALO_PIPELINE_ARCHIVE_SAVE_MS", 15000);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay_ms * (int64_t)NSEC_PER_MSEC), cc->io, ^{ archive_save(cc, 0); });
}
static unsigned mr_archive_add_failures;       /* io queue only */
static void archive_add(mr_compiler *cc, MTLRenderPipelineDescriptor *pd) {
    if (!cc->io) return;
    MTLRenderPipelineDescriptor *copy = [pd copy]; copy.binaryArchives = nil;
    dispatch_async(cc->io, ^{
        @autoreleasepool {
        if (!cc->collector || cc->archive_total >= MR_RECIPE_MAX) return;
        NSError *error = nil;
        if (![cc->collector addRenderPipelineFunctionsWithDescriptor:copy error:&error]) {
            if (mr_archive_add_failures++ < 4) NSLog(@"metalrenderer: pipeline archive add failed: %@", error);
            return;
        }
        cc->archive_total++; cc->archive_unsaved++; MR_ADD(mr_stat_archive_stored, 1);
        archive_schedule_save(cc);
        }
    });
}
static void manifest_load(mr_compiler *cc) {    /* io queue */
    char path[1100]; snprintf(path, sizeof path, "%s/manifest-v%u.bin", cc->dir, MR_MANIFEST_VERSION);
    uint32_t header[4] = {0}; mr_recipe *all = NULL; size_t count = 0; int rewrite = 1;
    int fd = open(path, O_RDONLY), legacy = 0;
    /* Preserve the Build78 warmup recipes on the first v2 launch. Every v1
     * field has the same offset, and mode 0 retains the old pipeline keys.
     * Keep the original file intact so rolling the app back remains cheap. */
    if (fd < 0) {
        char old_path[1100]; snprintf(old_path, sizeof old_path, "%s/manifest-v1.bin", cc->dir);
        fd = open(old_path, O_RDONLY); legacy = fd >= 0;
    }
    size_t record_bytes = legacy ? MR_RECIPE_V1_BYTES : sizeof(mr_recipe);
    struct stat st;
    if (fd >= 0 && !fstat(fd, &st) && st.st_size >= (off_t)sizeof header &&
        read(fd, header, sizeof header) == (ssize_t)sizeof header &&
        header[0] == MR_MANIFEST_MAGIC && header[1] == (legacy ? 1u : MR_MANIFEST_VERSION) && header[2] == record_bytes) {
        size_t records = (size_t)(st.st_size - (off_t)sizeof header) / record_bytes;
        all = records ? calloc(records, sizeof *all) : NULL;
        if (all && legacy) {
            for (size_t i = 0; i < records; ++i) {
                if (read(fd, &all[i], record_bytes) != (ssize_t)record_bytes) break;
                count++;
            }
        } else if (all && read(fd, all, records * sizeof *all) == (ssize_t)(records * sizeof *all)) count = records;
        rewrite = legacy || (off_t)(sizeof header + records * record_bytes) != st.st_size;   /* torn tail */
    }
    if (fd >= 0) close(fd);
    /* Keep the newest record of each key, at most MR_RECIPE_MAX. */
    size_t slots = 16; while (slots < count * 2) slots <<= 1;
    uint64_t *seen = calloc(slots, sizeof *seen);
    mr_recipe *kept = count ? malloc(count * sizeof *kept) : NULL; size_t kept_count = 0;
    for (size_t i = count; seen && kept && i-- > 0 && kept_count < MR_RECIPE_MAX; ) {
        const mr_recipe *r = &all[i];
        if (!r->key || (r->kind != MR_RECIPE_PROGRAM && r->kind != MR_RECIPE_FIXED) || r->declaration_bytes > MR_RECIPE_DECL || r->radial_fog < 0 || r->radial_fog > HALO_RADIAL_FOG_VIEW_PLANE) { rewrite = 1; continue; }
        size_t h = slot_hash(r->key) & (slots - 1); int dup = 0;
        while (seen[h]) { if (seen[h] == r->key) { dup = 1; break; } h = (h + 1) & (slots - 1); }
        if (dup) { rewrite = 1; continue; }
        seen[h] = r->key; kept[kept_count++] = *r;
    }
    if (kept_count < count) rewrite = 1;
    for (size_t i = 0; i < kept_count / 2; ++i) { mr_recipe t = kept[i]; kept[i] = kept[kept_count - 1 - i]; kept[kept_count - 1 - i] = t; }
    free(seen); free(all);
    if (rewrite) {
        char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
        int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644); int ok = out >= 0;
        uint32_t fresh[4] = { MR_MANIFEST_MAGIC, MR_MANIFEST_VERSION, (uint32_t)sizeof(mr_recipe), 0 };
        if (ok) ok = write(out, fresh, sizeof fresh) == (ssize_t)sizeof fresh;
        if (ok && kept_count) ok = write(out, kept, kept_count * sizeof *kept) == (ssize_t)(kept_count * sizeof *kept);
        if (out >= 0) close(out);
        if (!ok || rename(tmp, path)) { unlink(tmp); NSLog(@"metalrenderer: pipeline manifest rewrite failed"); }
    }
    cc->manifest_fd = open(path, O_WRONLY | O_APPEND);
    uint8_t *queued = kept_count ? calloc(kept_count, 1) : NULL;
    pthread_mutex_lock(&cc->lock);
    if (kept_count && queued) {
        cc->recipes = kept; cc->recipe_queued = queued; cc->recipe_count = (uint32_t)kept_count;
        for (uint32_t i = 0; i < cc->recipe_count; ++i) {       /* the alpha-test/fog variants in use last time */
            const mr_recipe *r = &cc->recipes[i];
            if (r->radial_fog && !cc->radial_fog_prewarm) continue;
            if (r->has_ps) { int v = (r->alpha_enabled ? r->alpha_func : 0) * 2 + (r->fog_enable && (r->kind == MR_RECIPE_PROGRAM || r->radial_fog)); if (v > 0 && v < 32) cc->variants_seen |= 1u << v; }
        }
        recipes_scan_locked(cc, 0);
    } else { free(kept); free(queued); }
    pthread_mutex_unlock(&cc->lock);
    MR_ADD(mr_stat_recipes, kept_count);
}
static void archive_open(mr_compiler *cc) {     /* io queue */
    @autoreleasepool {
    id<MTLDevice> device = cc->s->dev;
    if (!mr_env_int("HALO_PIPELINE_ARCHIVE", 0) || ![device respondsToSelector:@selector(newBinaryArchiveWithDescriptor:error:)]) return;
    /* Compiled code is only valid for this GPU and this OS build. */
    NSString *identity = [NSString stringWithFormat:@"halo-pipelines-1|%@|%@", device.name, NSProcessInfo.processInfo.operatingSystemVersionString];
    uint64_t id_hash = hash_more(UINT64_C(1469598103934665603), identity.UTF8String, strlen(identity.UTF8String));
    NSString *dir = [NSString stringWithUTF8String:cc->dir];
    NSString *name = [NSString stringWithFormat:@"pipelines-%016llx.metalarchive", (unsigned long long)id_hash];
    NSString *path = [dir stringByAppendingPathComponent:name];
    for (NSString *other in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:dir error:nil])
        if ([other hasPrefix:@"pipelines-"] && ![other isEqualToString:name]) [[NSFileManager defaultManager] removeItemAtPath:[dir stringByAppendingPathComponent:other] error:nil];
    MTLBinaryArchiveDescriptor *descriptor = [[MTLBinaryArchiveDescriptor alloc] init];
    struct stat st;
    if (!stat(path.fileSystemRepresentation, &st)) {
        if (st.st_size > 0 && (uint64_t)st.st_size < MR_ARCHIVE_MAX_BYTES) descriptor.url = [NSURL fileURLWithPath:path];
        else unlink(path.fileSystemRepresentation);
    }
    NSError *error = nil;
    id<MTLBinaryArchive> archive = descriptor.url ? [device newBinaryArchiveWithDescriptor:descriptor error:&error] : nil;
    if (!archive && descriptor.url) { NSLog(@"metalrenderer: discarding unreadable pipeline archive: %@", error); unlink(path.fileSystemRepresentation); }
    error = nil;
    id<MTLBinaryArchive> collector = [device newBinaryArchiveWithDescriptor:[[MTLBinaryArchiveDescriptor alloc] init] error:&error];
    if (!collector) { NSLog(@"metalrenderer: no pipeline archive: %@", error); archive = nil; }
    cc->archive_url = [NSURL fileURLWithPath:path];
    pthread_mutex_lock(&cc->lock); cc->archive = archive; cc->collector = collector; pthread_mutex_unlock(&cc->lock);
    }
}
/* Runtime only (shared_state): persist pipelines in the Caches directory and
 * prewarm what earlier sessions used. HALO_PIPELINE_CACHE=0 disables it;
 * HALO_PIPELINE_CACHE_DIR moves it. */
static void mr_pipeline_store_open(mr_compiler *cc) {
    @autoreleasepool {
    if (cc->store_open || !mr_env_int("HALO_PIPELINE_CACHE", 1)) return;
    const char *override = getenv("HALO_PIPELINE_CACHE_DIR");
    NSString *dir = (override && override[0]) ? [NSString stringWithUTF8String:override]
        : [NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:@"HaloVision/pipelines"];
    if (!dir || ![[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) return;
    snprintf(cc->dir, sizeof cc->dir, "%s", dir.fileSystemRepresentation);
    cc->io = dispatch_queue_create("halo.pipeline-store", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    cc->store_open = 1;
    dispatch_async(cc->io, ^{ archive_open(cc); manifest_load(cc); });
    }
}
/* Write what is pending now (tests; an app about to be suspended). */
void mr_pipeline_cache_flush(void) {
    mr_shared *s = g_shared; mr_compiler *cc = s ? s->compiler : NULL;
    if (!cc || !cc->io) return;
    dispatch_sync(cc->io, ^{ archive_save(cc, 1); if (cc->manifest_fd >= 0) fsync(cc->manifest_fd); });
}

/* ---- the draw-time lookups ---- */
static void record_program(mr_compiler *cc, int kind, const mr_program_state *state, size_t stride, int overlay, int clip_space, uint64_t key) {
    if (!cc->store_open || !recipe_recordable(kind, state)) return;
    mr_recipe r; recipe_from_state(&r, kind, state, stride, overlay, clip_space, key);
    store_append(cc, &r);
}
static mr_program_cache *program_for(mr_context *c, const mr_program_state *state, size_t stride) {
    @autoreleasepool {
    mr_shared *s = c->s; mr_compiler *cc = compiler_for(s);
    if (!cc) { g_err = "pipeline compiler allocation failed"; return NULL; }
    uint64_t key = program_key(state, stride, c->overlay_target), waited = 0;
    pthread_mutex_lock(&cc->lock);
    mr_program_cache *e = program_lookup(s, key);
    for (;;) {
        if (!e) {
            if (!(e = program_insert(s, key))) { pthread_mutex_unlock(&cc->lock); g_err = "programmable pipeline cache is full"; return NULL; }
            e->state = MR_PL_BUILDING; break;
        }
        if (e->state == MR_PL_READY) {
            if (e->background) { e->background = 0; MR_ADD(mr_stat_prewarm_hits, 1); }
            pthread_mutex_unlock(&cc->lock);
            if (waited) { MR_ADD(mr_stat_waits, 1); MR_ADD(mr_stat_wait_ns, waited); }
            return e;
        }
        if (e->state == MR_PL_QUEUED || (e->state == MR_PL_FAILED && (e->background || (e->attempts < 3 && mr_now() - e->failed_at > MR_RETRY_NS)))) { e->state = MR_PL_BUILDING; break; }
        if (e->state == MR_PL_FAILED) { set_errorf("%s", e->error ? e->error : "pipeline build failed"); pthread_mutex_unlock(&cc->lock); return NULL; }
        /* A worker is building this pipeline: wait for it, and only it. */
        uint64_t t = mr_now(); pthread_cond_wait(&cc->changed, &cc->lock); waited += mr_now() - t;
    }
    pthread_mutex_unlock(&cc->lock);
    if (waited) { MR_ADD(mr_stat_waits, 1); MR_ADD(mr_stat_wait_ns, waited); }
    uint64_t started = mr_now(); mr_tl_wait_ns = 0;
    mr_program_cache built = {0}; int variant = 0;
    int failed = program_build(cc, state, stride, c->overlay_target, 1, &built, &variant);
    uint64_t spent = mr_now() - started - mr_tl_wait_ns;
    pthread_mutex_lock(&cc->lock);
    if (!failed) {
        e->vs = built.vs; e->ps = built.ps; e->has_pixel_shader = built.has_pixel_shader; e->extras = built.extras;
        e->streams = built.streams; e->pipeline = built.pipeline; e->state = MR_PL_READY; e->background = 0;
        free(e->error); e->error = NULL;
    } else { e->state = MR_PL_FAILED; e->background = 0; free(e->error); e->error = strdup(mr_last_error()); e->failed_at = mr_now(); e->attempts++; }
    const mr_recipe *from = e->recipe;
    pthread_cond_broadcast(&cc->changed);
    pthread_mutex_unlock(&cc->lock);
    built.pipeline = nil;
    if (mr_pipeline_trace()) NSLog(@"[pipelines] engine built program %016llx in %.1f ms (+%.1f ms waiting)%s", (unsigned long long)key, spent / 1e6, (waited + mr_tl_wait_ns) / 1e6, failed ? " (failed)" : "");
    if (failed) return NULL;
    MR_ADD(mr_stat_engine_builds, 1); MR_ADD(mr_stat_engine_build_ns, spent);
    if (!from) record_program(cc, MR_RECIPE_PROGRAM, state, stride, c->overlay_target, 0, key);
    note_variant(cc, variant);
    return e;
    }
}
static mr_fixed_rhw_cache *fixed_rhw_program_for(mr_context *c, const mr_program_state *state, int clip_space) {
    @autoreleasepool {
    mr_shared *s = c->s; mr_compiler *cc = compiler_for(s);
    if (!cc) { g_err = "pipeline compiler allocation failed"; return NULL; }
    uint64_t key = fixed_rhw_key(state, c->overlay_target, clip_space), waited = 0;
    pthread_mutex_lock(&cc->lock);
    mr_fixed_rhw_cache *e = fixed_rhw_lookup(s, key);
    for (;;) {
        if (!e) {
            if (!(e = fixed_rhw_insert(s, key))) { pthread_mutex_unlock(&cc->lock); g_err = "fixed RHW pipeline cache is full"; return NULL; }
            e->state = MR_PL_BUILDING; break;
        }
        if (e->state == MR_PL_READY) {
            if (e->background) { e->background = 0; MR_ADD(mr_stat_prewarm_hits, 1); }
            pthread_mutex_unlock(&cc->lock);
            if (waited) { MR_ADD(mr_stat_waits, 1); MR_ADD(mr_stat_wait_ns, waited); }
            return e;
        }
        if (e->state == MR_PL_QUEUED || (e->state == MR_PL_FAILED && (e->background || (e->attempts < 3 && mr_now() - e->failed_at > MR_RETRY_NS)))) { e->state = MR_PL_BUILDING; break; }
        if (e->state == MR_PL_FAILED) { set_errorf("%s", e->error ? e->error : "pipeline build failed"); pthread_mutex_unlock(&cc->lock); return NULL; }
        uint64_t t = mr_now(); pthread_cond_wait(&cc->changed, &cc->lock); waited += mr_now() - t;
    }
    pthread_mutex_unlock(&cc->lock);
    if (waited) { MR_ADD(mr_stat_waits, 1); MR_ADD(mr_stat_wait_ns, waited); }
    uint64_t started = mr_now(); mr_tl_wait_ns = 0;
    mr_fixed_rhw_cache built = {0}; int variant = 0;
    int failed = fixed_rhw_build(cc, state, c->overlay_target, clip_space, 1, &built, &variant);
    uint64_t spent = mr_now() - started - mr_tl_wait_ns;
    pthread_mutex_lock(&cc->lock);
    if (!failed) {
        e->ps = built.ps; e->has_ps = built.has_ps; e->extras = built.extras; e->pipeline = built.pipeline;
        e->state = MR_PL_READY; e->background = 0; free(e->error); e->error = NULL;
    } else { e->state = MR_PL_FAILED; e->background = 0; free(e->error); e->error = strdup(mr_last_error()); e->failed_at = mr_now(); e->attempts++; }
    const mr_recipe *from = e->recipe;
    pthread_cond_broadcast(&cc->changed);
    pthread_mutex_unlock(&cc->lock);
    built.pipeline = nil;
    if (mr_pipeline_trace()) NSLog(@"[pipelines] engine built fixed %016llx in %.1f ms (+%.1f ms waiting)%s", (unsigned long long)key, spent / 1e6, (waited + mr_tl_wait_ns) / 1e6, failed ? " (failed)" : "");
    if (failed) return NULL;
    MR_ADD(mr_stat_engine_builds, 1); MR_ADD(mr_stat_engine_build_ns, spent);
    if (!from) record_program(cc, MR_RECIPE_FIXED, state, 0, c->overlay_target, clip_space, key);
    note_variant(cc, variant);
    return e;
    }
}
/* Pretransformed vertex functions shared by fixed pipelines. */
static void compiler_prewarm_builtin(mr_compiler *cc) {
    if (!cc->workers_wanted) return;
    @autoreleasepool {
        for (int clip = 0; clip < 2; ++clip) for (int fog = 0; fog <= cc->radial_fog_prewarm; ++fog) {
            char *t = nsstring_text(fixed_rhw_vertex_source(clip, fog)); fn_enqueue(cc, MS_STAGE_VERTEX, t); free(t);
        }
    }
}

static int mr_draw_fixed_common(mr_context *c, const mr_program_state *state, int prim,
                                const void *vertices, size_t stride, uint32_t nv,
                                const uint16_t *indices, uint32_t ni, int clip_space) {
    @autoreleasepool {
    if (!c || !state || !vertices || !nv || stride < sizeof(mr_vertex_fixed_rhw)) { g_err = "bad fixed draw arguments"; return MR_ERR_ARGS; }
    if (state->blend < 0 || state->blend >= MR_BLEND_COUNT || (state->color_write_mask & ~15) || state->cull_mode < 1 || state->cull_mode > 3) { g_err = "unsupported fixed RHW render state"; return MR_ERR_STATE; }
    if (nv > MR_MAX_VERTS || ni > MR_MAX_INDICES || (size_t)nv > SIZE_MAX / sizeof(FixedRHWIn)) { g_err = "fixed RHW draw exceeds bounds"; return MR_ERR_BOUNDS; }
    if (!mr_primitive_supported(prim)) { g_err = "unsupported primitive type"; return MR_ERR_PRIMITIVE; }
    uint32_t count = indices ? ni : nv; if (count < 3) return MR_OK;
    if (indices && !indices_below(indices, ni, nv)) { g_err = "index out of range"; return MR_ERR_BOUNDS; }
    id<MTLDepthStencilState> depth = depth_stencil_state_for(c,state->depth_enable,state->depth_compare,state->depth_write,state->stencil_enable,
        state->stencil_fail,state->stencil_depth_fail,state->stencil_pass,state->stencil_compare,state->stencil_read_mask,state->stencil_write_mask);
    if (!depth) return MR_ERR_STATE;
    mr_fixed_rhw_cache *program = fixed_rhw_program_for(c, state, clip_space); if (!program) return MR_ERR_SHADER;
    float ps_uniforms[256][4];
    id<MTLSamplerState> stage_samplers[16] = {nil};
    if (program->has_ps) {
        if (program->ps->packed_float4_count && (!state->ps_float4 || pack_float_uniforms(program->ps, state->ps_float4, state->ps_float4_count, ps_uniforms))) return MR_ERR_STATE;
        for (int i = 0; i < program->ps->sampler_count; ++i) {
            int stage = program->ps->samplers[i].index;
            if (stage < 0 || stage >= 16) { set_errorf("pixel shader sampler %d is out of range", stage); return MR_ERR_TEXTURE; }
            if (!(stage_samplers[stage] = sampler_for(c, &state->samplers[stage]))) return MR_ERR_UNSUPPORTED;
        }
    } else {
        for (int stage = 0; stage < 8 && state->fixed_stages[stage].color_op != 1; ++stage) if (fixed_stage_uses_texture(&state->fixed_stages[stage])) {
            uint32_t texture = state->samplers[stage].texture;
            int available = texture && texture < MR_MAX_TEX && c->s->textures[texture];
            if (!available && !mr_tex_fallback()) { set_errorf("fixed RHW stage %d texture is unavailable", stage); return MR_ERR_TEXTURE; }
            if (!(stage_samplers[stage] = sampler_for(c, &state->samplers[stage]))) return MR_ERR_UNSUPPORTED;
        }
    }
    size_t vertex_offset = 0, index_offset = 0;
    int fog = fixed_rhw_fog_enabled(state);
    /* Preserve the original 112-byte GPU vertex stride when fog is disabled. */
    size_t output_stride = fog ? sizeof(FixedRHWIn) : offsetof(FixedRHWIn, fog);
    id<MTLBuffer> vertex_buffer = arena_alloc(c->s, (size_t)nv * output_stride, &vertex_offset);
    id<MTLBuffer> index_buffer = indices ? arena_copy(c->s, indices, (size_t)ni * 2, &index_offset) : nil;
    if (!vertex_buffer || (indices && !index_buffer)) { g_err = "fixed RHW draw buffer allocation failed"; return MR_ERR_DEVICE; }
    uint8_t *output = (uint8_t *)vertex_buffer.contents + vertex_offset; const uint8_t *input = vertices;
    for (uint32_t i = 0; i < nv; ++i) {
        FixedRHWIn *out = (FixedRHWIn *)(output + (size_t)i * output_stride);
        mr_vertex_fixed_rhw v; memcpy(&v, input + (size_t)i * stride, sizeof v);
        out->pos[0]=v.x; out->pos[1]=v.y; out->pos[2]=v.z; out->pos[3]=v.rhw;
        out->color[0]=((v.color>>16)&255)/255.f; out->color[1]=((v.color>>8)&255)/255.f;
        out->color[2]=(v.color&255)/255.f; out->color[3]=((v.color>>24)&255)/255.f;
        out->color1[0]=((v.specular>>16)&255)/255.f; out->color1[1]=((v.specular>>8)&255)/255.f;
        out->color1[2]=(v.specular&255)/255.f; out->color1[3]=((v.specular>>24)&255)/255.f;
        memcpy(out->uv, v.uv, sizeof v.uv);
        if (fog) {
            out->fog[0] = v.fog;
            out->fog[1] = out->fog[2] = out->fog[3] = 0.f;
        }
    }
    if (c->vw <= 0 || c->vh <= 0) return MR_OK;
    id<MTLRenderCommandEncoder> enc = begin_draw_encoder(c); if (!enc) return MR_ERR_DEVICE;
    bind_draw_state(c->s, enc, program->pipeline, depth, state->stencil_enable, state->stencil_ref,
        clip_space ? (MTLViewport){ (double)c->vx, (double)c->vy, (double)c->vw, (double)c->vh, 0.0, 1.0 }
                   : (MTLViewport){ 0, 0, (double)c->w, (double)c->h, 0.0, 1.0 },
        (MTLScissorRect){ (NSUInteger)c->vx, (NSUInteger)c->vy, (NSUInteger)c->vw, (NSUInteger)c->vh },
        state->cull_mode == 1, state->cull_mode == 3);
    [enc setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:0]; float viewport_size[2] = { (float)c->w, (float)c->h };
    [enc setVertexBytes:viewport_size length:sizeof viewport_size atIndex:1];
    if (program->has_ps) {
        if (program->ps->packed_float4_count) [enc setFragmentBytes:ps_uniforms length:(NSUInteger)program->ps->packed_float4_count * 16 atIndex:0];
        if (program->extras) { float extras[8]; pixel_extras(state, extras); [enc setFragmentBytes:extras length:sizeof extras atIndex:1]; }
        for (int i = 0; i < program->ps->sampler_count; ++i) { int stage = program->ps->samplers[i].index;
            bind_fragment_texture(c->s, enc, ps_sampler_texture(c, state, stage, program->ps->samplers[i].type), stage_samplers[stage], stage); }
    } else {
        float fixed[12]; fixed_uniforms(state, fixed);
        [enc setFragmentBytes:fixed length:sizeof fixed atIndex:1];
        for (int stage = 0; stage < 8 && state->fixed_stages[stage].color_op != 1; ++stage) if (fixed_stage_uses_texture(&state->fixed_stages[stage])) {
            uint32_t texture = state->samplers[stage].texture;
            id<MTLTexture> t = (texture && texture < MR_MAX_TEX && c->s->textures[texture]) ? c->s->textures[texture] : c->s->white;
            bind_fragment_texture(c->s, enc, t, stage_samplers[stage], stage);
        }
    }
    MTLPrimitiveType type = mr_primitive_metal(prim);
    if (indices) [enc drawIndexedPrimitives:type indexCount:ni indexType:MTLIndexTypeUInt16 indexBuffer:index_buffer indexBufferOffset:index_offset];
    else [enc drawPrimitives:type vertexStart:0 vertexCount:nv];
    c->tris += mr_primitive_shapes(prim, count); return MR_OK;
    }
}

int mr_draw_fixed_rhw(mr_context *c, const mr_program_state *state, int prim,
                      const void *vertices, size_t stride, uint32_t nv,
                      const uint16_t *indices, uint32_t ni) {
    return mr_draw_fixed_common(c,state,prim,vertices,stride,nv,indices,ni,0);
}

int mr_draw_program_rhw(mr_context *c, const mr_program_state *state, int prim,
                        const void *vertices, size_t stride, uint32_t nv,
                        const uint16_t *indices, uint32_t ni) {
    if (!state || !state->pixel_tokens) { g_err = "pretransformed shader draw needs pixel shader tokens"; return MR_ERR_ARGS; }
    return mr_draw_fixed_common(c,state,prim,vertices,stride,nv,indices,ni,0);
}

int mr_draw_fixed_clip(mr_context *c, const mr_program_state *state, int prim,
                       const void *vertices, size_t stride, uint32_t nv,
                       const uint16_t *indices, uint32_t ni) {
    return mr_draw_fixed_common(c,state,prim,vertices,stride,nv,indices,ni,1);
}

/* D3D alpha testing with a translated pixel shader: add a reference buffer to
 * the entry signature and discard before the final return. MojoShader's Metal
 * profile closes the parameter list with ") {" on its own line and ends the
 * body with "return output;". */
/* Fixed-function stages D3D applies after a pixel shader: vertex fog (oFog
 * factor from the vertex shader, blended toward the fog color) and alpha test.
 * `func` is D3DCMP_* or 0 for no alpha test; `fog` adds the [[user(fog)]] input. */
static NSString *inject_pixel_extras(const char *source, size_t bytes, int func, int fog) {
    NSString *src = [[NSString alloc] initWithBytes:source length:bytes encoding:NSUTF8StringEncoding];
    if (!src) { set_errorf("%s", "pixel shader source is not UTF-8"); return nil; }
    NSRange entry = [src rangeOfString:@"fragment "]; if (entry.location == NSNotFound) { set_errorf("%s", "pixel extras: no fragment entry"); return nil; }
    NSRange close = [src rangeOfString:@"\n) {" options:0 range:NSMakeRange(entry.location, src.length - entry.location)];
    if (close.location == NSNotFound) { set_errorf("%s", "pixel extras: unexpected fragment signature"); return nil; }
    NSRange ret = [src rangeOfString:@"return output;" options:NSBackwardsSearch];
    if (ret.location == NSNotFound || ret.location < close.location) { set_errorf("%s", "pixel extras: no final return"); return nil; }
    NSRange input_struct = [src rangeOfString:@"_Input\n{\n"];
    int add_input = fog && (input_struct.location == NSNotFound || input_struct.location > entry.location);
    NSRange open = [src rangeOfString:@"(" options:0 range:NSMakeRange(entry.location, close.location - entry.location)];
    if (open.location == NSNotFound) { set_errorf("%s", "pixel extras: no parameter list"); return nil; }
    NSString *parameters = [[src substringWithRange:NSMakeRange(open.location + 1, close.location - open.location - 1)]
                            stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    const char *cmp = NULL;
    switch (func) {
        case 0: break;
        case 1: cmp = "false"; break; case 2: cmp = "a < r"; break; case 3: cmp = "a == r"; break; case 4: cmp = "a <= r"; break;
        case 5: cmp = "a > r"; break; case 6: cmp = "a != r"; break; case 7: cmp = "a >= r"; break; case 8: cmp = "true"; break;
        default: set_errorf("%s", "invalid alpha comparison function"); return nil;
    }
    NSMutableString *tail = [NSMutableString string];
    if (fog) [tail appendString:@"output.oC0.rgb = mix(mr_extra.fog_color.rgb, output.oC0.rgb, saturate(input.mr_fog.x)); "];
    if (cmp) [tail appendFormat:@"{ float a = output.oC0.a * 255.0; float r = mr_extra.alpha_ref; if (!(%s)) discard_fragment(); } ", cmp];
    NSMutableString *out = [NSMutableString stringWithString:src];
    [out replaceCharactersInRange:ret withString:[NSString stringWithFormat:@"%@\n\treturn output;", tail]];
    NSString *extra_input = add_input ? @"MRExtraInput input [[stage_in]],\n\t" : @"";
    [out replaceCharactersInRange:close withString:[NSString stringWithFormat:@"%@\n\t%@constant MRPixelExtras &mr_extra [[buffer(1)]]\n) {",
                                                   parameters.length || !fog ? @"," : @"", extra_input]];
    NSMutableString *declarations = [NSMutableString stringWithString:@"struct MRPixelExtras { float alpha_ref; float fog_enable; float2 pad; float4 fog_color; };\n"];
    if (add_input) [declarations appendString:@"struct MRExtraInput { float4 mr_fog [[user(fog)]]; };\n"];
    /* Apply edits from the end backwards: adding an input member first moves
     * the entry offset and can splice MRPixelExtras into the output struct. */
    [out insertString:declarations atIndex:entry.location];
    if (fog && !add_input) [out insertString:@"\tfloat4 mr_fog [[user(fog)]];\n" atIndex:input_struct.location + input_struct.length];
    return out;
}

static id<MTLDepthStencilState> depth_stencil_state_for(mr_context *c,
    int depth_enable,int depth_compare,int depth_write,int stencil_enable,
    int stencil_fail,int stencil_depth_fail,int stencil_pass,int stencil_compare,
    uint32_t stencil_read_mask,uint32_t stencil_write_mask) {
    @autoreleasepool {
    if(!depth_enable){depth_compare=8;depth_write=0;}if(depth_compare<1||depth_compare>8){g_err="invalid D3D depth comparison";return nil;}
    if(stencil_enable&&((stencil_fail<1||stencil_fail>8)||(stencil_depth_fail<1||stencil_depth_fail>8)||(stencil_pass<1||stencil_pass>8)||(stencil_compare<1||stencil_compare>8))){g_err="invalid D3D stencil state";return nil;}
    for(uint32_t i=0;i<c->s->depth_stencil_count;i++){mr_depth_stencil_cache *v=&c->s->depth_stencil_states[i];
        if(v->depth_enable==!!depth_enable&&v->depth_compare==depth_compare&&v->depth_write==!!depth_write&&v->stencil_enable==!!stencil_enable&&
           v->stencil_fail==stencil_fail&&v->stencil_depth_fail==stencil_depth_fail&&v->stencil_pass==stencil_pass&&v->stencil_compare==stencil_compare&&
           v->stencil_read_mask==(stencil_read_mask&255u)&&v->stencil_write_mask==(stencil_write_mask&255u))return v->state;}
    if(c->s->depth_stencil_count==64){g_err="depth/stencil state cache is full";return nil;}
    const MTLCompareFunction functions[9]={MTLCompareFunctionAlways,MTLCompareFunctionNever,MTLCompareFunctionLess,MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,MTLCompareFunctionGreater,MTLCompareFunctionNotEqual,MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways};
    const MTLStencilOperation operations[9]={MTLStencilOperationKeep,MTLStencilOperationKeep,MTLStencilOperationZero,MTLStencilOperationReplace,MTLStencilOperationIncrementClamp,MTLStencilOperationDecrementClamp,MTLStencilOperationInvert,MTLStencilOperationIncrementWrap,MTLStencilOperationDecrementWrap};
    MTLDepthStencilDescriptor *descriptor=[[MTLDepthStencilDescriptor alloc]init];descriptor.depthCompareFunction=functions[depth_compare];descriptor.depthWriteEnabled=!!depth_write;
    if(stencil_enable){MTLStencilDescriptor *stencil=[[MTLStencilDescriptor alloc]init];stencil.stencilCompareFunction=functions[stencil_compare];stencil.stencilFailureOperation=operations[stencil_fail];stencil.depthFailureOperation=operations[stencil_depth_fail];stencil.depthStencilPassOperation=operations[stencil_pass];stencil.readMask=stencil_read_mask&255u;stencil.writeMask=stencil_write_mask&255u;descriptor.frontFaceStencil=stencil;descriptor.backFaceStencil=stencil;}
    mr_depth_stencil_cache *slot=&c->s->depth_stencil_states[c->s->depth_stencil_count++];slot->depth_enable=!!depth_enable;slot->depth_compare=depth_compare;slot->depth_write=!!depth_write;
    slot->stencil_enable=!!stencil_enable;slot->stencil_fail=stencil_fail;slot->stencil_depth_fail=stencil_depth_fail;slot->stencil_pass=stencil_pass;slot->stencil_compare=stencil_compare;
    slot->stencil_read_mask=stencil_read_mask&255u;slot->stencil_write_mask=stencil_write_mask&255u;slot->state=[c->s->dev newDepthStencilStateWithDescriptor:descriptor];
    if(!slot->state)g_err="depth/stencil state allocation failed";return slot->state;
    }
}

/* Texture bound for a translated pixel shader sampler. Unbound stages and
 * stages whose bound texture kind differs from the shader's declaration get
 * opaque black of the declared kind, as the D3D9 runtime does, instead of
 * rejecting the whole draw. */
static id<MTLTexture> ps_sampler_texture(mr_context *c, const mr_program_state *state, int stage, int declared) {
    mr_shared *s = c->s;
    uint32_t id = (stage >= 0 && stage < 16) ? state->samplers[stage].texture : 0;
    if (id && id < MR_MAX_TEX && s->textures[id] && state->samplers[stage].type == declared) return s->textures[id];
    static unsigned reports; if (reports++ < 8) set_errorf("sampler %d: %s texture of type %d, using black", stage, id ? "mismatched" : "unbound", declared);
    return declared == MR_SAMPLER_CUBE ? s->blackcube : declared == MR_SAMPLER_VOLUME ? s->black3d : s->black2d;
}
static int pack_float_uniforms(const ms_shader *shader, const float *registers, uint32_t register_count,
                               float packed[256][4]) {
    if (shader->packed_int4_count || shader->packed_bool_count) { g_err = "integer/bool shader constants are unsupported"; return -1; }
    unsigned cursor = 0;
    for (int i = 0; i < shader->uniform_count; ++i) {
        const ms_binding *u = &shader->uniforms[i];
        if (u->type != MS_UNIFORM_FLOAT4 || u->index < 0 || u->count < 1 || (uint32_t)(u->index + u->count) > register_count || cursor + (unsigned)u->count > 256) {
            g_err = "shader constant register file is incomplete"; return -1;
        }
        memcpy(packed[cursor], registers + (size_t)u->index * 4, (size_t)u->count * 4 * sizeof(float)); cursor += (unsigned)u->count;
    }
    return cursor == (unsigned)shader->packed_float4_count ? 0 : -1;
}

static id<MTLSamplerState> sampler_for(mr_context *c, const mr_program_sampler *sampler) {
    @autoreleasepool {
    if (sampler->type != MR_SAMPLER_2D && sampler->type != MR_SAMPLER_CUBE && sampler->type != MR_SAMPLER_VOLUME) { g_err = "unknown sampler type"; return nil; }
    int au = sampler->address_u ? sampler->address_u : 1, av = sampler->address_v ? sampler->address_v : 1;
    if (au != av || (au != 1 && au != 3 && au != 4)) { g_err = "only matching wrap/clamp/border U/V addressing is supported"; return nil; }
    if (au == 4 && sampler->border_color != 0) { g_err = "nonzero texture border color is unsupported"; return nil; }
    uint8_t min_filter = sampler->min_filter ? sampler->min_filter : (sampler->linear_filter ? 2 : 1);
    uint8_t mag_filter = sampler->mag_filter ? sampler->mag_filter : (sampler->linear_filter ? 2 : 1);
    uint8_t mip_filter = sampler->mip_filter;
    if ((min_filter < 1 || min_filter > 3) || (mag_filter < 1 || mag_filter > 3) || mip_filter > 2) {
        g_err = "unsupported texture filtering mode"; return nil;
    }
    uint8_t max_anisotropy = sampler->max_anisotropy ? sampler->max_anisotropy : 1;
    if (max_anisotropy > 16) max_anisotropy = 16;
    if (min_filter != 3 && mag_filter != 3) max_anisotropy = 1;
    /* Quality override: the original hardware database only enables anisotropic
     * filtering for a few 2003-era cards, so the engine asks for bilinear with
     * point mips almost everywhere. Every mipmapped, linearly filtered sampler
     * becomes 8x anisotropic trilinear unless HALO_NO_ANISO is set (16x at
     * 2560x1920 was part of what made Build30 GPU-bound on the headset). */
    if (!mr_no_aniso() && mip_filter != 0 && min_filter != 1) { max_anisotropy = 8; mip_filter = 2; }
    uint8_t address = (uint8_t)au;
    for (uint32_t i = 0; i < c->s->program_sampler_count; ++i) {
        mr_sampler_cache *entry = &c->s->program_samplers[i];
        if (entry->address == address && entry->min_filter == min_filter && entry->mag_filter == mag_filter &&
            entry->mip_filter == mip_filter && entry->max_anisotropy == max_anisotropy &&
            entry->max_mip_level == sampler->max_mip_level) return entry->state;
    }
    if (c->s->program_sampler_count >= MR_MAX_PROGRAM_SAMPLERS) { g_err = "sampler cache full"; return nil; }
    MTLSamplerDescriptor *descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.minFilter = min_filter == 1 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = mag_filter == 1 ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    descriptor.mipFilter = mip_filter == 0 ? MTLSamplerMipFilterNotMipmapped :
                           (mip_filter == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear);
    descriptor.sAddressMode = descriptor.tAddressMode = descriptor.rAddressMode =
        address == 4 ? MTLSamplerAddressModeClampToZero :
        (address == 3 ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat);
    descriptor.maxAnisotropy = max_anisotropy;
    descriptor.lodMinClamp = sampler->max_mip_level;
    id<MTLSamplerState> state = [c->s->dev newSamplerStateWithDescriptor:descriptor];
    if (!state) { g_err = "sampler allocation failed"; return nil; }
    mr_sampler_cache *entry = &c->s->program_samplers[c->s->program_sampler_count++];
    entry->address = address; entry->min_filter = min_filter; entry->mag_filter = mag_filter;
    entry->mip_filter = mip_filter; entry->max_anisotropy = max_anisotropy;
    entry->max_mip_level = sampler->max_mip_level; entry->state = state;
    return state;
    }
}

/* Diagnostic-only: when HALO_TEX_FALLBACK is set, a fixed-function stage whose
 * texture has not been created yet (e.g. a level lightmap the texture-upload
 * path does not build) is drawn with the 1x1 white texture instead of dropping
 * the whole draw. White on a lightmap stage reads as fully lit, so gameplay
 * geometry becomes visible (unlit) rather than black. Default behaviour (env
 * unset) is unchanged: the draw still fails closed. */
static int mr_no_aniso(void) { static int v = -1; if (v < 0) { const char *e = getenv("HALO_NO_ANISO"); v = (e && e[0] && e[0] != '0') ? 1 : 0; } return v; }
static int mr_tex_fallback(void) { static int v = -1; if (v < 0) { const char *e = getenv("HALO_TEX_FALLBACK"); v = (e && e[0] && e[0] != '0') ? 1 : 0; } return v; }

int mr_draw_program(mr_context *c, const mr_program_state *state, int prim,
                    const void *vertices, size_t stride, uint32_t nv,
                    const uint16_t *indices, uint32_t ni) {
    @autoreleasepool {
    if (!c || !state || !state->vertex_tokens || !state->declaration || !vertices || !nv || stride == 0 || stride > MR_MAX_VERTEX_STRIDE) { g_err = "bad programmable draw arguments"; return MR_ERR_ARGS; }
    if (state->blend < 0 || state->blend >= MR_BLEND_COUNT || (state->color_write_mask & ~15) || state->cull_mode < 1 || state->cull_mode > 3) { g_err = "unsupported programmable render state"; return MR_ERR_STATE; }
    if (nv > MR_MAX_VERTS || ni > MR_MAX_INDICES || (size_t)nv > SIZE_MAX / stride) { g_err = "programmable draw exceeds bounds"; return MR_ERR_BOUNDS; }
    if (!mr_primitive_supported(prim)) { g_err = "unsupported primitive type"; return MR_ERR_PRIMITIVE; }
    uint32_t count = indices ? ni : nv; if (count < 3) return MR_OK;
    if (indices && !indices_below(indices, ni, nv)) { g_err = "index out of range"; return MR_ERR_BOUNDS; }
    id<MTLDepthStencilState> depth = depth_stencil_state_for(c,state->depth_enable,state->depth_compare,state->depth_write,state->stencil_enable,
        state->stencil_fail,state->stencil_depth_fail,state->stencil_pass,state->stencil_compare,state->stencil_read_mask,state->stencil_write_mask);
    if (!depth) return MR_ERR_STATE;
    mr_program_cache *program = program_for(c, state, stride); if (!program) return MR_ERR_SHADER;
    size_t vertex_bytes = (size_t)nv * stride, vertex_offset = 0, index_offset = 0, stream_offsets[16] = {0};
    /* A static vertex buffer's resident copy holds these very bytes; bind it
     * where they start instead of copying them for every draw and bearing. */
    id<MTLBuffer> vertex_buffer = resident_range(state->resident[0], state->resident_offset[0], vertex_bytes);
    if (vertex_buffer) { vertex_offset = state->resident_offset[0]; atomic_fetch_add_explicit(&g_resident_bytes, vertex_bytes, memory_order_relaxed); }
    else { vertex_buffer = arena_copy(c->s, vertices, vertex_bytes, &vertex_offset); atomic_fetch_add_explicit(&g_arena_vertex_bytes, vertex_bytes, memory_order_relaxed); }
    id<MTLBuffer> index_buffer = indices ? arena_copy(c->s, indices, (size_t)ni * 2, &index_offset) : nil;
    if (!vertex_buffer || (indices && !index_buffer)) { g_err = "programmable draw buffer allocation failed"; return MR_ERR_DEVICE; }
    id<MTLBuffer> stream_buffers[16] = {nil};
    for (int s = 1; s < 16; ++s) if (program->streams & (1u << s)) {
        size_t need = (size_t)nv * state->stream_stride[s];
        if (!state->stream_bytes[s] || !state->stream_stride[s] || state->stream_length[s] < need) { set_errorf("declaration stream %d is not bound for %u vertices", s, nv); return MR_ERR_STATE; }
        stream_buffers[s] = resident_range(state->resident[s], state->resident_offset[s], need);
        if (stream_buffers[s]) { stream_offsets[s] = state->resident_offset[s]; atomic_fetch_add_explicit(&g_resident_bytes, need, memory_order_relaxed); continue; }
        stream_buffers[s] = arena_copy(c->s, state->stream_bytes[s], need, &stream_offsets[s]); atomic_fetch_add_explicit(&g_arena_vertex_bytes, need, memory_order_relaxed);
        if (!stream_buffers[s]) { g_err = "secondary stream buffer allocation failed"; return MR_ERR_DEVICE; }
    }
    float vs_uniforms[256][4], ps_uniforms[256][4];
    if (program->vs->packed_float4_count && (!state->vs_float4 || pack_float_uniforms(program->vs, state->vs_float4, state->vs_float4_count, vs_uniforms))) return MR_ERR_STATE;
    if (program->has_pixel_shader && program->ps->packed_float4_count && (!state->ps_float4 || pack_float_uniforms(program->ps, state->ps_float4, state->ps_float4_count, ps_uniforms))) return MR_ERR_STATE;
    /* Each stage's sampler is looked up once; binding reuses the result. */
    id<MTLSamplerState> stage_samplers[16] = {nil};
    for (int i = 0; i < program->ps->sampler_count; ++i) {
        int stage = program->ps->samplers[i].index;
        if (stage < 0 || stage >= 16 || program->ps->samplers[i].type < MR_SAMPLER_2D || program->ps->samplers[i].type > MR_SAMPLER_VOLUME) { set_errorf("pixel shader sampler %d is out of range", stage); return MR_ERR_TEXTURE; }
        if (!(stage_samplers[stage] = sampler_for(c, &state->samplers[stage]))) return MR_ERR_UNSUPPORTED;
    }
    if (!program->has_pixel_shader) for (int stage = 0; stage < 8 && state->fixed_stages[stage].color_op != 1; ++stage) if (fixed_stage_uses_texture(&state->fixed_stages[stage])) {
        uint32_t texture = state->samplers[stage].texture;
        int avail = texture && texture < MR_MAX_TEX && c->s->textures[texture];
        if (!avail && !mr_tex_fallback()) { set_errorf("fixed stage %d texture is unavailable", stage); return MR_ERR_TEXTURE; }
        if (!(stage_samplers[stage] = sampler_for(c, &state->samplers[stage]))) return MR_ERR_UNSUPPORTED;
    }
    if (c->vw <= 0 || c->vh <= 0) return MR_OK;
    id<MTLRenderCommandEncoder> enc = begin_draw_encoder(c); if (!enc) return MR_ERR_DEVICE;
    bind_draw_state(c->s, enc, program->pipeline, depth, state->stencil_enable, state->stencil_ref,
        (MTLViewport){ 0, 0, (double)c->w, (double)c->h, 0.0, 1.0 },
        (MTLScissorRect){ (NSUInteger)c->vx, (NSUInteger)c->vy, (NSUInteger)c->vw, (NSUInteger)c->vh },
        state->cull_mode == 1, state->cull_mode == 3);
    [enc setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:1];
    for (int s = 1; s < 16; ++s) if (stream_buffers[s]) [enc setVertexBuffer:stream_buffers[s] offset:stream_offsets[s] atIndex:1 + s];
    if (program->vs->packed_float4_count) [enc setVertexBytes:vs_uniforms length:(NSUInteger)program->vs->packed_float4_count * 16 atIndex:0];
    if (program->has_pixel_shader) {
        if (program->ps->packed_float4_count) [enc setFragmentBytes:ps_uniforms length:(NSUInteger)program->ps->packed_float4_count * 16 atIndex:0];
        if (program->extras) { float extras[8]; pixel_extras(state, extras); [enc setFragmentBytes:extras length:sizeof extras atIndex:1]; }
        for (int i = 0; i < program->ps->sampler_count; ++i) { int stage = program->ps->samplers[i].index;
            bind_fragment_texture(c->s, enc, ps_sampler_texture(c, state, stage, program->ps->samplers[i].type), stage_samplers[stage], stage); }
    } else {
        float fixed[12]; fixed_uniforms(state, fixed);
        [enc setFragmentBytes:fixed length:sizeof fixed atIndex:1];
        for (int stage = 0; stage < 8 && state->fixed_stages[stage].color_op != 1; ++stage) if (fixed_stage_uses_texture(&state->fixed_stages[stage])) {
            uint32_t texture = state->samplers[stage].texture;
            id<MTLTexture> t = (texture && texture < MR_MAX_TEX && c->s->textures[texture]) ? c->s->textures[texture] : c->s->white;
            bind_fragment_texture(c->s, enc, t, stage_samplers[stage], stage); }
    }
    MTLPrimitiveType type = mr_primitive_metal(prim);
    if (indices) [enc drawIndexedPrimitives:type indexCount:ni indexType:MTLIndexTypeUInt16 indexBuffer:index_buffer indexBufferOffset:index_offset];
    else [enc drawPrimitives:type vertexStart:0 vertexCount:nv];
    c->tris += mr_primitive_shapes(prim, count); return MR_OK;
    }
}

int mr_read_framebuffer(mr_context *c, void *out, size_t out_size) {
    @autoreleasepool {
    if (!c || !out || out_size < (size_t)c->w * c->h * 4) { g_err = "bad readback args"; return -1; }
    if (flush_pending(c, 1)) return -1;
    [c->target getBytes:out bytesPerRow:(NSUInteger)c->w * 4 fromRegion:MTLRegionMake2D(0, 0, c->w, c->h) mipmapLevel:0];
    return 0;
    }
}

int mr_blit_target_to(mr_context *c, void *mtl_texture) {
    @autoreleasepool {
    if (!c || !mtl_texture || !c->target) { g_err = "bad blit arguments"; return MR_ERR_ARGS; }
    id<MTLTexture> dst = (__bridge id<MTLTexture>)mtl_texture;
    if (dst.width != (NSUInteger)c->w || dst.height != (NSUInteger)c->h || dst.pixelFormat != c->target.pixelFormat) {
        set_errorf("blit target %lux%lu/%lu does not match %dx%d/%lu", (unsigned long)dst.width, (unsigned long)dst.height,
                   (unsigned long)dst.pixelFormat, c->w, c->h, (unsigned long)c->target.pixelFormat);
        return MR_ERR_ARGS;
    }
    mr_shared *s = c->s;
    /* dst may alias a context with a pending clear: preserve both sides. */
    encode_pending_clears(s);
    end_pending_encoder(s);
    if (ensure_command_buffer(s)) return MR_ERR_DEVICE;
    id<MTLBlitCommandEncoder> blit = [s->pending_cb blitCommandEncoder];
    if (!blit) { g_err = "blit encoder allocation failed"; return MR_ERR_DEVICE; }
    [blit copyFromTexture:c->target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)c->w, (NSUInteger)c->h, 1)
               toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    return MR_OK;
    }
}

/* FXAA 3.11, quality path, for a world layer on its way to the pool.
 *
 * Every bearing is drawn at one sample per pixel, and on the headset each
 * engine pixel spreads over about 1.3 display pixels across and 3.5 to 4 down
 * (1536 rows over 105 degrees against a display near 35 pixels a degree), so
 * a near-horizontal edge is a staircase of tall steps, and it crawls as the
 * edge moves. The pass runs on the layer's own bytes, gamma-encoded as D3D9
 * drew them, before the presenter's sRGB view decodes them: FXAA's luma is
 * meant to be perceptual. The HUD layer is never filtered. */
static NSString *const mr_fxaa_source = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct FxaaOut { float4 position [[position]]; float2 uv; };\n"
"vertex FxaaOut fxaa_vertex(uint vid [[vertex_id]]) {\n"
"    float2 p = float2(float((vid << 1) & 2u), float(vid & 2u));\n"
"    FxaaOut o; o.position = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0); o.uv = p; return o;\n"
"}\n"
"static float fxaa_luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }\n"
"fragment float4 fxaa_fragment(FxaaOut in [[stage_in]], texture2d<float> src [[texture(0)]], sampler s [[sampler(0)]],\n"
"                              constant float4 &parameters [[buffer(0)]]) {\n"
"    float2 rcp = parameters.xy; float subpix = parameters.z; float2 uv = in.uv;\n"
"    float4 rgbaM = src.sample(s, uv);\n"
"    float lumaM = fxaa_luma(rgbaM.rgb);\n"
"    float lumaS = fxaa_luma(src.sample(s, uv, int2( 0,  1)).rgb);\n"
"    float lumaE = fxaa_luma(src.sample(s, uv, int2( 1,  0)).rgb);\n"
"    float lumaN = fxaa_luma(src.sample(s, uv, int2( 0, -1)).rgb);\n"
"    float lumaW = fxaa_luma(src.sample(s, uv, int2(-1,  0)).rgb);\n"
"    float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));\n"
"    float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));\n"
"    float range = rangeMax - rangeMin;\n"
"    if (range < max(0.0312, rangeMax * 0.125)) return rgbaM;\n"
"    float lumaNW = fxaa_luma(src.sample(s, uv, int2(-1, -1)).rgb);\n"
"    float lumaSE = fxaa_luma(src.sample(s, uv, int2( 1,  1)).rgb);\n"
"    float lumaNE = fxaa_luma(src.sample(s, uv, int2( 1, -1)).rgb);\n"
"    float lumaSW = fxaa_luma(src.sample(s, uv, int2(-1,  1)).rgb);\n"
"    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;\n"
"    float subpixRcpRange = 1.0 / range, subpixNSWE = lumaNS + lumaWE;\n"
"    float edgeHorz1 = -2.0 * lumaM + lumaNS, edgeVert1 = -2.0 * lumaM + lumaWE;\n"
"    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;\n"
"    float edgeHorz2 = -2.0 * lumaE + lumaNESE, edgeVert2 = -2.0 * lumaN + lumaNWNE;\n"
"    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;\n"
"    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);\n"
"    float edgeHorz3 = -2.0 * lumaW + lumaNWSW, edgeVert3 = -2.0 * lumaS + lumaSWSE;\n"
"    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;\n"
"    float subpixNWSWNESE = lumaNWSW + lumaNESE;\n"
"    float lengthSign = rcp.x;\n"
"    bool horzSpan = edgeHorz >= edgeVert;\n"
"    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;\n"
"    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; } else lengthSign = rcp.y;\n"
"    float subpixB = subpixA * (1.0 / 12.0) - lumaM;\n"
"    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;\n"
"    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;\n"
"    bool pairN = abs(gradientN) >= abs(gradientS);\n"
"    float gradient = max(abs(gradientN), abs(gradientS));\n"
"    if (pairN) lengthSign = -lengthSign;\n"
"    float subpixC = saturate(abs(subpixB) * subpixRcpRange);\n"
"    float2 offNP = horzSpan ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);\n"
"    float2 posB = uv;\n"
"    if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;\n"
"    float2 posN = posB - offNP, posP = posB + offNP;\n"
"    float subpixD = -2.0 * subpixC + 3.0, subpixE = subpixC * subpixC;\n"
"    if (!pairN) lumaNN = lumaSS;\n"
"    float gradientScaled = gradient * 0.25;\n"
"    float lumaMM = lumaM - lumaNN * 0.5;\n"
"    float subpixF = subpixD * subpixE;\n"
"    bool lumaMLTZero = lumaMM < 0.0;\n"
"    float lumaEndN = fxaa_luma(src.sample(s, posN).rgb) - lumaNN * 0.5;\n"
"    float lumaEndP = fxaa_luma(src.sample(s, posP).rgb) - lumaNN * 0.5;\n"
"    bool doneN = abs(lumaEndN) >= gradientScaled, doneP = abs(lumaEndP) >= gradientScaled;\n"
"    const float steps[8] = { 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 8.0 };\n"
"    for (int i = 0; i < 8 && !(doneN && doneP); ++i) {\n"
"        if (!doneN) { posN -= offNP * steps[i]; lumaEndN = fxaa_luma(src.sample(s, posN).rgb) - lumaNN * 0.5; doneN = abs(lumaEndN) >= gradientScaled; }\n"
"        if (!doneP) { posP += offNP * steps[i]; lumaEndP = fxaa_luma(src.sample(s, posP).rgb) - lumaNN * 0.5; doneP = abs(lumaEndP) >= gradientScaled; }\n"
"    }\n"
"    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;\n"
"    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;\n"
"    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero, goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;\n"
"    float spanLengthRcp = 1.0 / (dstP + dstN);\n"
"    bool directionN = dstN < dstP;\n"
"    float dst = min(dstN, dstP);\n"
"    bool goodSpan = directionN ? goodSpanN : goodSpanP;\n"
"    float subpixG = subpixF * subpixF;\n"
"    float pixelOffset = dst * -spanLengthRcp + 0.5;\n"
"    float subpixH = subpixG * subpix;\n"
"    float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);\n"
"    float2 finalUV = uv;\n"
"    if (!horzSpan) finalUV.x += pixelOffsetSubpix * lengthSign; else finalUV.y += pixelOffsetSubpix * lengthSign;\n"
"    return float4(src.sample(s, finalUV).rgb, rgbaM.a);\n"
"}\n";

static id<MTLRenderPipelineState> fxaa_pipeline(mr_shared *s, MTLPixelFormat format) {
    if (s->fxaa_pipeline && s->fxaa_format == format) return s->fxaa_pipeline;
    if (s->fxaa_failed) return nil;
    @autoreleasepool {
    NSError *e = nil;
    id<MTLLibrary> lib = [s->dev newLibraryWithSource:mr_fxaa_source options:nil error:&e];
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [lib newFunctionWithName:@"fxaa_vertex"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fxaa_fragment"];
    pd.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> p = lib && pd.vertexFunction && pd.fragmentFunction
        ? [s->dev newRenderPipelineStateWithDescriptor:pd error:&e] : nil;
    if (!p) { s->fxaa_failed = 1; NSLog(@"metalrenderer: FXAA pipeline unavailable, publishing unfiltered: %@", e); return nil; }
    s->fxaa_pipeline = p; s->fxaa_format = format;
    return p;
    }
}

int mr_fxaa_target_to(mr_context *c, void *mtl_texture, float subpix) {
    @autoreleasepool {
    if (!c || !mtl_texture || !c->target) { g_err = "bad FXAA arguments"; return MR_ERR_ARGS; }
    id<MTLTexture> dst = (__bridge id<MTLTexture>)mtl_texture;
    if (dst.width != (NSUInteger)c->w || dst.height != (NSUInteger)c->h || dst.pixelFormat != c->target.pixelFormat) {
        set_errorf("FXAA target %lux%lu/%lu does not match %dx%d/%lu", (unsigned long)dst.width, (unsigned long)dst.height,
                   (unsigned long)dst.pixelFormat, c->w, c->h, (unsigned long)c->target.pixelFormat);
        return MR_ERR_ARGS;
    }
    mr_shared *s = c->s;
    id<MTLRenderPipelineState> p = (dst.usage & MTLTextureUsageRenderTarget) ? fxaa_pipeline(s, dst.pixelFormat) : nil;
    if (!p) return mr_blit_target_to(c, mtl_texture);
    /* dst may alias a context with a pending clear: preserve both sides. */
    encode_pending_clears(s);
    end_pending_encoder(s);
    if (ensure_command_buffer(s)) return MR_ERR_DEVICE;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [s->pending_cb renderCommandEncoderWithDescriptor:rp];
    if (!enc) { g_err = "FXAA encoder allocation failed"; return MR_ERR_DEVICE; }
    float parameters[4] = { 1.f / (float)c->w, 1.f / (float)c->h, subpix, 0.f };
    [enc setRenderPipelineState:p];
    [enc setFragmentTexture:c->target atIndex:0];
    [enc setFragmentSamplerState:s->samplers[3] atIndex:0];
    [enc setFragmentBytes:parameters length:sizeof parameters atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
    return MR_OK;
    }
}

int mr_blit_texture_copy(void *src_texture, void *dst_texture) {
    @autoreleasepool {
    if (!src_texture || !dst_texture) { g_err = "bad copy arguments"; return MR_ERR_ARGS; }
    id<MTLTexture> src = (__bridge id<MTLTexture>)src_texture;
    id<MTLTexture> dst = (__bridge id<MTLTexture>)dst_texture;
    if (src.width != dst.width || src.height != dst.height || src.pixelFormat != dst.pixelFormat) {
        set_errorf("texture copy %lux%lu/%lu does not match %lux%lu/%lu",
                   (unsigned long)src.width, (unsigned long)src.height, (unsigned long)src.pixelFormat,
                   (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat);
        return MR_ERR_ARGS;
    }
    mr_shared *s = g_shared;
    if (!s) { g_err = "no shared renderer"; return MR_ERR_DEVICE; }
    encode_pending_clears(s);
    end_pending_encoder(s);
    if (ensure_command_buffer(s)) return MR_ERR_DEVICE;
    id<MTLBlitCommandEncoder> blit = [s->pending_cb blitCommandEncoder];
    if (!blit) { g_err = "blit encoder allocation failed"; return MR_ERR_DEVICE; }
    [blit copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(src.width, src.height, 1)
                toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    return MR_OK;
    }
}

int mr_commit_async(void (*done)(void *arg, int ok), void *arg) {
    @autoreleasepool {
    mr_shared *s = g_shared;
    if (!s) { if (done) done(arg, 0); return MR_ERR_DEVICE; }
    encode_pending_clears(s);
    end_pending_encoder(s);
    if (!s->pending_cb) { if (done) done(arg, 1); return MR_OK; }
    id<MTLCommandBuffer> cb = s->pending_cb;
    if (done) [cb addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        int ok = finished.status != MTLCommandBufferStatusError;
        if (!ok) {
            /* Keep the reason. A failed publish on the headset was otherwise a
             * bare "command buffer failed", which cannot tell a GPU fault in
             * the engine's own work from the compositor discarding everything
             * because the app went into the background. */
            NSError *e = finished.error;
            snprintf(g_commit_err, sizeof g_commit_err, "code %ld: %s", (long)(e ? e.code : 0),
                     e ? e.localizedDescription.UTF8String : "no error object");
        }
        done(arg, ok);
    }];
    arena_retire(s->arena, cb);
    [cb commit];
    s->last_cb = cb; s->pending_cb = nil; s->arena = nil; s->arena_used = s->arena_size = 0;
    return MR_OK;
    }
}

void *mr_shared_device(void) { mr_shared *s = shared_state(); return s ? (__bridge void *)s->dev : NULL; }

int mr_write_framebuffer(mr_context *c, const void *in, size_t size) {
    @autoreleasepool {
    if (!c || !in || size < (size_t)c->w * c->h * 4) return MR_ERR_ARGS;
    /* The shared command buffer orders this upload after earlier readers and
     * before later draws. Its retained references own the staging arena until
     * completion; never overwrite a texture from the CPU or drain the GPU. */
    mr_shared *s = c->s;
    encode_context_clears(c);
    end_pending_encoder(s);
    if (ensure_command_buffer(s)) return MR_ERR_DEVICE;
    size_t row = ((size_t)c->w * 4 + 255u) & ~(size_t)255u, offset;
    id<MTLBuffer> staging = arena_alloc(s, row * (size_t)c->h, &offset);
    if (!staging) return MR_ERR_DEVICE;
    for (int y = 0; y < c->h; ++y)
        memcpy((uint8_t *)staging.contents + offset + row * y,
               (const uint8_t *)in + (size_t)c->w * 4 * y, (size_t)c->w * 4);
    id<MTLBlitCommandEncoder> blit = [s->pending_cb blitCommandEncoder];
    if (!blit) { g_err = "upload blit allocation failed"; return MR_ERR_DEVICE; }
    [blit copyFromBuffer:staging sourceOffset:offset sourceBytesPerRow:row
            sourceBytesPerImage:row * (size_t)c->h
            sourceSize:MTLSizeMake(c->w, c->h, 1) toTexture:c->target
            destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    return MR_OK;
    }
}
