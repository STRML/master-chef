/* Pipelines built ahead of their draws: real Metal, real MojoShader, synthetic
 * D3D shaders (no game assets). Two processes model two sessions sharing a
 * temporary pipeline cache directory.
 *
 * First session: draws build pipelines on the calling (engine) thread; the
 * vertex and fragment functions are compiled once and shared by every blend /
 * write-mask variant; a pipeline found again is not rebuilt; shaders registered
 * at creation are compiled by the workers so a later first draw compiles no
 * function; a failed shader fails the same way without being translated again;
 * pixels match a pipeline built the Build75 way (one library holding both
 * functions). Second session: the recorded pipelines are rebuilt by the
 * workers once the same shaders are created, a draw whose pipeline a worker is
 * still building waits for that pipeline only, no draw builds anything, and
 * every pixel matches the first session.
 *
 * clang -O2 -fobjc-arc -fblocks -DMOJOSHADER_NO_VERSION_INCLUDE=1 <MojoShader profile defines>
 *   -I third_party/mojoshader test_metalrenderer_pipeline_cache.m ../metalshader.c
 *   third_party/mojoshader/{mojoshader,mojoshader_common,profiles/mojoshader_profile_common,
 *   profiles/mojoshader_profile_metal}.c -framework Foundation -framework Metal
 */
#include "../metalrenderer.m"
#include <assert.h>
#include <spawn.h>
#include <sys/wait.h>
#include <mach-o/dyld.h>
extern char **environ;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)
enum { W = 32, H = 32 };

/* vs_1_1: dcl_position v0, dcl_color v1, dcl_texcoord v2; oPos = v0; oD0 = v1; oT0 = oT1 = v2 */
static const uint32_t vs[] = { 0xFFFE0101,
    0x0000001F, 0x80000000, 0x900F0000, 0x0000001F, 0x8000000A, 0x900F0001, 0x0000001F, 0x80000005, 0x900F0002,
    0x00000001, 0xC00F0000, 0x90E40000, 0x00000001, 0xD00F0000, 0x90E40001,
    0x00000001, 0xE00F0000, 0x90E40002, 0x00000001, 0xE00F0001, 0x90E40002, 0x0000FFFF };
/* ps_1_1: tex t0; mul r0, t0, v0 */
static const uint32_t ps1[] = { 0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000005, 0x800F0000, 0xB0E40000, 0x90E40000, 0x0000FFFF };
/* ps_1_1: tex t0; tex t1; mad r0, t0, t1, c0 */
static const uint32_t ps2[] = { 0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000042, 0xB00F0001,
    0x00000004, 0x800F0000, 0xB0E40000, 0xB0E40001, 0xA0E40000, 0x0000FFFF };
/* not a shader MojoShader can parse */
static const uint32_t bad[] = { 0xFFFF0101, 0x12345678, 0x0000FFFF };
static const uint8_t decl[] = { 0,0, 0,0, 2,0,0,0,  0,0, 12,0, 4,0,10,0,  0,0, 16,0, 1,0,5,0,  0xFF,0, 0,0, 17,0,0,0 };
typedef struct { float x, y, z; uint32_t color; float u, v; } Vertex;
static const Vertex quad[4] = { {-1,-1,.5f,0xff2080c0,0,0}, {1,-1,.5f,0x80c04020,1,0}, {-1,1,.5f,0x40ffffff,0,1}, {1,1,.5f,0xc0102030,1,1} };
static const uint16_t quad_indices[6] = { 0,1,2, 2,1,3 };
static float vs_constants[256][4], ps_constants[224][4];
static uint32_t textures[2];

typedef struct { const uint32_t *ps; size_t ps_bytes; int blend, mask, alpha; } Draw;
static const Draw draws[] = {
    { ps1, sizeof ps1, MR_BLEND_NONE, 15, 0 }, { ps1, sizeof ps1, MR_BLEND_SRC_ALPHA, 15, 0 },
    { ps1, sizeof ps1, MR_BLEND_ADD, 15, 0 }, { ps1, sizeof ps1, MR_BLEND_NONE, 7, 0 },
    { ps1, sizeof ps1, MR_BLEND_NONE, 15, 1 },
    { ps2, sizeof ps2, MR_BLEND_NONE, 15, 0 }, { ps2, sizeof ps2, MR_BLEND_NONE, 15, 1 } };
