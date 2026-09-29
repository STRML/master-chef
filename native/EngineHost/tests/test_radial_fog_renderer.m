/* Production renderer + real MojoShader, synthetic shader tokens only.
 * Default runs use fake Metal compiler objects and do not touch a GPU.
 * Pass --gpu to additionally draw/read back on the local Metal device.
 * HALO_RADIAL_FOG_MSL_DIR optionally saves emitted sources for xrOS metal -c.
 * Link metalshader.c and MojoShader's core/common/Metal-profile C files with
 * the same profile defines as test_radial_fog.c, Foundation and Metal.
 */
#include "../metalrenderer.m"
#include <assert.h>
#include <math.h>

static NSMutableArray<NSString *> *sources;

@interface FogTestFunction : NSObject
@property MTLFunctionType functionType;
@end
@implementation FogTestFunction
@end
@interface FogTestLibrary : NSObject
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
@end
@implementation FogTestLibrary
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    FogTestFunction *function = [FogTestFunction new];
    function.functionType = [name hasPrefix:@"mr_vs_"] || [name hasPrefix:@"mr_ffvs_"] ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
    return (id<MTLFunction>)function;
}
@end
@interface FogTestDevice : NSObject
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error;
@end
@implementation FogTestDevice
- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error {
    (void)options; (void)error;
    [sources addObject:source];
    return (id<MTLLibrary>)[FogTestLibrary new];
}
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)descriptor error:(NSError **)error {
    (void)descriptor; (void)error;
    return (id<MTLRenderPipelineState>)[NSObject new];
}
@end

/* v0 is clip position; v1 is the world point. oFog is transmittance:
 * 1 - dot(world, c6), exactly the use that changes across panorama bearings. */
static const uint32_t vertex_tokens[] = {
    0xfffe0101,
    1, 0xc00f0000, 0x90e40000,              /* mov oPos, v0 */
    1, 0xd00f0000, 0xa0e40005,              /* mov oD0, c5 */
    9, 0x80010000, 0x90e40001, 0xa0e40006,  /* dp4 r0.x, v1, c6 */
    3, 0xc0010001, 0xa000000a, 0x80000000,  /* sub oFog.x, c10.x, r0.x */
    0xffff
};
static const uint32_t pixel_tokens[] = {
    0xffff0101, 1, 0x800f0000, 0x90e40000, 0xffff /* mov r0, v0 */
};
static const uint32_t constant_pixel_tokens[] = {
    0xffff0101, 1, 0x800f0000, 0xa0e40000, 0xffff /* mov r0, c0; no interpolants */
};
static const uint32_t literal_pixel_tokens[] = {
    0xffff0101,
    81, 0xa00f0000, 0x3f800000, 0, 0, 0x3f008081, /* def c0, 1,0,0,128/255 */
    1, 0x800f0000, 0xa0e40000, 0xffff /* no uniforms or interpolants */
};
static uint8_t declaration[24];
static float constants[256][4];
typedef struct { float position[4], world[4]; } FogVertex;

static mr_program_state base_state(int pixel) {
    mr_program_state s = {0};
    s.vertex_tokens = vertex_tokens; s.vertex_token_bytes = sizeof vertex_tokens;
    s.declaration = declaration; s.declaration_bytes = sizeof declaration;
    if (pixel) {
        s.pixel_tokens = pixel == 3 ? literal_pixel_tokens : pixel == 2 ? constant_pixel_tokens : pixel_tokens;
        s.pixel_token_bytes = pixel == 3 ? sizeof literal_pixel_tokens : sizeof pixel_tokens;
        s.ps_float4 = constants[5]; s.ps_float4_count = 1;
    }
    s.vs_float4 = &constants[0][0]; s.vs_float4_count = 256;
    s.color_write_mask = 15; s.cull_mode = 1; s.alpha_test_ref = -1;
    s.fog_enable = 1; s.fog_color = 0xff0000ff;
    s.fixed_stages[0].color_op = 1;
    return s;
}

