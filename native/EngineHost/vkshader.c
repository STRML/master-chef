/* vkshader.c — Halo PC D3D8 token streams -> SPIR-V via vendored MojoShader.
 *
 * The game's vertex shaders are original-Xbox vs_1_1 (no CTAB; the D3D8
 * `def`/`dcl` tokens carry the constants). Pixel shaders are ps_1_1, ps_2_0
 * (with a legacy 20-byte D3DX CTAB) and ps_1_4 (unsupported by the SPIR-V
 * emitter; translation fails -> the renderer falls back to the fixed
 * pipeline). The two CTAB normalizations mirror metalshader.c so the SPIR-V
 * path sees exactly the same preprocessing the Metal path does.
 */
#include "vkshader.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MOJOSHADER_NO_VERSION_INCLUDE 1
#include "../../third_party/mojoshader/mojoshader.h"
#define __MOJOSHADER_INTERNAL__ 1
#include "../../third_party/mojoshader/mojoshader_internal.h"
/* SpvOp codes for the input-type patches in vkshader_link_fixedfunc. */
#include "../../third_party/mojoshader/spirv/spirv.h"

static uint64_t g_translated, g_failed;

static void put16(uint8_t *p, uint16_t v) { p[0] = v & 255; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255; p[3] = v >> 24;
}
static uint32_t get32(const void *p) {
    const uint8_t *b = p; return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int has_only_error(const MOJOSHADER_parseData *pd, const char *message) {
    return pd->error_count == 1 && pd->errors[0].error && strstr(pd->errors[0].error, message);
}
static int needs_ctab(const MOJOSHADER_parseData *pd) {
    for (int i = 0; i < pd->error_count; ++i)
        if (pd->errors[i].error && strstr(pd->errors[i].error, "relative addressing unsupported without a CTAB")) return 1;
    return 0;
}

/* ps_2_0 blobs from the original Xbox carry a 20-byte D3DX CTAB header
 * (0x42415443 'CTAB' magic, size field 20) that MojoShader rejects. Strip
 * exactly that metadata block; instructions and version are untouched. */
static uint8_t *without_legacy_ctab20(const uint8_t *source, size_t bytes, size_t *out_bytes) {
    if (bytes < 20 || (bytes & 3)) return NULL;
    uint32_t comment = get32(source + 4);
    size_t payload_words = (comment >> 16) & 0x7FFFu;
    size_t skip_words = payload_words + 1;
    size_t total_words = bytes / 4;
    if ((comment & 0x8000FFFFu) != 0x0000FFFEu || payload_words < 3 || skip_words >= total_words ||
        get32(source + 8) != 0x42415443u || get32(source + 12) != 20u) return NULL;
    size_t skip_bytes = skip_words * 4;
    uint8_t *result = malloc(bytes - skip_bytes);
    if (!result) return NULL;
    memcpy(result, source, 4);
    memcpy(result + 4, source + 4 + skip_bytes, bytes - 4 - skip_bytes);
    *out_bytes = bytes - skip_bytes;
    return result;
}

/* vs_1_1 uses relative constant addressing (c[register]) with no CTAB.
 * Synthesize a D3DXSHADER_CONSTANTTABLE declaring one float4 vector array
 * covering the full register file so MojoShader can resolve the addressing. */
static uint8_t *with_register_ctab(const uint8_t *source, size_t bytes, unsigned registers, size_t *out_bytes) {
    if (bytes < 8 || registers == 0 || registers > 256) return NULL;
    const uint32_t version = get32(source);
    const char *target = ((version >> 16) == 0xFFFE) ? "vs_1_1" : "ps_1_1";
    const char creator[] = "HaloVision synthetic CTAB";
    const char name[] = "c";
    uint8_t payload[128] = {0};
    const uint32_t creator_offset = 64;
    const uint32_t target_offset = creator_offset + (uint32_t)sizeof creator;
    const uint32_t name_offset = target_offset + (uint32_t)strlen(target) + 1;
    put32(payload + 0, 28); put32(payload + 4, creator_offset); put32(payload + 8, version);
    put32(payload + 12, 1); put32(payload + 16, 28); put32(payload + 20, 0); put32(payload + 24, target_offset);
    put32(payload + 28, name_offset); put16(payload + 32, 2); put16(payload + 34, 0);
    put16(payload + 36, (uint16_t)registers); put16(payload + 38, 0); put32(payload + 40, 48); put32(payload + 44, 0);
    put16(payload + 48, 1); put16(payload + 50, 3); put16(payload + 52, 1); put16(payload + 54, 4);
    put16(payload + 56, (uint16_t)registers); put16(payload + 58, 0); put32(payload + 60, 0);
    memcpy(payload + creator_offset, creator, sizeof creator);
    memcpy(payload + target_offset, target, strlen(target) + 1);
    memcpy(payload + name_offset, name, sizeof name);
    size_t payload_bytes = (name_offset + sizeof name + 3) & ~(size_t)3;
    size_t inserted = payload_bytes + 8;
    uint8_t *result = malloc(bytes + inserted);
    if (!result) return NULL;
    memcpy(result, source, 4);
    put32(result + 4, (uint32_t)((1 + payload_bytes / 4) << 16) | 0xFFFEu);
    put32(result + 8, 0x42415443u);
    memcpy(result + 12, payload, payload_bytes);
    memcpy(result + 4 + inserted, source + 4, bytes - 4);
    *out_bytes = bytes + inserted;
    return result;
}

int vkshader_translate(const uint32_t *tokens, size_t bytes, int stage, vkshader *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->stage = stage;
    if (!tokens || bytes < 8 || (bytes & 3) || bytes > 1024 * 1024) {
        snprintf(out->error, sizeof out->error, "invalid shader input"); return -1;
    }
    /* Reject non-token streams and the ps_1_4 family (the SPIR-V emitter
     * has no TEXLD for them; the renderer must not try). */
    uint32_t vtok = get32(tokens);
    if (((vtok >> 16) & 0xFFFF) != 0xFFFE && ((vtok >> 16) & 0xFFFF) != 0xFFFF) {
        snprintf(out->error, sizeof out->error, "not a shader token stream"); return -1;
    }
    int is_vs = ((vtok >> 16) & 0xFFFF) == 0xFFFE;
    if ((is_vs && stage != 0) || (!is_vs && stage != 1)) {
        snprintf(out->error, sizeof out->error, "shader stage mismatch"); return -1;
    }

    const uint8_t *active = (const uint8_t *)tokens;
    size_t active_bytes = bytes;
    uint8_t *normalized = NULL, *augmented = NULL;
    const MOJOSHADER_parseData *pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_SPIRV, "main",
                                                      active, (unsigned)active_bytes, NULL, 0, NULL, 0, NULL, NULL, NULL);
    if (has_only_error(pd, "Shader has corrupt CTAB data")) {
        MOJOSHADER_freeParseData(pd);
        normalized = without_legacy_ctab20((const uint8_t *)tokens, bytes, &active_bytes);
        if (!normalized) { snprintf(out->error, sizeof out->error, "legacy CTAB normalization failed"); return -1; }
        active = normalized;
        pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_SPIRV, "main", active, (unsigned)active_bytes, NULL, 0, NULL, 0, NULL, NULL, NULL);
    }
    if (pd->error_count && needs_ctab(pd)) {
        MOJOSHADER_freeParseData(pd);
        unsigned count = is_vs ? 256 : 224;
        augmented = with_register_ctab(active, active_bytes, count, &active_bytes);
        if (!augmented) { free(normalized); snprintf(out->error, sizeof out->error, "synthetic CTAB failed"); return -1; }
        active = augmented;
        pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_SPIRV, "main", active, (unsigned)active_bytes, NULL, 0, NULL, 0, NULL, NULL, NULL);
    }
    if (pd->error_count) {
        size_t used = 0;
        for (int i = 0; i < pd->error_count && used + 4 < sizeof out->error; ++i) {
            const char *msg = pd->errors[i].error ? pd->errors[i].error : "unknown MojoShader error";
            int n = snprintf(out->error + used, sizeof out->error - used, "%s%s@%d", i ? "; " : "", msg, pd->errors[i].error_position);
            if (n < 0) break;
            used += (size_t)n < sizeof out->error - used ? (size_t)n : sizeof out->error - used - 1;
        }
        MOJOSHADER_freeParseData(pd);
        free(augmented); free(normalized);
        g_failed++;
        return -1;
    }
    if (!pd->output || pd->output_len <= (int)sizeof(SpirvPatchTable)) {
        snprintf(out->error, sizeof out->error, "no SPIR-V output");
        MOJOSHADER_freeParseData(pd); free(augmented); free(normalized);
        g_failed++;
        return -1;
    }
    if ((uint32_t)pd->uniform_count > VKSHADER_MAX_UNIFORMS ||
        (uint32_t)pd->attribute_count > VKSHADER_MAX_ATTRIBS ||
        (uint32_t)pd->output_count > VKSHADER_MAX_OUTPUTS ||
        (uint32_t)pd->sampler_count > VKSHADER_MAX_SAMPLERS) {
        snprintf(out->error, sizeof out->error, "shader metadata exceeds renderer bounds");
        MOJOSHADER_freeParseData(pd); free(augmented); free(normalized);
        g_failed++;
        return -1;
    }

    /* The renderer flattens cube maps into 2D strips (see
     * mr_texture_create_cube_cached), so a shader that declares a
     * CUBE/VOLUME sampler would demand a cube view the engine never
     * creates. Reject it; the fixed pipeline handles the draw. */
    for (int i = 0; i < pd->sampler_count; ++i) {
        if (pd->samplers[i].type != MOJOSHADER_SAMPLER_2D) {
            snprintf(out->error, sizeof out->error, "sampler %d is not 2D", pd->samplers[i].index);
            MOJOSHADER_freeParseData(pd); free(augmented); free(normalized);
            g_failed++;
            return -1;
        }
    }

    out->pd = pd;                       /* held alive; the link API patches it */
    out->spirv = (const uint32_t *)pd->output;
    out->spirv_bytes = (size_t)pd->output_len;
    out->major = pd->major_ver; out->minor = pd->minor_ver;

    out->uniform_count = (uint32_t)pd->uniform_count;
    for (int i = 0; i < pd->uniform_count; ++i) {
        const MOJOSHADER_uniform *u = &pd->uniforms[i];
        out->uniforms[i].type = (u->type == MOJOSHADER_UNIFORM_FLOAT) ? VKSHADER_UNI_FLOAT :
                                (u->type == MOJOSHADER_UNIFORM_INT) ? VKSHADER_UNI_INT : VKSHADER_UNI_BOOL;
        out->uniforms[i].index = (uint32_t)u->index;
        out->uniforms[i].count = (uint32_t)(u->array_count ? u->array_count : 1);
    }
    out->attribute_count = (uint32_t)pd->attribute_count;
    for (int i = 0; i < pd->attribute_count; ++i) {
        out->attributes[i].usage = (uint32_t)pd->attributes[i].usage;
        out->attributes[i].index = (uint32_t)pd->attributes[i].index;
        /* VS inputs carry the D3D register name ("vN"); the SPIR-V emitter
         * decorates the input variable with Location = that register number
         * (mojoshader_profile_spirv.c:2284) and the link path never remaps
         * it, so the renderer must bind the declaration's element at the
         * same location. PS inputs are "tN" (or TEXCOORD semantics) — the
         * renderer ignores regnum there and uses the link's dense numbering. */
        int regnum = -1;
        const char *nm = pd->attributes[i].name;
        if (nm && nm[0] == 'v' && nm[1] >= '0' && nm[1] <= '9')
            regnum = atoi(nm + 1);
        out->attributes[i].regnum = (uint32_t)(regnum >= 0 ? regnum : 0);
    }
    out->output_count = (uint32_t)pd->output_count;
    for (int i = 0; i < pd->output_count; ++i) {
        out->outputs[i].usage = (uint32_t)pd->outputs[i].usage;
        out->outputs[i].index = (uint32_t)pd->outputs[i].index;
    }
    out->sampler_count = (uint32_t)pd->sampler_count;
    for (int i = 0; i < pd->sampler_count; ++i) {
        out->samplers[i].type = (uint32_t)pd->samplers[i].type;
        out->samplers[i].index = (uint32_t)pd->samplers[i].index;
    }
    free(augmented); free(normalized);
    g_translated++;
    return 0;
}

