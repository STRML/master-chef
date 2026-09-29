/* Differential GPU test for the draw-path shortcuts in metalrenderer.m:
 * resident vertex copies, clears folded into the next pass, and the
 * encoder state reuse.
 *
 * One seeded sequence of renderer calls runs twice on the Mac GPU, once with
 * the shortcuts and once with the original paths (mr_set_fast_paths), into
 * fresh targets each time: programmable draws (two vertex shaders, two pixel
 * shaders and the fixed-function stages, stream zero and a second stream,
 * resident copies at aligned and unaligned offsets), pretransformed and
 * clip-space draws, colour/depth/stencil clears at every point in the
 * sequence, depth and stencil testing against those clears, blending and
 * write masks, sampling one target from the other, CPU uploads, readbacks,
 * blits and FXAA into outside textures, and commits. Every readback and
 * every outside texture must match byte for byte. Targeted cases assert
 * actual colored pixels, cached target-ID sampling after clears, destination
 * aliasing, encoder restart, and releasing resident buffers before commit.
 * CPU timings exclude GPU completion. Prints SKIP without a Metal device.
 *
 * clang -O2 -fobjc-arc -fblocks tests/test_metalrenderer_fastpaths.m
 *   -framework Foundation -framework Metal -o /tmp/mr-fastpaths
 */
#include "../metalrenderer.m"
#include <assert.h>

/* The MojoShader boundary is replaced by hand-written Metal with the same
 * shape (stage_in attributes, packed float4 constants in buffer 0, user()
 * interpolants, textures and samplers at their stage index). What is under
 * test is the renderer around it, not the translation. The second token
 * selects the variant. */
static char *format_source(const char *format, const char *entry) {
    size_t n = strlen(format) + 16 * strlen(entry) + 1; char *out = malloc(n);
    char *w = out; for (const char *p = format; *p; ++p) { if (*p == '@') { strcpy(w, entry); w += strlen(entry); } else *w++ = *p; }
    *w = 0; return out;
}
int ms_translate_shader(const uint32_t *tokens, size_t bytes, int stage, const char *entry, ms_shader *out) {
    memset(out, 0, sizeof *out); assert(bytes >= 8);
    uint32_t variant = tokens[1] & 0xF;
    snprintf(out->entry, sizeof out->entry, "%s", entry); out->stage = stage;
    if (stage == MS_STAGE_VERTEX) {
        out->attribute_count = 3;
        out->attributes[0] = (ms_semantic){ 0, 0, 0 }; out->attributes[1] = (ms_semantic){ 10, 0, 1 }; out->attributes[2] = (ms_semantic){ 5, 0, 2 };
        out->output_count = 2; out->outputs[0] = (ms_semantic){ 10, 0, 0 }; out->outputs[1] = (ms_semantic){ 5, 0, 1 };
        out->uniform_count = variant ? 2 : 1;
        out->uniforms[0] = (ms_binding){ MS_UNIFORM_FLOAT4, 0, 2 };
        if (variant) out->uniforms[1] = (ms_binding){ MS_UNIFORM_FLOAT4, 5, 1 };   /* c5 packed after c0-c1 */
        out->packed_float4_count = variant ? 3 : 2;
        out->source = format_source(variant ?
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct @_in { float3 pos [[attribute(0)]]; float4 color [[attribute(1)]]; float2 uv [[attribute(2)]]; };\n"
            "struct @_out { float4 pos [[position]]; float4 color0 [[user(color0)]]; float4 texcoord0 [[user(texcoord0)]]; };\n"
            "vertex @_out @(@_in in [[stage_in]], constant float4 *c [[buffer(0)]]) {\n"
            " @_out o; o.pos = float4(in.pos.xy * c[0].xy + c[0].zw, in.pos.z, 1.0); o.color0 = in.color.bgra * c[2];\n"
            " o.texcoord0 = float4(in.uv * c[1].xy, 0.0, 1.0); return o; }\n"
          : "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct @_in { float3 pos [[attribute(0)]]; float4 color [[attribute(1)]]; float2 uv [[attribute(2)]]; };\n"
            "struct @_out { float4 pos [[position]]; float4 color0 [[user(color0)]]; float4 texcoord0 [[user(texcoord0)]]; };\n"
            "vertex @_out @(@_in in [[stage_in]], constant float4 *c [[buffer(0)]]) {\n"
            " @_out o; o.pos = float4(in.pos.xy * c[0].xy + c[0].zw, in.pos.z, 1.0); o.color0 = in.color * c[1];\n"
            " o.texcoord0 = float4(in.uv, 0.0, 1.0); return o; }\n", entry);
    } else {
        out->sampler_count = variant ? 2 : 1;
        out->samplers[0] = (ms_sampler){ MR_SAMPLER_2D, 0 };
        if (variant) out->samplers[1] = (ms_sampler){ MR_SAMPLER_2D, 1 };
        out->uniform_count = variant ? 0 : 1; out->uniforms[0] = (ms_binding){ MS_UNIFORM_FLOAT4, 0, 1 };
        out->packed_float4_count = variant ? 0 : 1;
        out->source = format_source(variant ?
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct @_in { float4 color0 [[user(color0)]]; float4 texcoord0 [[user(texcoord0)]]; };\n"
            "fragment float4 @(@_in in [[stage_in]], texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]],\n"
            " texture2d<float> t1 [[texture(1)]], sampler s1 [[sampler(1)]]) {\n"
            " return mix(t0.sample(s0, in.texcoord0.xy), t1.sample(s1, in.texcoord0.yx), 0.5) * in.color0; }\n"
          : "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct @_in { float4 color0 [[user(color0)]]; float4 texcoord0 [[user(texcoord0)]]; };\n"
            "fragment float4 @(@_in in [[stage_in]], constant float4 *c [[buffer(0)]], texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]]) {\n"
            " return in.color0 * t0.sample(s0, in.texcoord0.xy) + c[0]; }\n", entry);
    }
    out->source_bytes = strlen(out->source); return 0;
}
int ms_translate_shader_mapped(const uint32_t *tokens, size_t bytes, int stage, const char *entry, const int *types, int count, ms_shader *out) {
    (void)types; (void)count; return ms_translate_shader(tokens, bytes, stage, entry, out);
}
void ms_shader_destroy(ms_shader *shader) { free(shader->source); memset(shader, 0, sizeof *shader); }

