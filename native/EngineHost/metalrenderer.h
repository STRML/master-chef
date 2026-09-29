/*
 * metalrenderer.h - Offscreen Metal rasterizer for the D3D9 shim (plain C API).
 *
 * Draws pretransformed (D3DFVF_XYZRHW | DIFFUSE | TEX1) textured triangles into
 * an offscreen BGRA8 render target and lets the host read the framebuffer back
 * so it can be handed to metalwin_present() (macOS) or a visionOS presenter.
 * No Cocoa/UIKit: Metal + Foundation only, so metalrenderer.m compiles for
 * macOS and visionOS unchanged.
 *
 * Coordinates follow D3D9 pretransformed vertices: x,y in pixels with (0,0) at
 * the top-left of the target, z in [0,1], rhw = 1/w (used for perspective-
 * correct texturing only). Colours are D3DCOLOR (0xAARRGGBB in a uint32, i.e.
 * bytes B,G,R,A in memory). Textures are BGRA8 rows with an explicit pitch.
 *
 * Threading: one context, call all functions from the same thread.
 */
#ifndef METALRENDERER_H
#define METALRENDERER_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mr_context mr_context;   /* opaque */

typedef struct mr_vertex_rhw {          /* 28 bytes, D3DFVF_XYZRHW|DIFFUSE|TEX1 */
    float x, y, z, rhw;
    uint32_t color;                     /* D3DCOLOR, 0xAARRGGBB */
    float u, v;
} mr_vertex_rhw;

/* CPU-transformed fixed-function 3D vertex.  Separate coordinates are kept
 * for all D3D texture stages so a normal/detail/lightmap pass is evaluated by
 * the original texture-stage combiner instead of painting texture zero alone. */
typedef struct mr_vertex_fixed_rhw {
    float x, y, z, rhw;
    uint32_t color;
    uint32_t specular;                  /* D3DCOLOR COLOR1, feeds D3DTA_SPECULAR / v1 */
    float uv[8][2];
    float fog;                         /* transmittance when fog_enable is set */
} mr_vertex_fixed_rhw;

/* CPU-transformed fixed-function vertex already in homogeneous clip space.
 * Keeping clip w avoids the undefined divide/reconstruct round trip at the
 * camera plane; Metal performs the actual near/behind-camera clipping. */
typedef struct mr_vertex_fixed_clip {
    float x, y, z, w;
    uint32_t color;
    uint32_t specular;
    float uv[8][2];
    float fog;
} mr_vertex_fixed_clip;

enum mr_primitive { MR_TRIANGLE_LIST = 1, MR_TRIANGLE_STRIP = 2,
                    MR_LINE_LIST = 3, MR_LINE_STRIP = 4, MR_POINT_LIST = 5 };

enum mr_blend { MR_BLEND_NONE = 0,       /* opaque: out = src */
                MR_BLEND_SRC_ALPHA = 1,  /* out = src*srcA + dst*(1-srcA) (D3DBLEND_SRCALPHA/INVSRCALPHA) */
                MR_BLEND_ADD = 2,        /* out = src + dst (D3DBLEND_ONE/ONE) */
                MR_BLEND_MODULATE = 3,   /* out = src*dst (D3DBLEND_DESTCOLOR/ZERO): lightmap/detail multiply */
                MR_BLEND_SRC_ALPHA_ZERO = 4, /* out = src*srcA */
                MR_BLEND_SRC_ALPHA_ADD = 5,  /* out = src*srcA + dst */
                MR_BLEND_PREMULTIPLIED = 6, /* out = src + dst*(1-srcA) */
                MR_BLEND_MODULATE2 = 7,   /* out = 2*src*dst (D3DBLEND_DESTCOLOR/SRCCOLOR) */
                MR_BLEND_DEST_ALPHA_ADD = 8, /* RGBA out = src*dstA + dst (D3DBLEND_DESTALPHA/ONE) */
                MR_BLEND_CUSTOM = 9,      /* raw D3DBLEND factors and D3DBLENDOP from blend_src/blend_dst/blend_op */
                MR_BLEND_COUNT = 10 };