static void prepare(void) {
    ms_shader vs = {0};
    assert(ms_translate_shader(vertex_tokens, sizeof vertex_tokens, MS_STAGE_VERTEX, "fixture", &vs) == 0);
    assert(vs.attribute_count == 2);
    for (int i = 0; i < 2; ++i) {
        const ms_semantic *a = &vs.attributes[i];
        assert(a->location == 0 || a->location == 1);
        declaration[i * 8 + 2] = a->location * 16;
        declaration[i * 8 + 4] = 3; /* FLOAT4 */
        declaration[i * 8 + 6] = a->usage;
        declaration[i * 8 + 7] = a->index;
    }
    declaration[16] = 255; declaration[20] = 17;
    ms_shader_destroy(&vs);
    constants[4][3] = 2.f;
    constants[5][0] = 1.f; constants[5][3] = 128.f / 255.f;
    constants[6][0] = 0.1f;
    constants[10][0] = 1.f;
}

static void check_fallback(mr_context *context) {
    /* These are legal shaders that the conservative rewriter cannot handle.
     * The compiler must still use their original translated source. */
    uint32_t occupied[64], matrix[64], modified[sizeof vertex_tokens / sizeof *vertex_tokens];
    size_t count = sizeof vertex_tokens / sizeof *vertex_tokens;
    memcpy(occupied, vertex_tokens, (count - 1) * sizeof *occupied);
    size_t at = count - 1;
    for (unsigned r = 1; r < 12; ++r) {
        occupied[at++] = 1;
        occupied[at++] = halo_rf_dst(0, r, 15);
        occupied[at++] = halo_rf_src(1, 0, 0xe4, 0);
    }
    occupied[at++] = 0xffff;
    memcpy(modified, vertex_tokens, sizeof modified);
    modified[10] |= 0x01000000u; /* dp4 r0.x, v1, -c6 is not a recognized fog plane. */
    memcpy(matrix, vertex_tokens, (count - 1) * sizeof *matrix);
    size_t matrix_at = count - 1;
    matrix[matrix_at++] = 20; /* m4x4 r1, v0, c10: implicit constant span. */
    matrix[matrix_at++] = halo_rf_dst(0, 1, 15);
    matrix[matrix_at++] = halo_rf_src(1, 0, 0xe4, 0);
    matrix[matrix_at++] = halo_rf_src(2, 10, 0xe4, 0);
    matrix[matrix_at++] = 0xffff;
    const uint32_t *tokens[] = {occupied, modified, matrix};
    const size_t bytes[] = {at * sizeof *occupied, sizeof modified, matrix_at * sizeof *matrix};
    for (unsigned shape = 0; shape < 3; ++shape) {
        mr_program_state state = base_state(1);
        state.vertex_tokens = tokens[shape]; state.vertex_token_bytes = bytes[shape];
        mr_program_cache *entries[3];
        for (int mode = 0; mode <= HALO_RADIAL_FOG_VIEW_PLANE; ++mode) {
            assert(halo_radial_fog_terms(tokens[shape], bytes[shape], mode) == 0);
            state.radial_fog = mode;
            entries[mode] = program_for(context, &state, sizeof(FogVertex));
            assert(entries[mode]);
            ms_shader reference = {0};
            assert(ms_translate_shader(tokens[shape], bytes[shape], MS_STAGE_VERTEX, entries[mode]->vs->entry, &reference) == 0);
            assert(reference.source_bytes == entries[mode]->vs->source_bytes);
            assert(!memcmp(reference.source, entries[mode]->vs->source, reference.source_bytes));
            ms_shader_destroy(&reference);
            assert(entries[mode] == program_for(context, &state, sizeof(FogVertex)));
        }
        assert(entries[0] != entries[1] && entries[1] != entries[2] && entries[0] != entries[2]);
    }
}