static uint64_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }
static float unit(void) { return (float)(rnd() & 0xFFFF) / 65535.0f; }

enum { SIZE = 64, POOL_VERTS = 4096 };
static const uint32_t vs_tokens[2][3] = { { 0xFFFE0101u, 0, 0xFFFFu }, { 0xFFFE0101u, 1, 0xFFFFu } };
static const uint32_t ps_tokens[2][3] = { { 0xFFFF0101u, 0, 0xFFFFu }, { 0xFFFF0101u, 1, 0xFFFFu } };
/* D3DVERTEXELEMENT9: stream, offset, type, method, usage, index. */
static const uint8_t decl_one_stream[] = { 0,0,0,0,2,0,0,0, 0,0,12,0,4,0,10,0, 0,0,16,0,1,0,5,0, 255,0,0,0,17,0,0,0 };
static const uint8_t decl_two_streams[] = { 0,0,0,0,2,0,0,0, 0,0,12,0,4,0,10,0, 1,0,0,0,1,0,5,0, 255,0,0,0,17,0,0,0 };

/* Vertex pools: one CPU copy and one resident copy of the same bytes. */
static uint8_t *pool_cpu[2]; static void *pool_resident[2]; static size_t pool_bytes[2];
static void make_pools(void) {
    const size_t strides[2] = { 24, 8 };
    for (int p = 0; p < 2; ++p) {
        pool_bytes[p] = POOL_VERTS * strides[p] + 64; pool_cpu[p] = calloc(1, pool_bytes[p]);
        for (size_t v = 0; v < POOL_VERTS; ++v) {
            uint8_t *e = pool_cpu[p] + v * strides[p];
            if (p == 0) {
                float xyz[3] = { unit() * 2.4f - 1.2f, unit() * 2.4f - 1.2f, unit() };
                uint32_t color = rnd() | 0x20000000u;
                float uv[2] = { unit() * 2.f - .5f, unit() * 2.f - .5f };
                memcpy(e, xyz, 12); memcpy(e + 12, &color, 4); memcpy(e + 16, uv, 8);
            } else { float uv[2] = { unit() * 3.f - 1.f, unit() * 3.f - 1.f }; memcpy(e, uv, 8); }
        }
        pool_resident[p] = mr_buffer_create(pool_cpu[p], pool_bytes[p]); assert(pool_resident[p]);
    }
}

typedef struct { uint8_t *bytes[64]; size_t sizes[64]; unsigned count; } Record;
static void record(Record *r, const void *bytes, size_t size) { assert(r->count < 64); r->bytes[r->count] = malloc(size); memcpy(r->bytes[r->count], bytes, size); r->sizes[r->count++] = size; }

