#include "metalshader.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MOJOSHADER_NO_VERSION_INCLUDE 1
#include "../../third_party/mojoshader/mojoshader.h"

static void put16(uint8_t *p, uint16_t value) { p[0] = value & 255; p[1] = value >> 8; }
static void put32(uint8_t *p, uint32_t value) {
    p[0] = value & 255; p[1] = (value >> 8) & 255; p[2] = (value >> 16) & 255; p[3] = value >> 24;
}
static uint32_t get32(const void *p) {
    const uint8_t *b = p; return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int needs_ctab(const MOJOSHADER_parseData *parsed) {
    for (int i = 0; i < parsed->error_count; ++i)
        if (parsed->errors[i].error && strstr(parsed->errors[i].error, "relative addressing unsupported without a CTAB")) return 1;
    return 0;
}

static int has_only_error(const MOJOSHADER_parseData *parsed, const char *message) {
    return parsed->error_count == 1 && parsed->errors[0].error && strstr(parsed->errors[0].error, message);
}

/* Early Halo PC ps_2_0 blobs carry a D3DX9-compiler CTAB variant whose header
 * is 20 bytes instead of the later 28-byte D3DX layout MojoShader accepts.
 * Its comment length is still standard. Strip only this recognized metadata
 * block; the version and every shader instruction token remain unchanged. */
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

/* D3DXSHADER_CONSTANTTABLE with one float4 vector array covering the complete
 * D3D register file. The comment goes after the version token, and no shader
 * instruction token is changed. */
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

static int parse_location(const char *name) {
    if (!name || name[0] != 'v' || !isdigit((unsigned char)name[1])) return -1;
    char *end = NULL; long n = strtol(name + 1, &end, 10);
    return (end && !*end && n >= 0 && n < 16) ? (int)n : -1;
}

static char *copy_and_sanitize_source(const char *source, size_t bytes) {
    char *result = malloc(bytes + 1);
    if (!result) return NULL;
    memcpy(result, source, bytes); result[bytes] = 0;
    /* Upstream Metal's CTAB-array path emits `#define cN array[N];`. The
     * semicolon becomes part of expressions. Blank only that known typo. */
    char *line = result;
    while (*line) {
        char *end = strchr(line, '\n'); if (!end) end = result + bytes;
        char *macro = strstr(line, "#define c");
        if (macro && macro < end) {
            char *uniform = strstr(line, "uniforms.uniforms_float4[");
            if (uniform && uniform < end) {
                char *last = end;
                while (last > line && isspace((unsigned char)last[-1])) --last;
                if (last > line && last[-1] == ';') last[-1] = ' ';
            }
        }
        line = *end ? end + 1 : end;
    }
    return result;
}

static void set_parse_error(ms_shader *out, const MOJOSHADER_parseData *parsed) {
    size_t used = 0;
    for (int i = 0; i < parsed->error_count && used + 4 < sizeof out->error; ++i) {
        const char *message = parsed->errors[i].error ? parsed->errors[i].error : "unknown MojoShader error";
        int n = snprintf(out->error + used, sizeof out->error - used, "%s%s@%d", i ? "; " : "", message,
                         parsed->errors[i].error_position);
        if (n < 0) break;
        used += (size_t)n < sizeof out->error - used ? (size_t)n : sizeof out->error - used - 1;
    }
}

int ms_translate_shader(const uint32_t *tokens, size_t bytes, int expected_stage,
                        const char *entry, ms_shader *out) {
    return ms_translate_shader_mapped(tokens, bytes, expected_stage, entry, NULL, 0, out);
}

int ms_translate_shader_mapped(const uint32_t *tokens, size_t bytes, int expected_stage,
                               const char *entry, const int *sampler_types, int sampler_type_count,
                               ms_shader *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!tokens || bytes < 8 || (bytes & 3) || bytes > 1024 * 1024 || !entry || !entry[0] || strlen(entry) >= sizeof out->entry) {
        snprintf(out->error, sizeof out->error, "invalid shader input"); return -1;
    }
    MOJOSHADER_samplerMap smap[16]; unsigned smapcount = 0;
    if (sampler_types && expected_stage == MS_STAGE_PIXEL && ((get32(tokens) >> 8) & 0xFF) < 2) {
        for (int i = 0; i < sampler_type_count && i < 16; ++i) {
            if (sampler_types[i] < MOJOSHADER_SAMPLER_2D || sampler_types[i] > MOJOSHADER_SAMPLER_VOLUME) continue;
            smap[smapcount].index = i; smap[smapcount].type = (MOJOSHADER_samplerType)sampler_types[i]; smapcount++;
        }
    }
    const MOJOSHADER_samplerMap *map = smapcount ? smap : NULL;
    const MOJOSHADER_parseData *parsed = MOJOSHADER_parse(MOJOSHADER_PROFILE_METAL, entry,
        (const unsigned char *)tokens, (unsigned)bytes, NULL, 0, map, smapcount, NULL, NULL, NULL);
    uint8_t *normalized = NULL, *augmented = NULL;
    const uint8_t *active = (const uint8_t *)tokens;
    size_t active_bytes = bytes;
    if (has_only_error(parsed, "Shader has corrupt CTAB data")) {
        size_t normalized_bytes = 0;
        normalized = without_legacy_ctab20((const uint8_t *)tokens, bytes, &normalized_bytes);
        if (normalized) {
            MOJOSHADER_freeParseData(parsed); active = normalized; active_bytes = normalized_bytes;
            parsed = MOJOSHADER_parse(MOJOSHADER_PROFILE_METAL, entry, active, (unsigned)active_bytes,
                                      NULL, 0, map, smapcount, NULL, NULL, NULL);
        }
    }
    if (parsed->error_count && needs_ctab(parsed)) {
        MOJOSHADER_freeParseData(parsed);
        size_t augmented_bytes = 0;
        unsigned count = expected_stage == MS_STAGE_VERTEX ? 256 : 224;
        augmented = with_register_ctab(active, active_bytes, count, &augmented_bytes);
        if (!augmented) { snprintf(out->error, sizeof out->error, "unable to synthesize constant metadata"); free(normalized); return -1; }
        parsed = MOJOSHADER_parse(MOJOSHADER_PROFILE_METAL, entry, augmented, (unsigned)augmented_bytes,
                                  NULL, 0, map, smapcount, NULL, NULL, NULL);
        out->used_synthetic_ctab = 1;
    }
    if (parsed->error_count || !parsed->output) { set_parse_error(out, parsed); MOJOSHADER_freeParseData(parsed); free(augmented); free(normalized); return -1; }
    int actual_stage = parsed->shader_type == MOJOSHADER_TYPE_VERTEX ? MS_STAGE_VERTEX :
                       parsed->shader_type == MOJOSHADER_TYPE_PIXEL ? MS_STAGE_PIXEL : 0;
    if (actual_stage != expected_stage) {
        snprintf(out->error, sizeof out->error, "shader stage mismatch"); MOJOSHADER_freeParseData(parsed); free(augmented); free(normalized); return -1;
    }
    if (parsed->uniform_count > MS_MAX_UNIFORMS || parsed->attribute_count > MS_MAX_ATTRIBUTES ||
        parsed->output_count > MS_MAX_OUTPUTS || parsed->sampler_count > MS_MAX_SAMPLERS) {
        snprintf(out->error, sizeof out->error, "shader metadata exceeds renderer bounds"); MOJOSHADER_freeParseData(parsed); free(augmented); free(normalized); return -1;
    }
    out->source = copy_and_sanitize_source(parsed->output, (size_t)parsed->output_len);
    if (!out->source) { snprintf(out->error, sizeof out->error, "shader source allocation failed"); MOJOSHADER_freeParseData(parsed); free(augmented); free(normalized); return -1; }
    out->source_bytes = (size_t)parsed->output_len; out->stage = actual_stage;
    out->major = parsed->major_ver; out->minor = parsed->minor_ver;
    snprintf(out->entry, sizeof out->entry, "%s", entry);
    out->uniform_count = parsed->uniform_count;
    for (int i = 0; i < parsed->uniform_count; ++i) {
        int count = parsed->uniforms[i].array_count ? parsed->uniforms[i].array_count : 1;
        out->uniforms[i] = (ms_binding){ parsed->uniforms[i].type, parsed->uniforms[i].index, count };
        if (parsed->uniforms[i].type == MOJOSHADER_UNIFORM_FLOAT) out->packed_float4_count += count;
        else if (parsed->uniforms[i].type == MOJOSHADER_UNIFORM_INT) out->packed_int4_count += count;
        else if (parsed->uniforms[i].type == MOJOSHADER_UNIFORM_BOOL) out->packed_bool_count += count;
    }
    out->attribute_count = parsed->attribute_count;
    for (int i = 0; i < parsed->attribute_count; ++i)
        out->attributes[i] = (ms_semantic){ parsed->attributes[i].usage, parsed->attributes[i].index, parse_location(parsed->attributes[i].name) };
    out->output_count = parsed->output_count;
    for (int i = 0; i < parsed->output_count; ++i)
        out->outputs[i] = (ms_semantic){ parsed->outputs[i].usage, parsed->outputs[i].index, -1 };
    out->sampler_count = parsed->sampler_count;
    for (int i = 0; i < parsed->sampler_count; ++i)
        out->samplers[i] = (ms_sampler){ parsed->samplers[i].type, parsed->samplers[i].index };
    MOJOSHADER_freeParseData(parsed); free(augmented); free(normalized); return 0;
}

void ms_shader_destroy(ms_shader *shader) {
    if (!shader) return; free(shader->source); memset(shader, 0, sizeof *shader);
}