static void check_recipes(mr_shared *shared) {
    mr_compiler *compiler = shared->compiler;
    const uint32_t *tokens[] = {vertex_tokens, pixel_tokens};
    const size_t bytes[] = {sizeof vertex_tokens, sizeof pixel_tokens};
    const int stages[] = {MS_STAGE_VERTEX, MS_STAGE_PIXEL};
    const uint64_t keys[] = {0x123, 0x456};
    for (unsigned i = 0; i < 2; ++i) {
        mr_registered *shader = calloc(1, sizeof *shader + bytes[i]); assert(shader);
        shader->key = keys[i]; shader->stage = stages[i]; shader->bytes = bytes[i];
        memcpy(shader->tokens, tokens[i], bytes[i]);
        unsigned bucket = shader->key & (MR_REG_BUCKETS - 1u);
        shader->next = compiler->registered[bucket]; compiler->registered[bucket] = shader;
    }
    compiler->recipes = calloc(6, sizeof *compiler->recipes); assert(compiler->recipes);
    compiler->recipe_queued = calloc(6, sizeof *compiler->recipe_queued); assert(compiler->recipe_queued);
    compiler->recipe_count = 6;
    unsigned record = 0;
    for (unsigned kind = MR_RECIPE_PROGRAM; kind <= MR_RECIPE_FIXED; ++kind)
        for (int mode = 0; mode <= HALO_RADIAL_FOG_VIEW_PLANE; ++mode) {
            mr_program_state state = base_state(1), restored;
            state.vertex_key = keys[0]; state.pixel_key = keys[1]; state.radial_fog = mode;
            state.fog_enable = 1; state.fog_color = 0xFF102030;
            uint64_t key = kind == MR_RECIPE_PROGRAM ? program_key(&state, sizeof(FogVertex), 0) : fixed_rhw_key(&state, 0, 1);
            mr_recipe recipe;
            recipe_from_state(&recipe, kind, &state, sizeof(FogVertex), 0, 1, key);
            assert(recipe.radial_fog == mode && recipe.fog_enable == 1);
            assert(!recipe_state_locked(compiler, &recipe, &restored));
            assert(restored.radial_fog == mode && restored.fog_enable == 1);
            uint64_t restored_key = kind == MR_RECIPE_PROGRAM ? program_key(&restored, sizeof(FogVertex), 0) : fixed_rhw_key(&restored, 0, 1);
            assert(key == restored_key);
            compiler->recipes[record++] = recipe;
        }
    /* A prior opt-in session may have saved radial recipes. The immutable
     * default-off setting must keep those from competing for compile time,
     * while still preparing both ordinary recipes whose shaders exist. */
    assert(!compiler->radial_fog_prewarm && !compiler->workers_wanted);
    pthread_mutex_lock(&compiler->lock); recipes_scan_locked(compiler, 0); pthread_mutex_unlock(&compiler->lock);
    assert(compiler->prepare_count == 2 && compiler->prepare[0] == 0 && compiler->prepare[1] == 3);
    for (unsigned i = 0; i < compiler->recipe_count; ++i)
        assert(compiler->recipe_queued[i] == (i % 3 == 0));
}