static id<MTLTexture> outside_texture(id<MTLDevice> device, int render_target) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:SIZE height:SIZE mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | (render_target ? MTLTextureUsageRenderTarget : 0); d.storageMode = MTLStorageModeShared;
    return [device newTextureWithDescriptor:d];
}
static void read_texture(Record *r, id<MTLTexture> t) {
    uint8_t pixels[SIZE * SIZE * 4];
    [t getBytes:pixels bytesPerRow:SIZE * 4 fromRegion:MTLRegionMake2D(0, 0, SIZE, SIZE) mipmapLevel:0]; record(r, pixels, sizeof pixels);
}

static unsigned draws_done, draws_resident_capable;
static void random_depth_stencil(int *enable, int *write, int *compare) {
    const int compares[] = { 4, 5, 8 };
    *enable = below(3) != 0; *write = below(2); *compare = compares[below(3)];
}
static void random_program_draw(mr_context *target, mr_context *other, uint32_t texture) {
    mr_program_state s = { 0 };
    int vs = below(2), two = below(2), shader_kind = below(3);   /* 0,1 pixel shaders, 2 fixed stages */
    s.vertex_tokens = vs_tokens[vs]; s.vertex_token_bytes = sizeof vs_tokens[vs]; s.vertex_key = 0x1000 + vs;
    s.declaration = two ? decl_two_streams : decl_one_stream; s.declaration_bytes = two ? sizeof decl_two_streams : sizeof decl_one_stream;
    size_t stride = two ? 24 : 24;   /* stream zero keeps the pool's 24-byte layout either way */
    float vs_constants[8][4];
    for (int r = 0; r < 8; ++r) for (int k = 0; k < 4; ++k) vs_constants[r][k] = unit() * 1.5f - .25f;
    vs_constants[0][0] = .6f + unit(); vs_constants[0][1] = .6f + unit(); vs_constants[0][2] = unit() * .4f - .2f; vs_constants[0][3] = unit() * .4f - .2f;
    s.vs_float4 = &vs_constants[0][0]; s.vs_float4_count = 8;
    float ps_constants[4][4]; for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) ps_constants[r][k] = unit() * .3f;
    s.ps_float4 = &ps_constants[0][0]; s.ps_float4_count = 4;
    if (shader_kind < 2) {
        s.pixel_tokens = ps_tokens[shader_kind]; s.pixel_token_bytes = sizeof ps_tokens[shader_kind]; s.pixel_key = 0x2000 + shader_kind;
        s.samplers[0] = (mr_program_sampler){ .texture = texture, .type = MR_SAMPLER_2D, .address_u = 1 + 2 * below(2), .min_filter = 1 + below(2), .mag_filter = 1 + below(2), .mip_filter = below(3) };
        s.samplers[0].address_v = s.samplers[0].address_u;
        if (shader_kind == 1) s.samplers[1] = (mr_program_sampler){ .texture = mr_target_texture(other), .type = MR_SAMPLER_2D, .address_u = 3, .address_v = 3, .min_filter = 1, .mag_filter = 1 };
        s.alpha_test_ref = -1;
    } else {
        s.fixed_stages[0] = (mr_fixed_stage){ .color_op = 4, .color_arg1 = 2, .color_arg2 = 0, .alpha_op = below(2) ? 2 : 4, .alpha_arg1 = 0, .alpha_arg2 = 2, .result_arg = 1 };
        s.fixed_stages[1].color_op = 1;
        s.samplers[0] = (mr_program_sampler){ .texture = texture, .type = MR_SAMPLER_2D, .address_u = 1, .address_v = 1, .min_filter = 2, .mag_filter = 2, .mip_filter = 2 };
        s.alpha_test_ref = below(3) ? -1 : (int)below(200); s.alpha_test_func = 7;
        s.texture_factor = rnd();
    }
    /* Choices are kept few enough for the pipeline and depth/stencil caches. */
    const int blends[] = { MR_BLEND_NONE, MR_BLEND_SRC_ALPHA, MR_BLEND_ADD, MR_BLEND_CUSTOM };
    s.blend = blends[below(4)]; s.blend_src = 5; s.blend_dst = 2; s.blend_op = 3;
    s.color_write_mask = 15;
    random_depth_stencil(&s.depth_enable, &s.depth_write, &s.depth_compare);
    s.cull_mode = 1 + below(3);
    s.stencil_enable = !below(4); s.stencil_fail = 1; s.stencil_depth_fail = 1; s.stencil_pass = below(2) ? 3 : 7;
    s.stencil_compare = below(2) ? 8 : 3; s.stencil_ref = below(4); s.stencil_read_mask = 255; s.stencil_write_mask = 255;
    if (!s.stencil_enable) s.stencil_fail = s.stencil_depth_fail = s.stencil_pass = s.stencil_compare = s.stencil_read_mask = s.stencil_write_mask = 0;
    uint32_t nv = 3 + below(300), first = below(POOL_VERTS - nv);
    size_t offset = (size_t)first * stride + (below(6) ? 0 : 2);   /* sometimes unaligned: must fall back to the copy */
    const uint8_t *vertices = pool_cpu[0] + offset;
    s.resident[0] = pool_resident[0]; s.resident_offset[0] = offset;
    if (two) {
        size_t uv_offset = (size_t)below(POOL_VERTS - nv) * 8 + (below(8) ? 0 : 4);
        s.stream_bytes[1] = pool_cpu[1] + uv_offset; s.stream_stride[1] = 8; s.stream_length[1] = (size_t)nv * 8;
        s.resident[1] = pool_resident[1]; s.resident_offset[1] = uv_offset;
    }
    uint32_t ni = below(4) ? 3 * (1 + below(200)) : 0; uint16_t indices[600];
    for (uint32_t i = 0; i < ni; ++i) indices[i] = (uint16_t)below(nv);
    int prim = below(5) ? MR_TRIANGLE_LIST : MR_TRIANGLE_STRIP;
    mr_set_viewport(target, below(16), below(16), SIZE - below(16), SIZE - below(16));
    int status = mr_draw_program(target, &s, prim, vertices, stride, nv, ni ? indices : NULL, ni);
    if (status) { fprintf(stderr, "draw failed %d: %s\n", status, mr_last_error()); abort(); }
    draws_done++; draws_resident_capable += !(offset & 3);
}
static void random_rhw_draw(mr_context *target, uint32_t texture) {
    mr_vertex_rhw v[6]; for (int i = 0; i < 6; ++i) v[i] = (mr_vertex_rhw){ unit() * SIZE, unit() * SIZE, unit(), 1.f, rnd(), unit(), unit() };
    mr_draw_state st = { .texture = below(2) ? texture : 0, .blend = below(3), .linear_filter = below(2), .alpha_test_ref = -1, .color_write_mask = below(2) ? 15 : 7, .cull_mode = below(4) };
    random_depth_stencil(&st.depth_enable, &st.depth_write, &st.depth_compare);
    mr_set_viewport(target, 0, 0, SIZE, SIZE);
    assert(mr_draw_rhw(target, &st, MR_TRIANGLE_LIST, v, sizeof v[0], 6, NULL, 0) == MR_OK);
}
static void random_clip_draw(mr_context *target, uint32_t texture) {
    mr_vertex_fixed_clip v[3]; memset(v, 0, sizeof v);
    for (int i = 0; i < 3; ++i) { v[i].x = unit() * 2 - 1; v[i].y = unit() * 2 - 1; v[i].z = unit(); v[i].w = 1; v[i].color = rnd(); v[i].uv[0][0] = unit(); v[i].uv[0][1] = unit(); }
    mr_program_state s = { 0 }; s.color_write_mask = 15; s.alpha_test_ref = -1; s.cull_mode = 1; s.depth_enable = below(2); s.depth_compare = 4; s.depth_write = 1;
    s.fixed_stages[0] = (mr_fixed_stage){ .color_op = 4, .color_arg1 = 2, .color_arg2 = 0, .alpha_op = 2, .alpha_arg1 = 0, .result_arg = 1 };
    s.fixed_stages[1].color_op = 1;
    s.samplers[0] = (mr_program_sampler){ .texture = texture, .type = MR_SAMPLER_2D, .address_u = 1, .address_v = 1, .min_filter = 2, .mag_filter = 2 };
    mr_set_viewport(target, 0, 0, SIZE, SIZE);
    assert(mr_draw_fixed_clip(target, &s, MR_TRIANGLE_LIST, v, sizeof v[0], 3, NULL, 0) == MR_OK);
}

