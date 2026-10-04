/* vkshader.h — Halo PC -> SPIR-V shader translation layer.
 *
 * The game ships original-Xbox D3D8 token streams (vs_1_1, ps_1_1, ps_2_0;
 * ps_1_4 is not implemented by the vendored MojoShader SPIR-V emitter and is
 * rejected here). MojoShader's SPIRV profile turns those into SPIR-V that
 * loads on a Vulkan driver, with a SpirvPatchTable appended to the output.
 *
 * The translation layer keeps the MOJOSHADER_parseData alive (the link API
 * patches it in place and needs the patch table's internal arrays, which
 * freeParseData does not free).
 *
 * Descriptor layout (SPIRV_MODE_VK, from profiles/mojoshader_profile_spirv.c):
 *   VS samplers    : set 0, binding = sampler register
 *   VS uniform UBO : set 1, binding 0   (array_vec4/ivec4/bool members)
 *   PS samplers    : set 2, binding = sampler register
 *   PS uniform UBO : set 3, binding 0
 *   vertex inputs  : Location = D3D register number (v0->0, v4->4, v9->9)
 *   VS->PS outputs : Location patched by vkshader_link()
 *
 * The UBO is a dense array in declaration order: each declared constant
 * (type, index, array_count) copies from the stage's register file at
 * `index`; the engine's state->vs_float4 / ps_float4 are the full files.
 */
#ifndef VKSHADER_H
#define VKSHADER_H

#include <stddef.h>
#include <stdint.h>

#define VKSHADER_MAX_UNIFORMS 32
#define VKSHADER_MAX_ATTRIBS 16
#define VKSHADER_MAX_OUTPUTS 16
#define VKSHADER_MAX_SAMPLERS 16

/* MojoShader uniform types */
#define VKSHADER_UNI_FLOAT 0
#define VKSHADER_UNI_INT 1
#define VKSHADER_UNI_BOOL 2

/* MojoShader attribute usages (MOJOSHADER_USAGE_* values, mojoshader.h:157-172).
 * Note: the D3D8 declaration bytes use different numbering (POSITION=0,
 * BLENDWEIGHT=1, BLENDINDICES=2, NORMAL=3, PSIZE=4, TEXCOORD=5, USAGE=6,
 * TEXBLEND=7, VERTEXBLEND=8, CLOUD=9, COLOR=10, FOG=11, DEPTH=12,
 * SAMPLE=13); the renderer's build_vertex_input maps one to the other. */
enum vkshader_usage {
    VKSHADER_USAGE_UNKNOWN = 0,
    VKSHADER_USAGE_POSITION = 1,
    VKSHADER_USAGE_BLENDWEIGHT = 2,
    VKSHADER_USAGE_BLENDINDICES = 3,
    VKSHADER_USAGE_NORMAL = 4,
    VKSHADER_USAGE_POINTSIZE = 5,
    VKSHADER_USAGE_TEXCOORD = 6,
    VKSHADER_USAGE_TANGENT = 7,
    VKSHADER_USAGE_BINORMAL = 8,
    VKSHADER_USAGE_TESSFACTOR = 9,
    VKSHADER_USAGE_POSITIONT = 10,
    VKSHADER_USAGE_COLOR = 11,
    VKSHADER_USAGE_FOG = 12,
    VKSHADER_USAGE_DEPTH = 13,
    VKSHADER_USAGE_SAMPLE = 14,
    VKSHADER_USAGE_TOTAL = 15
};

typedef struct vkshader_uniform {
    int type;          /* VKSHADER_UNI_FLOAT/INT/BOOL */
    uint32_t index;    /* first register in the stage's register file */
    uint32_t count;    /* number of vec4s (1 if array_count==0) */
} vkshader_uniform;

