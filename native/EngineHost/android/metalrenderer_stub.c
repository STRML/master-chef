/* metalrenderer_stub.c - headless stand-in for the Metal renderer.
 *
 * This implements every function declared in metalrenderer.h with behaviour
 * that keeps the translated engine's D3D9->present loop running end to end
 * on the CPU with no GPU:
 *   - mr_create returns a small heap context carrying the target size, so
 *     the device reaches the ready state and surface_prepare succeeds.
 *   - All clears/draws are no-ops (they count triangles for diagnostics).
 *   - mr_read_framebuffer zero-fills, so a readback yields a black frame;
 *     mr_write_framebuffer accepts uploads. The engine's panorama/HUD path
 *     therefore completes without a real framebuffer.
 *   - Textures hand back fresh ids (find_cached always misses: re-decoding
 *     on the CPU is cheap here and avoids a stale-cache surface).
 *   - mr_commit_async invokes its completion immediately with ok=1.
 * Phase 3 replaces this file with the Vulkan backend (same header contract),
 * so nothing above this line changes when real rendering arrives.
 */
#include "metalrenderer.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct mr_context {
    int w, h;
    _Atomic uint64_t tris;
    uint32_t target_tex;
};

/* Resident vertex-buffer block: a length header followed by the bytes, so
 * mr_buffer_contents/mr_buffer_length round-trip exactly what the D3D bridge
 * handed to mr_buffer_create (vb_resident_equal compares these bytes to the
 * guest buffer, keeping the fast path alive). */
typedef struct { size_t len; } bufhdr;

static _Atomic uint32_t g_tex_id = 1;
static _Atomic int g_fast_paths = 0;
static _Atomic uint64_t g_resident_bytes, g_arena_bytes, g_folded_clears;
static _Atomic uint64_t g_tex_cache_hits, g_tex_uploads, g_tex_evictions;
static _Atomic uint64_t g_tex_count, g_tex_bytes;

static _Thread_local const char *g_last_error = "";
static _Thread_local const char *g_commit_err = "";

static void set_err(const char *e) { g_last_error = e; }

/* ---- lifecycle ---- */
mr_context *mr_create(int width, int height) {
    if (width <= 0 || height <= 0) { set_err("bad size"); return NULL; }
    struct mr_context *c = (struct mr_context *)calloc(1, sizeof *c);
    if (!c) { set_err("out of memory"); return NULL; }
    c->w = width; c->h = height; c->target_tex = 0;
    atomic_store_explicit(&c->tris, 0, memory_order_relaxed);
    return c;
}
void mr_destroy(mr_context *ctx) { free(ctx); }

int mr_resize(mr_context *ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) return -1;
    ctx->w = width; ctx->h = height;
    return 0;
}

/* ---- state ---- */
void mr_set_viewport(mr_context *ctx, int x, int y, int w, int h) {
    (void)ctx; (void)x; (void)y; (void)w; (void)h;
}
void mr_clear(mr_context *ctx, uint32_t d3dcolor) { (void)ctx; (void)d3dcolor; }
void mr_clear_depth(mr_context *ctx, float depth) { (void)ctx; (void)depth; }
void mr_clear_stencil(mr_context *ctx, uint32_t value) { (void)ctx; (void)value; }
void mr_set_overlay_target(mr_context *ctx) { (void)ctx; }

/* ---- shaders / pipeline (no-op: MSL never reaches a GPU here) ---- */
void mr_shader_created(int pixel, const uint32_t *tokens, size_t bytes, uint64_t key) {
    (void)pixel; (void)tokens; (void)bytes; (void)key;
}
void mr_program_compile_stats(uint64_t *count, uint64_t *ns) {
    if (count) *count = 0;
    if (ns) *ns = 0;
}
void mr_pipeline_stats(mr_pipeline_stats_t *out) { if (out) memset(out, 0, sizeof *out); }
void mr_pipeline_cache_flush(void) { }