static void destroy_test_shared(mr_shared *shared) {
    /* The synchronous fixture owns the compiler caches. Entries only borrow
     * translations, so destroying them as in the old inline compiler would
     * double-free a source reused by several pipelines. */
    mr_compiler *compiler = shared->compiler;
    assert(compiler && !compiler->workers && !compiler->store_open);
    for (unsigned i = 0; i < shared->program_count; ++i) shared->programs[i].pipeline = nil;
    for (unsigned i = 0; i < shared->fixed_rhw_program_count; ++i) shared->fixed_rhw_programs[i].pipeline = nil;
    for (unsigned bucket = 0; bucket < MR_XLAT_BUCKETS; ++bucket) {
        mr_xlat *entry = compiler->xlats[bucket];
        while (entry) { mr_xlat *next = entry->next; ms_shader_destroy(&entry->sh); free(entry); entry = next; }
    }
    for (unsigned bucket = 0; bucket < MR_FN_BUCKETS; ++bucket) {
        mr_fn *entry = compiler->fns[bucket];
        while (entry) { mr_fn *next = entry->next; entry->fn = nil; free(entry->text); free(entry->error); free(entry); entry = next; }
    }
    for (unsigned bucket = 0; bucket < MR_REG_BUCKETS; ++bucket) {
        mr_registered *entry = compiler->registered[bucket];
        while (entry) { mr_registered *next = entry->next; free(entry); entry = next; }
    }
    free(compiler->recipes); free(compiler->recipe_queued);
    free(compiler->prepare); free(compiler->build); free(compiler->translate); free(compiler->pixel_shaders);
    if (compiler->manifest_fd >= 0) close(compiler->manifest_fd);
    pthread_cond_destroy(&compiler->work); pthread_cond_destroy(&compiler->changed); pthread_mutex_destroy(&compiler->lock);
    free(compiler); shared->dev = nil; free(shared);
}

static void check_legacy_manifest(void) {
    /* The v1 disk ABI predates radial fog. Keep an independent declaration so
     * a field insertion cannot silently make a freshly written "legacy"
     * fixture agree with a broken migration. */
    typedef struct {
        uint32_t kind; int32_t clip_space, overlay, has_ps;
        uint64_t key, vertex_key, pixel_key;
        uint32_t stride, declaration_bytes, stream_stride[16];
        int32_t blend, blend_src, blend_dst, blend_op, color_write_mask;
        int32_t alpha_enabled, alpha_func, fog_enable;
        uint8_t sampler_type[16], sampler_bound[16];
        mr_fixed_stage fixed_stages[8]; uint8_t declaration[MR_RECIPE_DECL];
    } RecipeV1;
    _Static_assert(sizeof(RecipeV1) == 1080, "fixture v1 ABI");
    _Static_assert(offsetof(mr_recipe, radial_fog) == sizeof(RecipeV1), "v1 prefix preserved");
    char directory[] = "/tmp/halo-radial-manifest-XXXXXX"; assert(mkdtemp(directory));
    char legacy_path[256], current_path[256];
    snprintf(legacy_path, sizeof legacy_path, "%s/manifest-v1.bin", directory);
    snprintf(current_path, sizeof current_path, "%s/manifest-v%u.bin", directory, MR_MANIFEST_VERSION);
    RecipeV1 records[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        records[i].kind = MR_RECIPE_PROGRAM; records[i].key = i == 1 ? 0x234 : 0x123;
        records[i].vertex_key = 0x500 + i; records[i].pixel_key = 0x600 + i;
        records[i].has_ps = 1; records[i].stride = sizeof(FogVertex); records[i].declaration_bytes = sizeof declaration;
        memcpy(records[i].declaration, declaration, sizeof declaration);
        records[i].fog_enable = i & 1; records[i].alpha_enabled = 1; records[i].alpha_func = 4;
        records[i].color_write_mask = 15; records[i].sampler_type[3] = 2; records[i].sampler_bound[3] = 1;
        records[i].fixed_stages[0].color_op = 1 + i; records[i].stream_stride[2] = 16 + i;
    }
    const uint32_t legacy_header[] = {MR_MANIFEST_MAGIC, 1, sizeof(RecipeV1), 0};
    const uint8_t torn_tail[] = {1, 2, 3, 4, 5, 6, 7};
    int fd = open(legacy_path, O_WRONLY | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    assert(write(fd, legacy_header, sizeof legacy_header) == sizeof legacy_header);
    assert(write(fd, records, sizeof records) == sizeof records);
    assert(write(fd, torn_tail, sizeof torn_tail) == sizeof torn_tail); assert(!close(fd));
    NSData *legacy = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:legacy_path]]; assert(legacy);
    mr_shared *shared = calloc(1, sizeof *shared); assert(shared);
    mr_compiler *compiler = compiler_for(shared); snprintf(compiler->dir, sizeof compiler->dir, "%s", directory);
    manifest_load(compiler);
    assert(compiler->recipe_count == 2);
    for (unsigned i = 0; i < 2; ++i) {
        assert(!memcmp(&compiler->recipes[i], &records[i + 1], sizeof(RecipeV1)));
        assert(compiler->recipes[i].radial_fog == 0);
    }
    assert([legacy isEqualToData:[NSData dataWithContentsOfFile:[NSString stringWithUTF8String:legacy_path]]]);
    struct stat info; assert(!stat(current_path, &info));
    assert(info.st_size == 16 + 2 * sizeof(mr_recipe));
    mr_recipe migrated[2]; uint32_t header[4];
    fd = open(current_path, O_RDONLY); assert(fd >= 0);
    assert(read(fd, header, sizeof header) == sizeof header);
    assert(header[0] == MR_MANIFEST_MAGIC && header[1] == MR_MANIFEST_VERSION && header[2] == sizeof(mr_recipe));
    assert(read(fd, migrated, sizeof migrated) == sizeof migrated); assert(!close(fd));
    assert(!memcmp(migrated, compiler->recipes, sizeof migrated));
    destroy_test_shared(shared);

    /* An existing v2 manifest wins on the next launch and keeps its mode. */
    migrated[0].radial_fog = HALO_RADIAL_FOG_VIEW_PLANE;
    fd = open(current_path, O_WRONLY | O_TRUNC); assert(fd >= 0);
    assert(write(fd, header, sizeof header) == sizeof header);
    assert(write(fd, migrated, sizeof migrated) == sizeof migrated); assert(!close(fd));
    shared = calloc(1, sizeof *shared); assert(shared); compiler = compiler_for(shared);
    snprintf(compiler->dir, sizeof compiler->dir, "%s", directory); manifest_load(compiler);
    assert(compiler->recipe_count == 2 && !memcmp(compiler->recipes, migrated, sizeof migrated));
    assert(!compiler->radial_fog_prewarm);
    assert((compiler->variants_seen & (1u << 8)) && !(compiler->variants_seen & (1u << 9)));
    destroy_test_shared(shared);
    assert(!unlink(current_path) && !unlink(legacy_path) && !rmdir(directory));
    puts("PASS radial fog manifest: v1 fields/keys preserved, newest duplicate retained, torn tail removed, legacy file untouched, radial0 migration and radial2 v2 reload");
}