typedef struct mr_draw_state {
    uint32_t texture;                   /* texture id from mr_texture_create, 0 = untextured (diffuse only) */
    int blend;                          /* enum mr_blend */
    int blend_src, blend_dst, blend_op; /* D3DBLEND_* 1..15 and D3DBLENDOP_* 1..5, used with MR_BLEND_CUSTOM */
    int linear_filter;                  /* 1 = bilinear, 0 = nearest (D3DTEXF_POINT) */
    int address_clamp;                  /* 1 = clamp, 0 = wrap (D3DTADDRESS_WRAP) */
    const struct mr_program_sampler *sampler; /* optional complete sampler state */
    int texture_alpha_only;             /* 1 = A8-style: colour from diffuse, alpha from texture alpha */
    int texture_color_only;             /* multiply texture RGB, keep diffuse alpha */
    int alpha_test_ref;                 /* -1 = off, else discard when alpha*255 < ref (D3DCMP_GREATEREQUAL) */
    int color_write_mask;               /* 0xF = all; bit0 R, bit1 G, bit2 B, bit3 A */
    int depth_enable;                   /* fixed-function 3D: enable depth test/attachment */
    int depth_write;                    /* write depth */
    int depth_compare;                  /* D3DCMP_* 1..8 */
    int cull_mode;                      /* D3DCULL_* 1 none, 2 CW, 3 CCW; 0 keeps legacy no-cull */
    int stencil_enable;
    int stencil_fail, stencil_depth_fail, stencil_pass; /* D3DSTENCILOP_* 1..8 */
    int stencil_compare;                /* D3DCMP_* 1..8 */
    uint32_t stencil_ref, stencil_read_mask, stencil_write_mask;
} mr_draw_state;

/* Programmable D3D9 draw state. Numeric values intentionally match D3D9 so
 * the host can copy its captured render/texture-stage state without a second
 * translation table. Only 2D textures and stream zero are currently accepted;
 * unsupported declarations, samplers, or operations fail explicitly. */
enum mr_sampler_type { MR_SAMPLER_2D = 0, MR_SAMPLER_CUBE = 1, MR_SAMPLER_VOLUME = 2 };
typedef struct mr_program_sampler {
    uint32_t texture;                   /* mr_texture_create id, 0 = unbound */
    uint8_t type;                       /* enum mr_sampler_type */
    uint8_t linear_filter;              /* legacy fallback when min/mag are zero */
    uint8_t address_u;                  /* D3DTADDRESS_*: wrap, clamp, transparent-black border */
    uint8_t address_v;
    uint8_t min_filter;                 /* D3DTEXF_*: point, linear, anisotropic */
    uint8_t mag_filter;                 /* D3DTEXF_*: point, linear, anisotropic */
    uint8_t mip_filter;                 /* D3DTEXF_NONE, POINT, or LINEAR */
    uint8_t max_anisotropy;             /* D3DSAMP_MAXANISOTROPY, clamped to 1..16 */
    uint8_t max_mip_level;              /* D3DSAMP_MAXMIPLEVEL / Metal minimum LOD */
    uint32_t border_color;              /* D3DSAMP_BORDERCOLOR; border mode currently requires zero */
} mr_program_sampler;

typedef struct mr_fixed_stage {
    uint32_t color_op;                  /* D3DTOP_* */
    uint32_t color_arg0, color_arg1, color_arg2; /* D3DTA_* incl. modifiers */
    uint32_t alpha_op;
    uint32_t alpha_arg0, alpha_arg1, alpha_arg2;
    uint32_t result_arg;                /* D3DTA_CURRENT or D3DTA_TEMP */
    uint32_t texcoord_index;            /* low 16 bits; generated coords unsupported */
    uint32_t texture_transform_flags;   /* D3DTTFF_DISABLE supported */
    uint32_t constant;                  /* D3DTSS_CONSTANT, D3DCOLOR */
} mr_fixed_stage;

/* Only operands consumed by D3DTOP participate in texture dependencies.
 * In particular SELECTARG1/2 must not require stale values in other slots. */
static inline unsigned mr_fixed_op_arg_mask(uint32_t op) {
    switch (op) {
        case 1: return 0;
        case 2: case 17: return 2;
        case 3: return 4;
        case 25: case 26: return 7;
        default: return 6;
    }
}
static inline int mr_fixed_op_uses_texture(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2) {
    unsigned mask=mr_fixed_op_arg_mask(op);
    return op==13 || op==15 ||
        ((mask&1) && (a0&15u)==2) || ((mask&2) && (a1&15u)==2) || ((mask&4) && (a2&15u)==2);
}
static inline int mr_fixed_stage_uses_texture(const mr_fixed_stage *s) {
    return s->color_op!=1 &&
        (mr_fixed_op_uses_texture(s->color_op,s->color_arg0,s->color_arg1,s->color_arg2) ||
         mr_fixed_op_uses_texture(s->alpha_op,s->alpha_arg0,s->alpha_arg1,s->alpha_arg2));
}