/* ---- textures ---- */
uint32_t mr_texture_create(mr_context *ctx, int width, int height, const void *bgra, size_t pitch) {
    (void)bgra; (void)pitch;
    if (!ctx || width <= 0 || height <= 0) { set_err("bad texture args"); return 0; }
    atomic_fetch_add_explicit(&g_tex_uploads, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_tex_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_tex_bytes, (uint64_t)width * height * 4, memory_order_relaxed);
    return atomic_fetch_add_explicit(&g_tex_id, 1, memory_order_relaxed);
}
int mr_texture_update(mr_context *ctx, uint32_t id, const void *bgra, size_t pitch) {
    (void)ctx; (void)id; (void)bgra; (void)pitch;
    return 0;
}
void mr_texture_destroy(mr_context *ctx, uint32_t id) {
    (void)ctx; (void)id;
    atomic_fetch_add_explicit(&g_tex_evictions, 1, memory_order_relaxed);
}
void mr_texture_bindings_begin(void) { }
void mr_texture_bindings_end(void) { }
void mr_texture_cache_counters(uint64_t *hits, uint64_t *uploads, uint64_t *evictions) {
    if (hits) *hits = atomic_load_explicit(&g_tex_cache_hits, memory_order_relaxed);
    if (uploads) *uploads = atomic_load_explicit(&g_tex_uploads, memory_order_relaxed);
    if (evictions) *evictions = atomic_load_explicit(&g_tex_evictions, memory_order_relaxed);
}
/* Always miss: the caller then uploads via the *_cached entry points, which
 * return a fresh id. Correct, just unoptimised, which is the headless trade. */
uint32_t mr_texture_find_cached(mr_context *ctx, uint64_t key) { (void)ctx; (void)key; return 0; }
uint32_t mr_texture_create_cached(mr_context *ctx, uint64_t key, int width, int height, const void *bgra, size_t pitch) {
    (void)key; return mr_texture_create(ctx, width, height, bgra, pitch);
}
uint32_t mr_texture_create_cached_nomip(mr_context *ctx, uint64_t key, int width, int height, const void *bgra, size_t pitch) {
    (void)key; return mr_texture_create(ctx, width, height, bgra, pitch);
}
uint32_t mr_texture_create_cube_cached(mr_context *ctx, uint64_t key, int edge, const void *face_bgra[6], size_t pitch) {
    (void)key; (void)face_bgra;
    if (!ctx || edge <= 0 || pitch < (size_t)edge * 4) { set_err("bad cube args"); return 0; }
    atomic_fetch_add_explicit(&g_tex_uploads, 6, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_tex_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_tex_bytes, (uint64_t)edge * edge * 4 * 6, memory_order_relaxed);
    return atomic_fetch_add_explicit(&g_tex_id, 1, memory_order_relaxed);
}
uint32_t mr_texture_create_volume_cached(mr_context *ctx, uint64_t key, int width, int height, int depth,
                                         const void *bgra, size_t pitch, size_t slice_pitch) {
    (void)key; (void)bgra; (void)slice_pitch;
    if (!ctx || width <= 0 || height <= 0 || depth <= 0) { set_err("bad volume args"); return 0; }
    atomic_fetch_add_explicit(&g_tex_uploads, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_tex_count, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_tex_bytes, (uint64_t)width * height * 4 * depth, memory_order_relaxed);
    return atomic_fetch_add_explicit(&g_tex_id, 1, memory_order_relaxed);
}
uint32_t mr_target_texture(mr_context *ctx) { return ctx ? ctx->target_tex : 0; }

/* ---- draws (no pixels, triangles counted) ---- */
static void count_tris(mr_context *ctx, int primitive, uint32_t nv, uint32_t ni) {
    uint64_t n;
    if (ni) n = ni / 3;
    else if (nv >= 3) n = (primitive == MR_TRIANGLE_STRIP) ? (nv - 2) : (nv / 3);
    else n = 0;
    atomic_fetch_add_explicit(&ctx->tris, n, memory_order_relaxed);
}
static int draw_common(mr_context *ctx, int primitive, const void *vertices,
                       size_t vertex_stride, uint32_t vertex_count,
                       const uint16_t *indices16, uint32_t index_count) {
    (void)vertex_stride;
    if (!ctx) return MR_ERR_DEVICE;
    if (!vertices || vertex_count == 0) return MR_ERR_ARGS;
    if (indices16 && index_count % 3) return MR_ERR_BOUNDS;
    count_tris(ctx, primitive, vertex_count, index_count);
    return MR_OK;
}
int mr_draw_rhw(mr_context *ctx, const mr_draw_state *state, int primitive,
                const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                const uint16_t *indices16, uint32_t index_count) {
    (void)state;
    return draw_common(ctx, primitive, vertices, vertex_stride, vertex_count, indices16, index_count);
}
int mr_draw_program(mr_context *ctx, const mr_program_state *state, int primitive,
                    const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                    const uint16_t *indices16, uint32_t index_count) {
    (void)state;
    return draw_common(ctx, primitive, vertices, vertex_stride, vertex_count, indices16, index_count);
}
int mr_draw_fixed_rhw(mr_context *ctx, const mr_program_state *state, int primitive,
                      const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                      const uint16_t *indices16, uint32_t index_count) {
    (void)state;
    return draw_common(ctx, primitive, vertices, vertex_stride, vertex_count, indices16, index_count);
}
int mr_draw_program_rhw(mr_context *ctx, const mr_program_state *state, int primitive,
                        const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                        const uint16_t *indices16, uint32_t index_count) {
    (void)state;
    return draw_common(ctx, primitive, vertices, vertex_stride, vertex_count, indices16, index_count);
}
int mr_draw_fixed_clip(mr_context *ctx, const mr_program_state *state, int primitive,
                       const void *vertices, size_t vertex_stride, uint32_t vertex_count,
                       const uint16_t *indices16, uint32_t index_count) {
    (void)state;
    return draw_common(ctx, primitive, vertices, vertex_stride, vertex_count, indices16, index_count);
}