typedef struct vkshader {
    const void *pd;            /* MOJOSHADER_parseData* (opaque; held alive) */
    const uint32_t *spirv;     /* SPIR-V words, patch table appended */
    size_t spirv_bytes;        /* length in bytes (includes patch table) */
    int stage;                 /* 0 = vertex, 1 = pixel */
    int major, minor;          /* D3D token version (vs_1_1 -> 1,1) */

    uint32_t uniform_count;
    vkshader_uniform uniforms[VKSHADER_MAX_UNIFORMS];

    uint32_t attribute_count;  /* vertex inputs (VS) / PS inputs (PS) */
    /* regnum is the D3D register number (the "vN" name MojoShader assigns to
     * VS inputs; the SPIR-V emitter decorates VS input variables with
     * Location = regnum, see mojoshader_profile_spirv.c:2284). */
    struct { uint32_t usage, index, regnum; } attributes[VKSHADER_MAX_ATTRIBS];
    uint32_t output_count;     /* VS outputs */
    struct { uint32_t usage, index; } outputs[VKSHADER_MAX_OUTPUTS];

    uint32_t sampler_count;
    struct { uint32_t type, index; } samplers[VKSHADER_MAX_SAMPLERS];

    int linked;                /* 1 after vkshader_link patched a VS+PS pair */
    char error[256];
} vkshader;

/* Translate a single token stream. bytes must be the raw D3D9/Xbox token
 * dwords (little endian). stage: 0 = vertex, 1 = pixel. Returns 0 on success
 * (out->spirv set, caller frees with vkshader_destroy), -1 on failure
 * (out->error filled). Expected vertex input Location is the D3D register
 * number; outputs carry 0xDEADBEEF sentinels until linked. */
int vkshader_translate(const uint32_t *tokens, size_t bytes, int stage, vkshader *out);

/* Link a translated VS+PS pair: patches the VS outputs and PS inputs to a
 * shared Location sequence (in place in both modules). Returns 0 on success,
 * -1 on failure (out->error). Call before uploading the SPIR-V to the driver.
 *
 * formats: per-attribute vertex element format (MOJOSHADER_VERTEXELEMENTFORMAT_*,
 * same order as vkshader.attributes) — the input variables are patched to
 * the matching type; NULL means all FLOAT4. */
int vkshader_link(vkshader *vs, vkshader *ps, const uint8_t *formats);

/* Link a vertex shader to the engine's fixed-function fragment shader (no_ps
 * draws): the VS's outputs must be exactly one diffuse colour (Location 1)
 * plus one texcoord (Location 0) usable by the fixed fragment. Non-float
 * declaration element types are carried in `formats`
 * (MOJOSHADER_VERTEXELEMENTFORMAT_*, same order as vkshader.attributes) and
 * patch the VS input variables exactly like vkshader_link does for real
 * pairs; NULL means all float. Returns 0 on success, -1 when the VS output
 * shape does not fit the fixed fragment; the renderer then falls back. */
int vkshader_link_fixedfunc(vkshader *vs, const uint8_t *formats);

/* Byte size of the UBO for this shader's uniform block (0 = no uniforms). */
size_t vkshader_uniform_size(const vkshader *sh);

/* Fill the UBO byte buffer from the draw's register files.
 * out points at the UBO memory (caller-owned, sized by vkshader_uniform_size);
 * float4s is the stage's float4 register file (vs_float4/ps_float4),
 * int4s/bools may be NULL (the engine only tracks float4 today; a shader
 * that needs them fails the pack and falls back). */
void vkshader_pack_uniforms(const vkshader *sh, void *out,
                            const float *float4s, uint32_t float4_count,
                            const int *int4s, const int *bools);

/* SPIR-V bytes without the patch table (for vkCreateShaderModule). */
size_t vkshader_spirv_bytes(const vkshader *sh);

void vkshader_destroy(vkshader *sh);

/* Stats counters (cumulative over the process). */
void vkshader_stats(uint64_t *translated, uint64_t *failed);

#endif /* VKSHADER_H */