enum { DRAWS = sizeof draws / sizeof draws[0], PS2_FIRST = 5 };

static uint64_t key_of(const uint32_t *tokens, size_t bytes) { return hash_more(UINT64_C(1469598103934665603), tokens, bytes); }
static void state_for(const Draw *d, mr_program_state *st) {
    memset(st, 0, sizeof *st);
    st->vertex_tokens = vs; st->vertex_token_bytes = sizeof vs; st->vertex_key = key_of(vs, sizeof vs);
    st->pixel_tokens = d->ps; st->pixel_token_bytes = d->ps_bytes; st->pixel_key = key_of(d->ps, d->ps_bytes);
    st->declaration = decl; st->declaration_bytes = sizeof decl;
    st->vs_float4 = &vs_constants[0][0]; st->vs_float4_count = 256; st->ps_float4 = &ps_constants[0][0]; st->ps_float4_count = 224;
    st->blend = d->blend; st->color_write_mask = d->mask; st->cull_mode = 1; st->depth_compare = 8;
    st->alpha_test_ref = d->alpha ? 128 : -1; st->alpha_test_func = 7;
    for (int s = 0; s < 2; ++s) { st->samplers[s].texture = textures[s]; st->samplers[s].type = MR_SAMPLER_2D; st->samplers[s].min_filter = st->samplers[s].mag_filter = 2; st->samplers[s].address_u = st->samplers[s].address_v = 1; }
}
static void fixed_state(mr_program_state *st) {
    memset(st, 0, sizeof *st);
    st->color_write_mask = 15; st->alpha_test_ref = -1; st->cull_mode = 1; st->depth_compare = 8;
    st->fixed_stages[0] = (mr_fixed_stage){ .color_op = 4, .color_arg1 = 2, .color_arg2 = 0, .alpha_op = 2, .alpha_arg1 = 0, .result_arg = 1 };
    st->fixed_stages[1].color_op = 1;
    st->samplers[0].texture = textures[0]; st->samplers[0].type = MR_SAMPLER_2D; st->samplers[0].min_filter = st->samplers[0].mag_filter = 1;
}
static void render(mr_context *c, int index, uint8_t out[W * H * 4]) {
    mr_clear(c, 0xff304050); mr_clear_depth(c, 1.0f); mr_set_viewport(c, 0, 0, W, H);
    int status;
    if (index < DRAWS) { mr_program_state st; state_for(&draws[index], &st); status = mr_draw_program(c, &st, MR_TRIANGLE_LIST, quad, sizeof quad[0], 4, quad_indices, 6); }
    else {
        mr_program_state st; fixed_state(&st); mr_vertex_fixed_rhw v[4] = {{0}};
        for (int i = 0; i < 4; ++i) { v[i].x = (i & 1) ? W : 0; v[i].y = (i & 2) ? H : 0; v[i].rhw = 1; v[i].color = quad[i].color; v[i].uv[0][0] = quad[i].u; v[i].uv[0][1] = quad[i].v; }
        status = mr_draw_fixed_rhw(c, &st, MR_TRIANGLE_LIST, v, sizeof v[0], 4, quad_indices, 6);
    }
    CHECK(status == MR_OK, "draw %d failed: %d %s", index, status, mr_last_error());
    CHECK(mr_read_framebuffer(c, out, (size_t)W * H * 4) == 0, "readback %d", index);
}
static int distinct_pixels(const uint8_t *px) {
    uint32_t seen[16]; int n = 0;
    for (int i = 0; i < W * H && n < 16; ++i) { uint32_t p; memcpy(&p, px + 4 * i, 4); int found = 0; for (int k = 0; k < n; ++k) found |= seen[k] == p; if (!found) seen[n++] = p; }
    return n;
}
static mr_context *setup(void) {
    mr_context *c = mr_create(W, H); if (!c) return NULL;
    uint32_t texels[8 * 8]; uint32_t seed = 7;
    for (int t = 0; t < 2; ++t) { for (int i = 0; i < 64; ++i) { seed = seed * 1664525u + 1013904223u; texels[i] = seed | 0xff000000u; } textures[t] = mr_texture_create_cached(c, 0x7000u + t, 8, 8, texels, 32); }
    for (int i = 0; i < 256; ++i) for (int k = 0; k < 4; ++k) vs_constants[i][k] = 0.25f * (float)((i + k) % 4);
    for (int i = 0; i < 224; ++i) for (int k = 0; k < 4; ++k) ps_constants[i][k] = 0.125f * (float)((i + k) % 5);
    mr_pipeline_cache_flush();               /* the manifest is loaded */
    compiler_drain(g_shared->compiler);      /* the shared pretransformed vertex functions are compiled */
    return c;
}
static unsigned translations(mr_compiler *cc) {
    unsigned n = 0; pthread_mutex_lock(&cc->lock);
    for (unsigned b = 0; b < MR_XLAT_BUCKETS; ++b) for (mr_xlat *x = cc->xlats[b]; x; x = x->next) n++;
    pthread_mutex_unlock(&cc->lock); return n;
}
/* Build75: both functions in one library, entry points named for the pipeline. */
static id<MTLRenderPipelineState> legacy_pipeline(mr_context *c, const mr_program_state *st) {
    ms_shader v, p; int types[16]; sampler_types_of(st, types);
    CHECK(!ms_translate_shader(vs, sizeof vs, MS_STAGE_VERTEX, "mr_vs_legacy", &v), "legacy vs");
    CHECK(!ms_translate_shader_mapped(st->pixel_tokens, st->pixel_token_bytes, MS_STAGE_PIXEL, "mr_ps_legacy", types, 16, &p), "legacy ps");
    NSString *source = [NSString stringWithFormat:@"%@\n%@", [[NSString alloc] initWithBytes:v.source length:v.source_bytes encoding:NSUTF8StringEncoding],
                        [[NSString alloc] initWithBytes:p.source length:p.source_bytes encoding:NSUTF8StringEncoding]];
    NSError *error = nil; id<MTLLibrary> library = [c->s->dev newLibraryWithSource:source options:nil error:&error];
    CHECK(library, "legacy library: %s", error.localizedDescription.UTF8String);
    uint32_t streams = 0; MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [library newFunctionWithName:@"mr_vs_legacy"]; pd.fragmentFunction = [library newFunctionWithName:@"mr_ps_legacy"];
    pd.vertexDescriptor = make_vertex_descriptor(&v, st->declaration, st->declaration_bytes, sizeof(Vertex), st, &streams);
    pd.depthAttachmentPixelFormat = pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    configure_program_color(pd.colorAttachments[0], st->blend, st->blend_src, st->blend_dst, st->blend_op, st->color_write_mask, 0);
    id<MTLRenderPipelineState> pipeline = [c->s->dev newRenderPipelineStateWithDescriptor:pd error:&error];
    CHECK(pipeline, "legacy pipeline: %s", error.localizedDescription.UTF8String);
    ms_shader_destroy(&v); ms_shader_destroy(&p);
    return pipeline;
}