int vkshader_link(vkshader *vs, vkshader *ps, const uint8_t *formats) {
    if (!vs || !ps || !vs->pd || !ps->pd) {
        if (vs) snprintf(vs->error, sizeof vs->error, "link: invalid shader");
        return -1;
    }
    const MOJOSHADER_parseData *vpd = (const MOJOSHADER_parseData *)vs->pd;
    const MOJOSHADER_parseData *ppd = (const MOJOSHADER_parseData *)ps->pd;
    /* The renderer supplies the per-attribute vertex element format from the
     * D3D8 declaration (see vulkanrenderer.c decl_mojo_format). The link
     * patches the input variable's type and load opcode to match it
     * (vec4/ivec4/uvec4 + S/UToF); NULL means "all FLOAT4". */
    MOJOSHADER_vertexAttribute attrs[VKSHADER_MAX_ATTRIBS];
    int acount = 0;
    for (int i = 0; i < vpd->attribute_count && acount < VKSHADER_MAX_ATTRIBS; ++i) {
        attrs[acount].usage = vpd->attributes[i].usage;
        attrs[acount].usageIndex = vpd->attributes[i].index;
        attrs[acount].vertexElementFormat = formats ? formats[i] : MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR4;
        acount++;
    }
    int patch = MOJOSHADER_linkSPIRVShaders(vpd, ppd, attrs, acount);
    if (patch <= 0) {
        snprintf(vs->error, sizeof vs->error, "linkSPIRVShaders failed (%d)", patch);
        return -1;
    }
    vs->linked = 1; ps->linked = 1;
    return 0;
}