static void run_sequence(uint64_t seed, id<MTLDevice> device, Record *out, uint32_t texture) {
    rng = seed;
    mr_context *c[2] = { mr_create(SIZE, SIZE), mr_create(SIZE, SIZE) }; assert(c[0] && c[1]);
    id<MTLTexture> blit_target = outside_texture(device, 0), fxaa_target = outside_texture(device, 1);
    uint8_t pixels[SIZE * SIZE * 4];
    for (unsigned step = 0; step < 900; ++step) {
        int t = below(2); mr_context *target = c[t], *other = c[!t];
        switch (below(24)) {
        case 0: case 1: mr_clear(target, rnd()); break;
        case 2: mr_clear_depth(target, below(4) ? unit() : (float)below(2)); break;
        case 3: mr_clear_stencil(target, rnd()); break;
        case 4: mr_clear(target, rnd()); mr_clear_depth(target, unit()); mr_clear_stencil(target, rnd()); break;
        case 5: random_rhw_draw(target, texture); break;
        case 6: random_clip_draw(target, texture); break;
        case 7: if (!below(4)) { assert(mr_read_framebuffer(target, pixels, sizeof pixels) == 0); record(out, pixels, sizeof pixels); } break;
        case 8: if (!below(6)) { for (size_t i = 0; i < sizeof pixels; ++i) pixels[i] = (uint8_t)rnd(); assert(mr_write_framebuffer(target, pixels, sizeof pixels) == 0); } break;
        case 9: assert(mr_blit_target_to(target, (__bridge void *)blit_target) == MR_OK); break;
        case 10: assert(mr_fxaa_target_to(target, (__bridge void *)fxaa_target, .5f) == MR_OK); break;
        case 11: if (!below(3)) mr_commit_async(NULL, NULL); break;
        default: random_program_draw(target, other, texture); break;
        }
    }
    for (int i = 0; i < 2; ++i) { assert(mr_read_framebuffer(c[i], pixels, sizeof pixels) == 0); record(out, pixels, sizeof pixels); }
    read_texture(out, blit_target); read_texture(out, fxaa_target);
    mr_destroy(c[0]); mr_destroy(c[1]);
}