static int first_session(const char *dir) {
    mr_context *c = setup(); if (!c) return 77;
    mr_compiler *cc = g_shared->compiler;
    static uint8_t pixels[DRAWS + 1][W * H * 4];
    mr_pipeline_stats_t s0, s; mr_pipeline_stats(&s0);
    /* Four blend/write-mask variants of one shader pair: four pipelines, two functions. */
    for (int i = 0; i < 4; ++i) render(c, i, pixels[i]);
    mr_pipeline_stats(&s);
    CHECK(s.engine_builds - s0.engine_builds == 4 && s.engine_functions - s0.engine_functions == 2 && g_shared->program_count == 4,
          "variants built %llu pipelines, %llu functions, %u entries", s.engine_builds - s0.engine_builds, s.engine_functions - s0.engine_functions, g_shared->program_count);
    uint64_t count = 0, ns = 0; mr_program_compile_stats(&count, &ns); CHECK(count == s.engine_builds && ns == s.engine_build_ns, "compile stats");
    for (int i = 0; i < 4; ++i) CHECK(distinct_pixels(pixels[i]) > 4, "variant %d drew nothing useful", i);
    /* Found again: no build. */
    for (int i = 0; i < 4; ++i) { uint8_t again[W * H * 4]; render(c, i, again); CHECK(!memcmp(again, pixels[i], sizeof again), "redraw %d differs", i); }
    mr_pipeline_stats(&s0); CHECK(s0.engine_builds == s.engine_builds, "a cached pipeline was rebuilt");
    /* Identical pixels to the Build75 construction of the same pipeline. */
    { mr_program_state st; state_for(&draws[0], &st); mr_program_cache *e = program_lookup(g_shared, program_key(&st, sizeof(Vertex), 0));
      CHECK(e && e->state == MR_PL_READY, "lookup");
      id<MTLRenderPipelineState> current = e->pipeline; e->pipeline = legacy_pipeline(c, &st);
      uint8_t legacy[W * H * 4]; render(c, 0, legacy); e->pipeline = current;
      CHECK(!memcmp(legacy, pixels[0], sizeof legacy), "separate libraries draw differently from one library"); }
    /* The alpha-tested variant: one more pipeline and one injected fragment. */
    render(c, 4, pixels[4]); mr_pipeline_stats(&s);
    CHECK(s.engine_builds - s0.engine_builds == 1 && s.engine_functions - s0.engine_functions == 1, "alpha variant: %llu builds %llu functions",
          s.engine_builds - s0.engine_builds, s.engine_functions - s0.engine_functions);
    /* Shaders created before their first draw: the workers compile them (and the
     * alpha-test variant already in use), the draws only build pipelines. */
    mr_shader_created(0, vs, sizeof vs, key_of(vs, sizeof vs));
    mr_shader_created(1, ps2, sizeof ps2, key_of(ps2, sizeof ps2));
    compiler_drain(cc); mr_pipeline_stats(&s0);
    CHECK(s0.background_functions - s.background_functions == 2, "prewarm compiled %llu functions", s0.background_functions - s.background_functions);
    for (int i = PS2_FIRST; i < DRAWS; ++i) render(c, i, pixels[i]);
    render(c, DRAWS, pixels[DRAWS]);                         /* fixed-function stages, pretransformed */
    mr_pipeline_stats(&s);
    CHECK(s.engine_builds - s0.engine_builds == 3 && s.engine_functions - s0.engine_functions == 1,
          "after prewarm: %llu builds %llu functions (expected 3 and the fixed fragment)", s.engine_builds - s0.engine_builds, s.engine_functions - s0.engine_functions);
    /* A shader that cannot be translated fails the same way every time, once. */
    { Draw d = { bad, sizeof bad, MR_BLEND_NONE, 15, 0 }; mr_program_state st; state_for(&d, &st); char first[256];
      CHECK(mr_draw_program(c, &st, MR_TRIANGLE_LIST, quad, sizeof quad[0], 4, quad_indices, 6) == MR_ERR_SHADER, "bad shader drew");
      snprintf(first, sizeof first, "%s", mr_last_error()); unsigned before = translations(cc);
      CHECK(mr_draw_program(c, &st, MR_TRIANGLE_LIST, quad, sizeof quad[0], 4, quad_indices, 6) == MR_ERR_SHADER && !strcmp(first, mr_last_error()) && translations(cc) == before,
            "bad shader retried: %s / %s", first, mr_last_error()); }
    mr_pipeline_cache_flush();
    char path[1024]; snprintf(path, sizeof path, "%s/pixels.bin", dir);
    FILE *f = fopen(path, "wb"); CHECK(f && fwrite(pixels, sizeof pixels, 1, f) == 1 && !fclose(f), "write %s", path);
    printf("first session: %llu pipelines built on the drawing thread in %.1f ms, %llu functions there; workers compiled %llu functions\n",
           s.engine_builds, s.engine_build_ns / 1e6, s.engine_functions, s.background_functions);
    return 0;
}