/* Link a game vertex shader to the engine's fixed-function fragment shader
 * (no_ps draws: the game provides a vertex shader, the pixel stage is the
 * pretransformed-texture fragment from the static SPIR-V blob). The fixed
 * fragment consumes two vertex varyings: colour at Location 1 and a single
 * texcoord at Location 0 (the static fragment's OpDecorate locations;
 * vk_spv_rhw_vert writes exactly those). A VS whose outputs don't match
 * that shape (no diffuse, multiple texcoords, point size...) cannot be
 * served by the fixed fragment: return -1 and let the renderer fall back. */
int vkshader_link_fixedfunc(vkshader *vs, const uint8_t *formats) {
    if (!vs || !vs->pd) {
        if (vs) snprintf(vs->error, sizeof vs->error, "link: invalid shader");
        return -1;
    }
    if (vs->stage != 0) { snprintf(vs->error, sizeof vs->error, "link: not a vertex shader"); return -1; }
    const MOJOSHADER_parseData *vpd = (const MOJOSHADER_parseData *)vs->pd;
    int vDataLen = vpd->output_len - (int)sizeof(SpirvPatchTable);
    SpirvPatchTable *table = (SpirvPatchTable *)&vpd->output[vDataLen];
    /* Input variable type patches: the same work
     * MOJOSHADER_linkSPIRVShaders does for real pairs. The renderer binds
     * declaration elements raw (blend indices as R16G16_SINT etc.), so the
     * VS input variables must carry the matching int/uint type and the
     * register loads convert on read; without this the driver rejects the
     * pipeline interface (InterfaceTypeMismatch). */
    for (int a = 0; a < vpd->attribute_count; ++a) {
        const MOJOSHADER_attribute *el = &vpd->attributes[a];
        if (el->usage < 0 || el->usage >= MOJOSHADER_USAGE_TOTAL || el->index >= 16)
            continue;
        uint32 f = formats ? formats[a] : (uint32) MOJOSHADER_VERTEXELEMENTFORMAT_VECTOR4;
        uint32 typeDecl, typeLoad, opcodeLoad;
        if (f >= MOJOSHADER_VERTEXELEMENTFORMAT_BYTE4 && f <= MOJOSHADER_VERTEXELEMENTFORMAT_SHORT4) {
            const int isByte = (f == MOJOSHADER_VERTEXELEMENTFORMAT_BYTE4);
            typeDecl = isByte ? table->tid_uvec4_p : table->tid_ivec4_p;
            typeLoad = isByte ? table->tid_uvec4 : table->tid_ivec4;
            opcodeLoad = isByte ? SpvOpConvertUToF : SpvOpConvertSToF;
        } else {
            typeDecl = table->tid_vec4_p;
            typeLoad = table->tid_vec4;
            opcodeLoad = SpvOpCopyObject;
        }
        uint32 typeDeclOffset = table->attrib_type_offsets[el->usage][el->index];
        if (!typeDeclOffset)
            continue; /* attribute bound but not DCL'd by the shader */
        ((uint32 *) vpd->output)[typeDeclOffset] = typeDecl;
        for (uint32 j = 0; j < table->attrib_type_load_offsets[el->usage][el->index].num_loads; ++j) {
            uint32 lt = table->attrib_type_load_offsets[el->usage][el->index].load_types[j];
            uint32 lo = table->attrib_type_load_offsets[el->usage][el->index].load_opcodes[j];
            if (lt)
                ((uint32 *) vpd->output)[lt] = typeLoad;
            if (lo) {
                uint32 *op = (uint32 *) vpd->output + lo;
                *op = (*op & 0xFFFF0000) | opcodeLoad;
            }
        }
    }
    int have_diffuse = 0, have_uv = 0, next_loc = 2;
    int i;
    for (i = 0; i < vpd->output_count; ++i) {
        const MOJOSHADER_attribute *o = &vpd->outputs[i];
        if (o->usage == MOJOSHADER_USAGE_POSITION && o->index == 0) continue;   /* gl_Position */
        if (o->usage < 0 || o->usage >= MOJOSHADER_USAGE_TOTAL || o->index >= 16) {
            snprintf(vs->error, sizeof vs->error, "fixedfunc link: output usage=%d index=%d", o->usage, o->index);
            return -1;
        }
        uint32 off = table->attrib_offsets[o->usage][o->index];
        if (!off) { snprintf(vs->error, sizeof vs->error, "fixedfunc link: no patch entry for output"); return -1; }
        /* attrib_offsets entries are WORD indices (the link API's own
         * convention: ((uint32*)output)[offset] — see
         * MOJOSHADER_spirv_link_attributes). */
        uint32 *word = (uint32 *)vpd->output + off;
        if (*word != 0xDEADBEEF) { snprintf(vs->error, sizeof vs->error, "fixedfunc link: output not linkable"); return -1; }
        if (o->usage == MOJOSHADER_USAGE_COLOR && o->index == 0) { *word = 1; have_diffuse = 1; }
        else if (o->usage == MOJOSHADER_USAGE_TEXCOORD && o->index == 0) { *word = 0; have_uv = 1; }
        else { *word = (uint32)next_loc++; }  /* unused by the fixed fragment; keeps locations distinct */
    }
    if (!have_diffuse || !have_uv) {
        snprintf(vs->error, sizeof vs->error, "fixedfunc link: need diffuse+texcoord (have %d/%d)", have_diffuse, have_uv);
        return -1;
    }
    vs->linked = 1;
    return 0;
}
size_t vkshader_spirv_bytes(const vkshader *sh) {
    if (!sh || !sh->spirv) return 0;
    return sh->spirv_bytes - sizeof(SpirvPatchTable);
}