/* ---- resident buffers ---- */
void *mr_buffer_create(const void *bytes, size_t length) {
    bufhdr *b = (bufhdr *)malloc(sizeof(bufhdr) + length);
    if (!b) return NULL;
    b->len = length;
    if (bytes && length) memcpy(b + 1, bytes, length);
    return b;
}
void mr_buffer_release(void *buffer) { free(buffer); }
const void *mr_buffer_contents(const void *buffer) {
    if (!buffer) return NULL;
    return (const char *)buffer + sizeof(bufhdr);
}
size_t mr_buffer_length(const void *buffer) {
    if (!buffer) return 0;
    return ((const bufhdr *)buffer)->len;
}

/* ---- fast paths ---- */
int mr_fast_paths_enabled(void) { return atomic_load_explicit(&g_fast_paths, memory_order_relaxed); }
void mr_set_fast_paths(int enabled) { atomic_store_explicit(&g_fast_paths, enabled ? 1 : 0, memory_order_relaxed); }
void mr_draw_traffic_stats(uint64_t *resident_bytes, uint64_t *arena_vertex_bytes, uint64_t *folded_clears) {
    if (resident_bytes) *resident_bytes = atomic_load_explicit(&g_resident_bytes, memory_order_relaxed);
    if (arena_vertex_bytes) *arena_vertex_bytes = atomic_load_explicit(&g_arena_bytes, memory_order_relaxed);
    if (folded_clears) *folded_clears = atomic_load_explicit(&g_folded_clears, memory_order_relaxed);
}

/* ---- framebuffer ---- */
int mr_read_framebuffer(mr_context *ctx, void *out, size_t out_size) {
    if (!ctx || !out || out_size < (size_t)ctx->w * ctx->h * 4) { set_err("bad readback args"); return MR_ERR_ARGS; }
    memset(out, 0, (size_t)ctx->w * ctx->h * 4);
    return MR_OK;
}
int mr_write_framebuffer(mr_context *ctx, const void *in, size_t size) {
    if (!ctx || !in || size < (size_t)ctx->w * ctx->h * 4) { set_err("bad upload args"); return MR_ERR_ARGS; }
    return MR_OK;
}

/* ---- blits ---- */
int mr_blit_target_to(mr_context *ctx, void *mtl_texture) { (void)ctx; (void)mtl_texture; return MR_OK; }
int mr_fxaa_target_to(mr_context *ctx, void *mtl_texture, float subpix) {
    (void)ctx; (void)mtl_texture; (void)subpix; return MR_OK;
}
int mr_blit_texture_copy(void *src_mtl_texture, void *dst_mtl_texture) {
    (void)src_mtl_texture; (void)dst_mtl_texture; return MR_OK;
}

/* ---- commit ---- */
int mr_commit_async(void (*done)(void *arg, int ok), void *arg) {
    if (done) done(arg, 1);
    return MR_OK;
}
const char *mr_last_commit_error(void) { return g_commit_err; }
void *mr_shared_device(void) { return NULL; }

/* ---- diagnostics ---- */
uint64_t mr_triangles_drawn(const mr_context *ctx) {
    return ctx ? atomic_load_explicit((_Atomic uint64_t *)&ctx->tris, memory_order_relaxed) : 0;
}
uint32_t mr_cached_texture_count(const mr_context *ctx) { (void)ctx; return (uint32_t)atomic_load_explicit(&g_tex_count, memory_order_relaxed); }
uint64_t mr_cached_texture_bytes(const mr_context *ctx) { (void)ctx; return atomic_load_explicit(&g_tex_bytes, memory_order_relaxed); }

const char *mr_last_error(void) { return g_last_error; }
int mr_width(const mr_context *ctx) { return ctx ? ctx->w : 0; }
int mr_height(const mr_context *ctx) { return ctx ? ctx->h : 0; }