typedef struct mr_program_state {
    const uint32_t *vertex_tokens;
    size_t vertex_token_bytes;
    const uint32_t *pixel_tokens;       /* NULL selects fixed-function fragment stages */
    size_t pixel_token_bytes;
    /* Hashes of the token streams computed once at shader creation; when set
     * the pipeline caches key on them instead of rehashing every draw. */
    uint64_t vertex_key, pixel_key;
    const void *declaration;            /* D3DVERTEXELEMENT9 byte array, including end */
    size_t declaration_bytes;
    const float *vs_float4;             /* complete D3D c0.. register file */
    uint32_t vs_float4_count;
    const float *ps_float4;
    uint32_t ps_float4_count;
    mr_program_sampler samplers[16];
    mr_fixed_stage fixed_stages[8];
    uint32_t texture_factor;             /* D3DCOLOR used by D3DTA_TFACTOR */
    int blend;                           /* enum mr_blend */
    int blend_src, blend_dst, blend_op;  /* D3DBLEND_* / D3DBLENDOP_* for MR_BLEND_CUSTOM */
    int fog_enable;                      /* D3DRS_FOGENABLE: blend toward fog_color by the vertex shader's oFog factor */
    uint32_t fog_color;                  /* D3DRS_FOGCOLOR */
    /* 0, or HALO_RADIAL_FOG_DEPTH / HALO_RADIAL_FOG_VIEW_PLANE: measure the
     * vertex shader's fog from the eye instead of along the view axis
     * (radial_fog.h), so panorama bearings fog a point alike. */
    int radial_fog;
    /* Secondary vertex streams referenced by the declaration (stream 1..15).
     * Stream zero is the vertices argument of mr_draw_program. Each pointer
     * addresses the first drawn vertex; length covers vertex_count vertices. */
    const void *stream_bytes[16];
    uint32_t stream_stride[16];
    size_t stream_length[16];
    /* Optional persistent copies (mr_buffer_create) that already hold the
     * same bytes as `vertices` / stream_bytes[s], starting at the given
     * offset. The draw binds the copy instead of copying the bytes into the
     * frame arena; an unaligned or out-of-range offset falls back to the
     * copy. Index 0 is stream zero. The caller guarantees byte identity. */
    const void *resident[16];
    size_t resident_offset[16];
    int color_write_mask;                /* D3DCOLORWRITEENABLE bits */
    int alpha_test_ref;                  /* -1 disabled, otherwise 0..255 */
    int alpha_test_func;                 /* D3DCMP_* 1..8 */
    int depth_enable;
    int depth_write;
    int depth_compare;                   /* D3DCMP_* 1..8 */
    int cull_mode;                       /* D3DCULL_* 1 none, 2 CW, 3 CCW */
    int stencil_enable;
    int stencil_fail, stencil_depth_fail, stencil_pass;
    int stencil_compare;
    uint32_t stencil_ref, stencil_read_mask, stencil_write_mask;
} mr_program_state;

/* Create/destroy a context whose render target is width x height BGRA8.
 * Returns NULL if no Metal device or the pipeline failed to build; the reason
 * is available via mr_last_error(). */
mr_context *mr_create(int width, int height);
void mr_destroy(mr_context *ctx);
/* Pipelines the engine thread built at a draw (none ready), and the time it
 * spent building them, not counting time spent waiting for a worker. */
void mr_program_compile_stats(uint64_t *count, uint64_t *ns);
/* Shader pipelines are compiled ahead of the draws that need them: the game's
 * shaders when it creates them, and every pipeline an earlier session used
 * (a manifest in the Caches directory; optionally an MTLBinaryArchive). */
typedef struct mr_pipeline_stats_t {
    uint64_t engine_builds, engine_build_ns;    /* as mr_program_compile_stats */
    uint64_t engine_functions;                  /* MSL functions compiled on the engine thread */
    uint64_t waits, wait_ns;                    /* draws that waited for a worker's pipeline or function */
    uint64_t background_pipelines, background_functions, background_ns;  /* worker builds and time */
    uint64_t prewarm_hits;                      /* first draws that found a worker's pipeline ready */
    uint64_t archive_hits, archive_stored;      /* pipelines loaded from / added to the binary archive (HALO_PIPELINE_ARCHIVE=1) */
    uint64_t shaders_registered, recipes_loaded;
} mr_pipeline_stats_t;
void mr_pipeline_stats(mr_pipeline_stats_t *out);
/* The game created a shader (D3D token stream; `key` its content hash or 0).
 * Starts its translation and compilation on a background worker. Call from
 * the thread that draws. */