static _Atomic int held;
static uint64_t hold_key;
static void hold_build(uint64_t key) { if (key == hold_key && !atomic_exchange(&held, 1)) usleep(300000); }

static int second_session(const char *dir) {
    mr_context *c = setup(); if (!c) return 77;
    mr_compiler *cc = g_shared->compiler;
    mr_program_state st; state_for(&draws[PS2_FIRST], &st);      /* after setup: textures bound */
    hold_key = program_key(&st, sizeof(Vertex), 0); mr_test_before_background_build = hold_build;
    mr_pipeline_stats_t s; mr_pipeline_stats(&s);
    CHECK(s.recipes_loaded == DRAWS + 1, "manifest held %llu recipes", s.recipes_loaded);
    /* The game creates its shaders; the workers rebuild what the last session drew. */
    mr_shader_created(0, vs, sizeof vs, key_of(vs, sizeof vs));
    mr_shader_created(1, ps1, sizeof ps1, key_of(ps1, sizeof ps1));
    mr_shader_created(1, ps2, sizeof ps2, key_of(ps2, sizeof ps2));
    for (int spin = 0; !atomic_load(&held) && spin < 2000; ++spin) usleep(5000);
    CHECK(atomic_load(&held), "no worker picked up the recorded pipeline");
    static uint8_t pixels[DRAWS + 1][W * H * 4], expected[DRAWS + 1][W * H * 4];
    render(c, PS2_FIRST, pixels[PS2_FIRST]);                 /* a worker holds it: wait for that pipeline */
    mr_pipeline_stats(&s);
    CHECK(s.waits >= 1 && s.wait_ns >= UINT64_C(100000000) && s.engine_builds == 0, "wait: %llu waits %.1f ms, %llu builds", s.waits, s.wait_ns / 1e6, s.engine_builds);
    compiler_drain(cc);
    for (int i = 0; i <= DRAWS; ++i) if (i != PS2_FIRST) render(c, i, pixels[i]);
    mr_pipeline_stats(&s);
    CHECK(s.engine_builds == 0 && s.engine_functions == 0 && s.prewarm_hits == DRAWS + 1,
          "second session: %llu builds, %llu functions, %llu prewarmed", s.engine_builds, s.engine_functions, s.prewarm_hits);
    char path[1024]; snprintf(path, sizeof path, "%s/pixels.bin", dir);
    FILE *f = fopen(path, "rb"); CHECK(f && fread(expected, sizeof expected, 1, f) == 1, "read %s", path); fclose(f);
    for (int i = 0; i <= DRAWS; ++i) CHECK(!memcmp(expected[i], pixels[i], sizeof pixels[i]), "draw %d differs between sessions", i);
    printf("second session: %llu recipes rebuilt by the workers, %llu first draws found them ready, 0 built on the drawing thread; one draw waited %.0f ms for its pipeline\n",
           s.background_pipelines, s.prewarm_hits, s.wait_ns / 1e6);
    return 0;
}