size_t vkshader_uniform_size(const vkshader *sh) {
    if (!sh) return 0;
    size_t bytes = 0;
    for (uint32_t i = 0; i < sh->uniform_count; ++i) {
        const vkshader_uniform *u = &sh->uniforms[i];
        if (u->type == VKSHADER_UNI_FLOAT) bytes += u->count * 16;
        else if (u->type == VKSHADER_UNI_INT) bytes += u->count * 16;
        else bytes += u->count * 4;   /* bool arrays are int per vec4 */
    }
    return bytes;
}

void vkshader_pack_uniforms(const vkshader *sh, void *out,
                            const float *float4s, uint32_t float4_count,
                            const int *int4s, const int *bools) {
    if (!sh || !out) return;
    size_t cursor = 0;
    for (uint32_t i = 0; i < sh->uniform_count; ++i) {
        const vkshader_uniform *u = &sh->uniforms[i];
        if (u->type == VKSHADER_UNI_FLOAT) {
            if (!float4s || u->index + u->count > float4_count) {
                /* Register file incomplete: leave zeros (the draw still
                 * happens; the fixed pipeline is the real fallback). */
                memset((uint8_t *)out + cursor, 0, u->count * 16);
            } else {
                memcpy((uint8_t *)out + cursor, float4s + (size_t)u->index * 4, u->count * 16);
            }
            cursor += u->count * 16;
        } else if (u->type == VKSHADER_UNI_INT) {
            if (!int4s || u->index + u->count > float4_count) {
                memset((uint8_t *)out + cursor, 0, u->count * 16);
            } else {
                memcpy((uint8_t *)out + cursor, int4s + (size_t)u->index * 4, u->count * 16);
            }
            cursor += u->count * 16;
        } else { /* BOOL */
            if (!bools || u->index + u->count > float4_count) {
                memset((uint8_t *)out + cursor, 0, u->count * 4);
            } else {
                memcpy((uint8_t *)out + cursor, bools + u->index, u->count * 4);
            }
            cursor += u->count * 4;
        }
    }
}

void vkshader_destroy(vkshader *sh) {
    if (!sh) return;
    if (sh->pd) {
        /* The parseData was created with the default allocator; free it
         * the same way. The CTAB-normalized token buffers are not
         * referenced by the parseData (it copies what it needs). */
        MOJOSHADER_freeParseData((const MOJOSHADER_parseData *)sh->pd);
    }
    memset(sh, 0, sizeof *sh);
}

void vkshader_stats(uint64_t *translated, uint64_t *failed) {
    if (translated) *translated = g_translated;
    if (failed) *failed = g_failed;
}
