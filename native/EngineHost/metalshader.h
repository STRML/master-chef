#ifndef METALSHADER_H
#define METALSHADER_H
#include <stddef.h>
#include <stdint.h>

enum { MS_MAX_ATTRIBUTES = 16, MS_MAX_OUTPUTS = 16, MS_MAX_UNIFORMS = 512, MS_MAX_SAMPLERS = 16 };
enum { MS_STAGE_VERTEX = 1, MS_STAGE_PIXEL = 2 };
enum { MS_UNIFORM_FLOAT4 = 0, MS_UNIFORM_INT4 = 1, MS_UNIFORM_BOOL = 2 };

typedef struct ms_binding {
    int type;
    int index;
    int count;
} ms_binding;

typedef struct ms_semantic {
    int usage;
    int index;
    int location;
} ms_semantic;

typedef struct ms_sampler {
    int type;
    int index;
} ms_sampler;

typedef struct ms_shader {
    char *source;
    size_t source_bytes;
    char entry[64];
    int stage;
    int major, minor;
    int used_synthetic_ctab;
    int uniform_count, packed_float4_count, packed_int4_count, packed_bool_count;
    ms_binding uniforms[MS_MAX_UNIFORMS];
    int attribute_count;
    ms_semantic attributes[MS_MAX_ATTRIBUTES];
    int output_count;
    ms_semantic outputs[MS_MAX_OUTPUTS];
    int sampler_count;
    ms_sampler samplers[MS_MAX_SAMPLERS];
    char error[512];
} ms_shader;

/* Uses upstream MojoShader's Metal profile. For Halo's stripped relative-
 * constant shaders, a metadata-only c0 register-array CTAB is inserted before
 * parsing; instruction bytes are copied unchanged. */
int ms_translate_shader(const uint32_t *tokens, size_t token_bytes, int expected_stage,
                        const char *entry, ms_shader *out);
/* ps_1_x shaders do not declare sampler types; D3D samples according to the
 * bound texture. sampler_types[i] (0 2D, 1 cube, 2 volume, -1 unknown) is
 * forwarded to MojoShader's sampler map for shaders older than 2.0 only. */
int ms_translate_shader_mapped(const uint32_t *tokens, size_t token_bytes, int expected_stage,
                               const char *entry, const int *sampler_types, int sampler_type_count,
                               ms_shader *out);
void ms_shader_destroy(ms_shader *shader);

#endif