static int run_self(const char *self, const char *phase, const char *dir) {
    char *argv[] = { (char *)self, (char *)phase, (char *)dir, NULL }; pid_t pid; int status = 0;
    if (posix_spawn(&pid, self, NULL, NULL, argv, environ) || waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc == 3) {
        setenv("HALO_PIPELINE_CACHE_DIR", argv[2], 1); setenv("HALO_PIPELINE_ARCHIVE", "0", 1);
        return !strcmp(argv[1], "first") ? first_session(argv[2]) : second_session(argv[2]);
    }
    if (!MTLCreateSystemDefaultDevice()) { puts("SKIP: no Metal device"); return 0; }
    char self[4096]; uint32_t size = sizeof self; CHECK(!_NSGetExecutablePath(self, &size), "executable path");
    char dir[] = "/tmp/halo-pipeline-cache-XXXXXX"; CHECK(mkdtemp(dir), "mkdtemp");
    int first = run_self(self, "first", dir), second = first ? -1 : run_self(self, "second", dir);
    char command[256]; snprintf(command, sizeof command, "rm -rf '%s'", dir); system(command);
    if (first == 77) { puts("SKIP: no Metal device"); return 0; }
    CHECK(first == 0 && second == 0, "sessions exited %d and %d", first, second);
    puts("PASS: pipelines shared by variants, found again without rebuilding, prewarmed from shader creation, "
         "failures cached, identical to one-library pipelines; a second session rebuilds them in the background, "
         "waits only for a pipeline still building, and draws identical pixels; real Metal, synthetic shaders.");
    return 0;
} }