void mr_shader_created(int pixel, const uint32_t *tokens, size_t bytes, uint64_t key);
/* Write the pipeline manifest and archive now rather than at the next quiet
 * moment. */
void mr_pipeline_cache_flush(void);
/* Preserve source-over coverage when drawing a transparent HUD layer. */
void mr_set_overlay_target(mr_context *ctx);
const char *mr_last_error(void);        /* static string, never NULL */
int mr_width(const mr_context *ctx);
int mr_height(const mr_context *ctx);

/* Recreate the render target at a new size (D3D Reset). Returns 0 on success. */
int mr_resize(mr_context *ctx, int width, int height);

/* Viewport in target pixels (D3D9 SetViewport semantics; also the scissor). */
void mr_set_viewport(mr_context *ctx, int x, int y, int w, int h);

/* Clear color, depth, or stencil independently (matching D3DCLEAR_*). */
void mr_clear(mr_context *ctx, uint32_t d3dcolor);
void mr_clear_depth(mr_context *ctx, float depth);
void mr_clear_stencil(mr_context *ctx, uint32_t value);

/* Textures: BGRA8 with `pitch` bytes per row (pitch >= width*4). A copy is
 * taken. Returns id > 0, or 0 on failure. mr_texture_update replaces the
 * whole image with new bytes of the same size. */
uint32_t mr_texture_create(mr_context *ctx, int width, int height, const void *bgra, size_t pitch);
int mr_texture_update(mr_context *ctx, uint32_t id, const void *bgra, size_t pitch);
void mr_texture_destroy(mr_context *ctx, uint32_t id);
/* Immutable texture cache used by the D3D bridge. `key` identifies both the
 * source resource and its current bytes. A hit avoids decoding/uploading the
 * same Halo texture for every draw and panorama view. Cached ids are owned by
 * the context and remain valid until eviction or context destruction. */
/* Pair around a D3D draw's sampler resolution and encoding. Nested scopes
 * are supported; already-resolved IDs cannot be recycled by later uploads. */
void mr_texture_bindings_begin(void);
void mr_texture_bindings_end(void);
void mr_texture_cache_counters(uint64_t *hits, uint64_t *uploads, uint64_t *evictions);
uint32_t mr_texture_find_cached(mr_context *ctx, uint64_t key);
uint32_t mr_texture_create_cached(mr_context *ctx, uint64_t key, int width, int height,
                                  const void *bgra, size_t pitch);
/* Same, level zero only: for render-target contents that are sampled at 1:1
 * every frame, where CPU mip generation dominated the frame time. */
uint32_t mr_texture_create_cached_nomip(mr_context *ctx, uint64_t key, int width, int height,
                                        const void *bgra, size_t pitch);
uint32_t mr_texture_create_cube_cached(mr_context *ctx, uint64_t key, int edge,
                                       const void *face_bgra[6], size_t pitch);
/* Level-zero 3D texture: depth slices of width x height BGRA8, each slice
 * slice_pitch bytes apart. No lower mips are generated. */
uint32_t mr_texture_create_volume_cached(mr_context *ctx, uint64_t key, int width, int height, int depth,
                                         const void *bgra, size_t pitch, size_t slice_pitch);

/* Draw pretransformed vertices. vertex_stride must be >= sizeof(mr_vertex_rhw)
 * and the first 28 bytes of each vertex must match mr_vertex_rhw (extra bytes
 * are ignored, which covers FVFs with additional texcoord sets).
 * For indexed draws `indices16` holds uint16 indices (index_count of them);
 * pass NULL to draw vertex_count vertices in order.
 * Bounds: vertex_count <= 65536, index_count <= 3*65536, every index must be
 * < vertex_count. Returns 0 on success, MR_ERR_* otherwise (nothing drawn). */
enum { MR_OK = 0, MR_ERR_ARGS = 1, MR_ERR_BOUNDS = 2, MR_ERR_PRIMITIVE = 3,
       MR_ERR_TEXTURE = 4, MR_ERR_DEVICE = 5, MR_ERR_SHADER = 6,
       MR_ERR_DECL = 7, MR_ERR_UNSUPPORTED = 8, MR_ERR_STATE = 9 };
int mr_draw_rhw(mr_context *ctx, const mr_draw_state *state, int primitive,
                const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                const uint16_t *indices16, uint32_t index_count);

/* Translate/cache the D3D shader pair with pinned upstream MojoShader, bind
 * declaration/constant/texture/render state, and draw raw stream-zero bytes.
 * The index and primitive rules match mr_draw_rhw. */