static void check_sources(void) {
    sources = [NSMutableArray array];
    mr_shared *shared = calloc(1, sizeof *shared); assert(shared);
    shared->dev = (id<MTLDevice>)[FogTestDevice new];
    mr_context c = {.s = shared};
    for (int pixel = 0; pixel < 4; ++pixel) {
        mr_program_state s = base_state(pixel);
        mr_program_cache *off = program_for(&c, &s, sizeof(FogVertex));
        if (!off) fprintf(stderr, "pipeline: %s\n", mr_last_error());
        assert(off);
        ms_shader reference = {0};
        assert(ms_translate_shader(vertex_tokens, sizeof vertex_tokens, MS_STAGE_VERTEX, off->vs->entry, &reference) == 0);
        assert(reference.source_bytes == off->vs->source_bytes);
        assert(memcmp(reference.source, off->vs->source, reference.source_bytes) == 0);
        ms_shader_destroy(&reference);
        mr_texts texts;
        assert(!program_texts(shared->compiler, &s, 0, &texts));
        /* Baseline MojoShader calls oFog semantic index 1, whereas the
         * existing bridge seeks index 0. Leave that default behavior alone. */
        assert(!strstr(texts.fs_text, pixel ? "output.oC0.rgb = mix(" : "current.rgb = mix("));
        texts_free(&texts);
        s.radial_fog = HALO_RADIAL_FOG_DEPTH;
        mr_program_cache *on = program_for(&c, &s, sizeof(FogVertex));
        assert(on && on != off && on->key != off->key);
        assert(strcmp(on->vs->source, off->vs->source));
        assert(!program_texts(shared->compiler, &s, 0, &texts));
        assert(strstr(texts.fs_text, pixel ? "output.oC0.rgb = mix(" : "current.rgb = mix("));
        assert(!strstr(texts.fs_text, pixel ? "output.oC0.a = mix(" : "current.a = mix("));
        texts_free(&texts);
        assert(on == program_for(&c, &s, sizeof(FogVertex)));
        s.radial_fog = 0;
        assert(off == program_for(&c, &s, sizeof(FogVertex)));
        if (pixel) assert(on->extras && !off->extras);

        s.vertex_tokens = NULL; s.vertex_token_bytes = 0; s.fog_enable = 0;
        mr_fixed_rhw_cache *fixed_off = fixed_rhw_program_for(&c, &s, 1);
        assert(fixed_off);
        assert(!fixed_rhw_texts(shared->compiler, &s, 1, &texts));
        assert(!strstr(texts.vs_text, "[[user(fog)]]"));
        /* Build78 ignored fixed fog-enable without the opt-in. Preserve the
         * same pipeline identity and both stage texts for that exact state. */
        s.fog_enable = 1;
        assert(!s.radial_fog && fixed_off == fixed_rhw_program_for(&c, &s, 1));
        mr_texts ignored_fog;
        assert(!fixed_rhw_texts(shared->compiler, &s, 1, &ignored_fog));
        assert(!strcmp(texts.vs_text, ignored_fog.vs_text) && !strcmp(texts.fs_text, ignored_fog.fs_text));
        texts_free(&ignored_fog);
        texts_free(&texts);
        s.fog_enable = 1; s.radial_fog = HALO_RADIAL_FOG_DEPTH;
        mr_fixed_rhw_cache *fixed_on = fixed_rhw_program_for(&c, &s, 1);
        assert(fixed_on && fixed_on != fixed_off && fixed_on->key != fixed_off->key);
        assert(!fixed_rhw_texts(shared->compiler, &s, 1, &texts));
        assert(strstr(texts.vs_text, "o.fog=i.fog;"));
        assert(strstr(texts.fs_text, pixel ? "input.mr_fog.x" : "input.fog.x"));
        texts_free(&texts);
        assert(fixed_on == fixed_rhw_program_for(&c, &s, 1));
        s.fog_enable = 0; s.radial_fog = 0;
        assert(fixed_off == fixed_rhw_program_for(&c, &s, 1));
        if (pixel) assert(fixed_on->extras && !fixed_off->extras);
    }
    assert(shared->program_count == 8 && shared->fixed_rhw_program_count == 8);
    check_fallback(&c);
    assert(shared->program_count == 17);
    check_recipes(shared);
    const char *directory = getenv("HALO_RADIAL_FOG_MSL_DIR");
    if (directory && *directory) {
        for (NSUInteger i = 0; i < sources.count; ++i) {
            NSString *file = [NSString stringWithFormat:@"%s/radial-fog-%02lu.metal", directory, (unsigned long)i];
            assert([sources[i] writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        }
    }
    destroy_test_shared(shared);
    printf("PASS radial fog renderer: real MojoShader, 25 pipelines / %lu compiled stage sources, off VS byte-identical, off fixed fog source/key unchanged, unknown/no-temp/matrix fallback, fog varying links, RGB-only blending, cache segregation/reuse, 6 persisted recipe roundtrips, saved radial recipes not queued while disabled; mock Metal\n", (unsigned long)sources.count);
}

static void read_pixel(mr_context *c, float expected_red, float expected_blue, const char *label) {
    uint8_t pixels[8 * 8 * 4];
    assert(mr_read_framebuffer(c, pixels, sizeof pixels) == 0);
    const uint8_t *p = pixels + (4 * 8 + 4) * 4;
    fprintf(stdout, "%s BGRA=%u,%u,%u,%u\n", label, p[0], p[1], p[2], p[3]);
    assert(abs((int)p[0] - (int)lroundf(expected_blue * 255.f)) <= 1);
    assert(p[1] == 0);
    assert(abs((int)p[2] - (int)lroundf(expected_red * 255.f)) <= 1);
    assert(p[3] == 128); /* Fog must never change alpha. */
}

static void check_gpu(void) {
    mr_context *c = mr_create(8, 8);
    if (!c) fprintf(stderr, "Metal context: %s\n", mr_last_error());
    assert(c);
    printf("GPU: %s\n", c->s->dev.name.UTF8String);
    FogVertex vertices[3] = {
        {{-1, -1, 0.5, 1}, {3, 4, 0, 1}},
        {{ 3, -1, 0.5, 1}, {3, 4, 0, 1}},
        {{-1,  3, 0.5, 1}, {3, 4, 0, 1}}
    };
    for (int pixel = 0; pixel < 4; ++pixel) {
        mr_program_state s = base_state(pixel);
        for (int bearing = 0; bearing < 2; ++bearing) {
            constants[6][0] = bearing ? 0.f : 0.1f;
            constants[6][1] = bearing ? 0.1f : 0.f;
            for (int radial = 0; radial < 2; ++radial) {
                s.radial_fog = radial ? HALO_RADIAL_FOG_DEPTH : 0;
                mr_clear(c, 0);
                int result = mr_draw_program(c, &s, MR_TRIANGLE_LIST, vertices, sizeof vertices[0], 3, NULL, 0);
                if (result) fprintf(stderr, "draw: %s\n", mr_last_error());
                assert(result == MR_OK);
                /* Default reproduces the existing renderer's lack of oFog
                 * blending; the enabled correction repairs that varying. */
                float transmittance = radial ? 0.5f : 1.f;
                read_pixel(c, transmittance, 1.f - transmittance, pixel ? "translated VS + PS" : "translated VS + fixed fragment");
            }
        }
        mr_vertex_fixed_clip fixed[3] = {0};
        for (int i = 0; i < 3; ++i) {
            fixed[i].x = vertices[i].position[0]; fixed[i].y = vertices[i].position[1];
            fixed[i].z = 0.5f; fixed[i].w = 1.f; fixed[i].color = 0x80ff0000;
            fixed[i].fog = 0.25f;
        }
        s.vertex_tokens = NULL; s.vertex_token_bytes = 0;
        for (int fog = 0; fog < 2; ++fog) {
            s.fog_enable = fog; s.radial_fog = fog ? HALO_RADIAL_FOG_DEPTH : 0;
            mr_clear(c, 0);
            int result = mr_draw_fixed_clip(c, &s, MR_TRIANGLE_LIST, fixed, sizeof fixed[0], 3, NULL, 0);
            if (result) fprintf(stderr, "fixed draw: %s\n", mr_last_error());
            assert(result == MR_OK);
            read_pixel(c, fog ? 0.25f : 1.f, fog ? 0.75f : 0.f, pixel ? "fixed clip + PS" : "fixed clip + fixed fragment");
        }
    }
    mr_destroy(c);
    puts("PASS radial fog renderer GPU: 24 draws/readbacks, 2 bearings, fixed/programmed fragments and clip vertices, alpha preserved");
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        assert(!setenv("HALO_PIPELINE_WORKERS", "0", 1));
        assert(!setenv("HALO_PIPELINE_CACHE", "0", 1));
        assert(!setenv("HALO_RADIAL_FOG", "0", 1));
        prepare(); check_sources(); check_legacy_manifest();
        if (argc == 2 && !strcmp(argv[1], "--gpu")) check_gpu();
    }
    return 0;
}