static void expect_pixel(mr_context *c, unsigned x, unsigned y, uint32_t expected) {
    uint32_t pixels[SIZE * SIZE];
    assert(mr_read_framebuffer(c, pixels, sizeof pixels) == 0);
    if (pixels[y * SIZE + x] != expected) {
        fprintf(stderr, "pixel (%u,%u): got %08x expected %08x\n", x, y, pixels[y * SIZE + x], expected);
        abort();
    }
}

/* A full-screen triangle samples the previously acquired texture ID, without
 * mr_target_texture() forcing deferred clears to be consumed for the test. */
static void draw_sample(mr_context *c, uint32_t texture) {
    mr_vertex_rhw v[3] = {
        { 0, 0, .5f, 1, 0xFFFFFFFFu, 0, 0 },
        { SIZE * 2, 0, .5f, 1, 0xFFFFFFFFu, 1, 0 },
        { 0, SIZE * 2, .5f, 1, 0xFFFFFFFFu, 0, 1 }
    };
    mr_draw_state s = { .texture = texture, .color_write_mask = 15, .alpha_test_ref = -1, .cull_mode = 1 };
    assert(mr_draw_rhw(c, &s, MR_TRIANGLE_LIST, v, sizeof v[0], 3, NULL, 0) == MR_OK);
}

/* A real visible programmable triangle. Every call creates a new immutable
 * snapshot then drops its owner before GPU submission. A later draw must not
 * overwrite bytes retained by the preceding one. Misaligned/out-of-range
 * hints exercise the arena fallback with valid input vertices. */