int mr_draw_program(mr_context *ctx, const mr_program_state *state, int primitive,
                    const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                    const uint16_t *indices16, uint32_t index_count);

/* Fixed-function fragment stages over CPU-transformed vertices.  The render,
 * sampler, and mr_fixed_stage fields of `state` are used; shader token,
 * declaration, and constant pointers are ignored. */
int mr_draw_fixed_rhw(mr_context *ctx, const mr_program_state *state, int primitive,
                      const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                      const uint16_t *indices16, uint32_t index_count);

/* Pretransformed vertices (mr_vertex_fixed_rhw) shaded by a translated D3D
 * pixel shader: state->pixel_tokens, ps_float4 and samplers are used. */
int mr_draw_program_rhw(mr_context *ctx, const mr_program_state *state, int primitive,
                        const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                        const uint16_t *indices16, uint32_t index_count);

/* Fixed-function fragment stages over homogeneous clip-space vertices. */
int mr_draw_fixed_clip(mr_context *ctx, const mr_program_state *state, int primitive,
                       const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                       const uint16_t *indices16, uint32_t index_count);

/* Persistent GPU copy of a static vertex buffer's bytes, shared by every
 * context. The copy is immutable: new contents need a new buffer, so draws
 * already queued keep reading what they were encoded with. Returns a retained
 * handle, or NULL. */
void *mr_buffer_create(const void *bytes, size_t length);
void mr_buffer_release(void *buffer);
const void *mr_buffer_contents(const void *buffer);
size_t mr_buffer_length(const void *buffer);
/* Draw-path shortcuts that feed the GPU exactly what the plain path does:
 * resident vertex copies, clears folded into the next render pass, and
 * encoder state reuse. Off unless HALO_DRAW_FASTPATH=1 pending headset
 * validation; the setter is for engine-thread tests to run both paths.
 * Pending clears are encoded before the switch changes. The getter and
 * traffic counters can also be read by the diagnostics thread. */
int mr_fast_paths_enabled(void);
void mr_set_fast_paths(int enabled);
/* Cumulative draw data traffic: bytes bound from resident copies, bytes
 * copied into the arena for vertices/streams, and clears folded into a
 * following pass instead of a pass of their own. */
void mr_draw_traffic_stats(uint64_t *resident_bytes, uint64_t *arena_vertex_bytes, uint64_t *folded_clears);

/* Wait for all queued GPU work, then copy the BGRA8 target into `out`
 * (width*height*4 bytes, tightly packed, row 0 = top). Returns 0 on success. */
int mr_read_framebuffer(mr_context *ctx, void *out, size_t out_size);
/* Upload CPU-written surface pixels, preserving later draw/clear ordering. */
int mr_write_framebuffer(mr_context *ctx, const void *in, size_t size);

/* Texture id (shared table) aliasing this context's color target, safe to
 * sample from another context's draws in submission order. 0 if unavailable. */
uint32_t mr_target_texture(mr_context *ctx);
/* Zero-copy hand-off: blit a context's colour target into an external
 * MTLTexture (same device, same size and format), ordered behind every
 * pending draw. Nothing waits for the GPU. */
int mr_blit_target_to(mr_context *ctx, void *mtl_texture);
/* The same copy through FXAA; falls back to the plain blit when the texture
 * cannot be rendered to or the pipeline is unavailable. */
int mr_fxaa_target_to(mr_context *ctx, void *mtl_texture, float subpix);
/* Copy one Metal texture into another on the shared command buffer, so the
 * copy is ordered against the engine's own blits into the same textures. */
int mr_blit_texture_copy(void *src_mtl_texture, void *dst_mtl_texture);
/* Commit the pending GPU work. done(arg, ok) is called exactly once, on a
 * Metal completion thread when the work finishes, or immediately when there
 * was nothing pending (ok=1) or no device (ok=0). */
int mr_commit_async(void (*done)(void *arg, int ok), void *arg);
/* Why the most recent committed command buffer failed ("code N: text"), or
 * an empty string. Set inside the completion handler before `done` runs. */
const char *mr_last_commit_error(void);
/* The shared MTLDevice as an unretained id<MTLDevice>, or NULL. */
void *mr_shared_device(void);

/* Number of triangles drawn since creation (diagnostics). */
uint64_t mr_triangles_drawn(const mr_context *ctx);
uint32_t mr_cached_texture_count(const mr_context *ctx);
uint64_t mr_cached_texture_bytes(const mr_context *ctx);

#ifdef __cplusplus
}
#endif
#endif /* METALRENDERER_H */