static void draw_color(mr_context *c, uint32_t color, unsigned hint) {
    struct TestVertex { float x, y, z; uint32_t color; float u, v; };
    struct TestVertex v[3] = { { -1, -1, .5f, color, 0, 0 }, { 3, -1, .5f, color, 0, 0 }, { -1, 3, .5f, color, 0, 0 } };
    uint8_t bytes[sizeof v + 4]; memset(bytes, 0, sizeof bytes);
    size_t offset = hint == 2 ? 2 : 4;
    memcpy(bytes + offset, v, sizeof v);
    void *resident = mr_buffer_create(bytes, hint == 3 ? 4 : sizeof bytes); assert(resident);
    assert(mr_buffer_length(resident) == (hint == 3 ? 4 : sizeof bytes));
    assert(!memcmp(mr_buffer_contents(resident), bytes, mr_buffer_length(resident)));
    const float constants[2][4] = { { 1, 1, 0, 0 }, { 1, 1, 1, 1 } };
    mr_program_state s = {0};
    s.vertex_tokens = vs_tokens[0]; s.vertex_token_bytes = sizeof vs_tokens[0]; s.vertex_key = 0x1000;
    s.declaration = decl_one_stream; s.declaration_bytes = sizeof decl_one_stream;
    s.vs_float4 = &constants[0][0]; s.vs_float4_count = 2;
    s.fixed_stages[0] = (mr_fixed_stage){ .color_op = 2, .color_arg1 = 0, .alpha_op = 2, .alpha_arg1 = 0, .result_arg = 1 };
    s.fixed_stages[1].color_op = 1;
    s.color_write_mask = 15; s.alpha_test_ref = -1; s.cull_mode = 1;
    s.resident[0] = resident; s.resident_offset[0] = hint == 4 ? SIZE_MAX - 3 : offset;
    assert(mr_draw_program(c, &s, MR_TRIANGLE_LIST, bytes + offset, sizeof v[0], 3, NULL, 0) == MR_OK);
    mr_buffer_release(resident);
    /* Mutating the original CPU bytes must not affect the queued snapshot. */
    memset(bytes, 0, sizeof bytes);
}

static void targeted_regressions(int fast) {
    mr_set_fast_paths(fast);
    mr_context *src = mr_create(SIZE, SIZE), *dst = mr_create(SIZE, SIZE); assert(src && dst);
    uint32_t old_id = mr_target_texture(src);
    draw_sample(dst, old_id);                  /* leave dst's encoder open */
    mr_clear(src, 0xFFFF0000u);
    draw_sample(dst, old_id);                  /* use cached ID after clear */
    expect_pixel(dst, 20, 20, 0xFFFF0000u);
    mr_clear(dst, 0xFF0000FFu);
    assert(mr_blit_target_to(src, (__bridge void *)dst->target) == MR_OK);
    expect_pixel(dst, 20, 20, 0xFFFF0000u);     /* dst clear must precede blit */
    mr_clear(dst, 0xFF0000FFu);
    assert(mr_fxaa_target_to(src, (__bridge void *)dst->target, .5f) == MR_OK);
    expect_pixel(dst, 20, 20, 0xFFFF0000u);     /* likewise the FXAA write */
    mr_clear(src, 0xFF00FF00u); mr_clear(dst, 0xFF0000FFu);
    assert(mr_blit_texture_copy((__bridge void *)src->target, (__bridge void *)dst->target) == MR_OK);
    expect_pixel(dst, 20, 20, 0xFF00FF00u);

    mr_clear(src, 0xFFFFFF00u); mr_set_fast_paths(!fast);
    draw_sample(dst, old_id); mr_set_fast_paths(fast); draw_sample(dst, old_id);
    expect_pixel(dst, 20, 20, 0xFFFFFF00u);     /* toggle with pending work */
    mr_clear(src, 0xFF0000FFu); assert(mr_resize(src, SIZE, SIZE) == 0);
    mr_clear(src, 0xFFFF00FFu); draw_sample(dst, old_id);
    expect_pixel(dst, 20, 20, 0xFFFF00FFu);     /* ID reused for new texture */

    for (unsigned hint = 1; hint <= 4; ++hint) {
        uint64_t resident_before, copied_before, resident_after, copied_after;
        mr_draw_traffic_stats(&resident_before, &copied_before, NULL);
        mr_clear(dst, 0xFF000000u);
        mr_set_viewport(dst, 0, 0, SIZE / 2, SIZE); draw_color(dst, 0xFFFF0000u, hint);
        mr_set_viewport(dst, SIZE / 2, 0, SIZE / 2, SIZE); draw_color(dst, 0xFF0000FFu, hint);
        mr_draw_traffic_stats(&resident_after, &copied_after, NULL);
        assert(resident_after - resident_before == (fast && hint == 1 ? 144 : 0));
        assert(copied_after - copied_before == (fast && hint == 1 ? 0 : 144));
        expect_pixel(dst, 12, 20, 0xFFFF0000u);
        expect_pixel(dst, 52, 20, 0xFF0000FFu);
    }
    /* Reusing a numeric texture slot must bind the replacement Metal object;
     * the preceding queued draw still owns the destroyed slot's old object. */
    uint32_t red = 0xFFFF0000u, blue = 0xFF0000FFu;
    uint32_t texture = mr_texture_create(dst, 1, 1, &red, 4); assert(texture);
    mr_set_viewport(dst, 0, 0, SIZE / 2, SIZE); draw_sample(dst, texture);
    mr_texture_destroy(dst, texture);
    uint32_t replacement = mr_texture_create(dst, 1, 1, &blue, 4); assert(replacement == texture);
    mr_set_viewport(dst, SIZE / 2, 0, SIZE / 2, SIZE); draw_sample(dst, replacement);
    expect_pixel(dst, 12, 20, red); expect_pixel(dst, 52, 20, blue);
    mr_texture_destroy(dst, replacement);
    mr_clear(src, 0xFF112233u); mr_destroy(src);
    mr_clear(dst, 0xFF445566u); mr_destroy(dst);
    assert(!g_shared->clear_list && !g_shared->pending_encoder);
    assert(!g_shared->bound.pipeline && !g_shared->bound.textures[0]);
}

static double now_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e3; }
/* CPU time to encode typical static-geometry draws: 900 vertices of 24
 * bytes (21.6 KB, the size range the b30 profile traced) and 1200 indices,
 * cycling through twelve pipelines, 50 draws per pass like a bearing, the
 * pass's publish reading the target. GPU execution is excluded. `resident`
 * 0 measures the other shortcuts alone. Median of nine runs, us per draw. */
static int compare_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double time_draws(int fast, int resident, uint32_t texture, int clears_per_pass) {
    mr_set_fast_paths(fast);
    mr_context *c = mr_create(SIZE, SIZE); assert(c);
    mr_program_state states[12];
    float constants[8][4] = { { 1, 1, 0, 0 }, { 1, 1, 1, 1 } };
    for (int k = 0; k < 12; ++k) {
        mr_program_state *s = &states[k]; memset(s, 0, sizeof *s);
        s->vertex_tokens = vs_tokens[k & 1]; s->vertex_token_bytes = sizeof vs_tokens[0]; s->vertex_key = 0x1000 + (k & 1);
        s->pixel_tokens = ps_tokens[0]; s->pixel_token_bytes = sizeof ps_tokens[0]; s->pixel_key = 0x2000;
        s->declaration = decl_one_stream; s->declaration_bytes = sizeof decl_one_stream;
        s->vs_float4 = &constants[0][0]; s->vs_float4_count = 8; s->ps_float4 = &constants[0][0]; s->ps_float4_count = 8;
        s->samplers[0] = (mr_program_sampler){ .texture = texture, .type = MR_SAMPLER_2D, .address_u = 1, .address_v = 1, .min_filter = 2, .mag_filter = 2, .mip_filter = 2 };
        s->alpha_test_ref = -1; s->color_write_mask = 15; s->depth_enable = 1; s->depth_write = 1; s->depth_compare = 4; s->cull_mode = 1 + (k % 3);
        s->blend = (k / 2) % 3 == 2 ? MR_BLEND_CUSTOM : (k / 2) % 3; s->blend_src = 5; s->blend_dst = 2; s->blend_op = 3;
        s->resident[0] = resident ? pool_resident[0] : NULL;
    }
    uint16_t indices[1200]; for (int i = 0; i < 1200; ++i) indices[i] = (uint16_t)((i * 7) % 900);
    mr_set_viewport(c, 0, 0, SIZE, SIZE);
    double runs[9];
    for (int rep = 0; rep < 9; ++rep) {
        double start = now_us();
        for (int pass = 0; pass < 40; ++pass) {
            if (clears_per_pass) { mr_clear(c, 0xFF000000u); mr_clear_depth(c, 1.f); }
            for (int d = 0; d < 50; ++d) {
                mr_program_state *s = &states[(d / 4) % 12];   /* runs of four draws per pipeline */
                size_t first = (size_t)((pass * 50 + d) % 4) * 900;
                s->resident_offset[0] = first * 24;
                assert(mr_draw_program(c, s, MR_TRIANGLE_LIST, pool_cpu[0] + first * 24, 24, 900, indices, 1200) == MR_OK);
            }
            (void)mr_target_texture(c);
        }
        runs[rep] = (now_us() - start) / 2000.0;
        uint8_t sink[SIZE * SIZE * 4]; assert(mr_read_framebuffer(c, sink, sizeof sink) == 0);
    }
    mr_destroy(c);
    qsort(runs, 9, sizeof runs[0], compare_double);
    return runs[4];
}
int main(void) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { puts("SKIP: no Metal device"); return 0; }
    /* Never populate the caller's real pipeline manifest with test shaders. */
    setenv("HALO_PIPELINE_CACHE", "0", 1); setenv("HALO_PIPELINE_WORKERS", "0", 1);
    unsetenv("HALO_DRAW_FASTPATH"); assert(!mr_fast_paths_enabled());
    /* Diagnostics can cache the unset default before the engine worker starts. */
    setenv("HALO_DRAW_FASTPATH", "1", 1);
    assert(!mr_fast_paths_enabled());
    const char *startup_fast = getenv("HALO_DRAW_FASTPATH");
    mr_set_fast_paths(startup_fast && !strcmp(startup_fast, "1"));
    assert(mr_fast_paths_enabled());
    setenv("HALO_DRAW_FASTPATH", "0", 1);
    startup_fast = getenv("HALO_DRAW_FASTPATH");
    mr_set_fast_paths(startup_fast && !strcmp(startup_fast, "1"));
    assert(!mr_fast_paths_enabled());
    unsetenv("HALO_DRAW_FASTPATH");
    puts("startup: an early diagnostics query cannot defeat explicit engine-thread configuration; opt-out preserved");
    rng = UINT64_C(0x732819A59); make_pools();
    mr_context *scratch = mr_create(SIZE, SIZE); assert(scratch);
    uint8_t texels[16 * 16 * 4]; rng = 7; for (size_t i = 0; i < sizeof texels; ++i) texels[i] = (uint8_t)rnd();
    uint32_t texture = mr_texture_create_cached(scratch, 0x5151, 16, 16, texels, 16 * 4); assert(texture);

    targeted_regressions(0); targeted_regressions(1);
    puts("targeted: 32 exact-color pixel checks; cached target IDs, blit/FXAA aliases, toggles, resize, texture slot reuse, immutable resident release and range fallbacks pass");

    /* 2. The same sequence with and without the shortcuts, byte for byte. */
    unsigned sequences = 0, compared = 0; uint64_t folded_before = 0, folded_after = 0, resident_before = 0, resident_after = 0;
    for (uint64_t seed = 1; seed <= 6; ++seed) {
        Record fast = { 0 }, slow = { 0 };
        mr_draw_traffic_stats(&resident_before, NULL, &folded_before);
        mr_set_fast_paths(1); run_sequence(seed * 0x9E3779B97F4A7C15ull, device, &fast, texture);
        mr_draw_traffic_stats(&resident_after, NULL, &folded_after);
        assert(folded_after > folded_before && resident_after > resident_before);
        uint64_t resident_slow = resident_after;
        mr_set_fast_paths(0); run_sequence(seed * 0x9E3779B97F4A7C15ull, device, &slow, texture);
        mr_draw_traffic_stats(&resident_after, NULL, &folded_before);
        assert(resident_after == resident_slow && folded_before == folded_after);   /* the original paths neither bind nor fold */
        assert(fast.count == slow.count && fast.count >= 4);
        for (unsigned i = 0; i < fast.count; ++i) {
            assert(fast.sizes[i] == slow.sizes[i]);
            if (memcmp(fast.bytes[i], slow.bytes[i], fast.sizes[i])) {
                size_t k = 0; while (fast.bytes[i][k] == slow.bytes[i][k]) ++k;
                fprintf(stderr, "seed %llu image %u differs at byte %zu: %u vs %u\n", (unsigned long long)seed, i, k, fast.bytes[i][k], slow.bytes[i][k]); abort();
            }
            free(fast.bytes[i]); free(slow.bytes[i]); compared++;
        }
        sequences++;
    }
    mr_set_fast_paths(1);
    printf("differential: %u sequences, %u draws (%u at resident-capable offsets), %u images identical byte for byte\n", sequences, draws_done, draws_resident_capable, compared);

    /* 3. Timing on this Mac. */
    double slow_draw = time_draws(0, 1, texture, 1), other_draw = time_draws(1, 0, texture, 1), fast_draw = time_draws(1, 1, texture, 1);
    printf("Metal-only CPU timing, us per draw (900 x 24 B vertices, 1200 indices, 12 pipelines, two clears per 50-draw pass; excludes D3D resident verification and GPU completion): reference %.2f, shortcuts without resident copies %.2f, all shortcuts %.2f\n",
           slow_draw, other_draw, fast_draw);

    mr_destroy(scratch);
    mr_buffer_release(pool_resident[0]); mr_buffer_release(pool_resident[1]);
    puts("PASS: resident copies, folded clears and encoder state reuse give byte-identical GPU output to the reference paths.");
    return 0;
} }
