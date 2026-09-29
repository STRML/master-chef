/* texture_decode.c - see texture_decode.h */
#include "texture_decode.h"

/* ---- helpers ---------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t expand5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t expand6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint8_t expand4(uint32_t v) { return (uint8_t)((v << 4) | v); }

static void put(uint8_t *d, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    d[0] = b; d[1] = g; d[2] = r; d[3] = a;
}

uint32_t halo_texture_bytes_per_pixel(uint32_t format) {
    switch (format) {
    case HALO_TEXFMT_A8R8G8B8:
    case HALO_TEXFMT_X8R8G8B8: return 4;
    case HALO_TEXFMT_R5G6B5:
    case HALO_TEXFMT_A1R5G5B5:
    case HALO_TEXFMT_A4R4G4B4:
    case HALO_TEXFMT_A8L8:     return 2;
    case HALO_TEXFMT_A8:
    case HALO_TEXFMT_L8:       return 1;
    default:                   return 0;
    }
}

uint32_t halo_texture_block_size(uint32_t format) {
    switch (format) {
    case HALO_TEXFMT_DXT1: return 8;
    case HALO_TEXFMT_DXT3:
    case HALO_TEXFMT_DXT5: return 16;
    default:               return 0;
    }
}

size_t halo_texture_bgra_size(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > HALO_TEXTURE_MAX_DIM || height > HALO_TEXTURE_MAX_DIM)
        return 0;
    return (size_t)width * (size_t)height * 4u;
}

/* ---- uncompressed ----------------------------------------------------- */

static void decode_row(uint32_t format, const uint8_t *s, uint8_t *d, uint32_t width) {
    uint32_t x;
    switch (format) {
    case HALO_TEXFMT_A8R8G8B8:
        for (x = 0; x < width; x++, s += 4, d += 4) put(d, s[0], s[1], s[2], s[3]);
        break;
    case HALO_TEXFMT_X8R8G8B8:
        for (x = 0; x < width; x++, s += 4, d += 4) put(d, s[0], s[1], s[2], 0xFF);
        break;
    case HALO_TEXFMT_R5G6B5:
        for (x = 0; x < width; x++, s += 2, d += 4) {
            uint32_t v = rd16(s);
            put(d, expand5(v & 31), expand6((v >> 5) & 63), expand5(v >> 11), 0xFF);
        }
        break;
    case HALO_TEXFMT_A1R5G5B5:
        for (x = 0; x < width; x++, s += 2, d += 4) {
            uint32_t v = rd16(s);
            put(d, expand5(v & 31), expand5((v >> 5) & 31), expand5((v >> 10) & 31),
                (v & 0x8000) ? 0xFF : 0x00);
        }
        break;
    case HALO_TEXFMT_A4R4G4B4:
        for (x = 0; x < width; x++, s += 2, d += 4) {
            uint32_t v = rd16(s);
            put(d, expand4(v & 15), expand4((v >> 4) & 15), expand4((v >> 8) & 15), expand4(v >> 12));
        }
        break;
    case HALO_TEXFMT_A8:
        for (x = 0; x < width; x++, s += 1, d += 4) put(d, 0, 0, 0, s[0]);
        break;
    case HALO_TEXFMT_L8:
        for (x = 0; x < width; x++, s += 1, d += 4) put(d, s[0], s[0], s[0], 0xFF);
        break;
    case HALO_TEXFMT_A8L8:
        for (x = 0; x < width; x++, s += 2, d += 4) put(d, s[0], s[0], s[0], s[1]);
        break;
    default:
        break;
    }
}

/* ---- DXT -------------------------------------------------------------- */

/* Decode the 8-byte colour half of a block into 4 BGRA palette entries.
 * four_color forces the 4-colour mode (DXT3/5); otherwise c0<=c1 selects
 * the 3-colour + transparent mode (DXT1). */
static void dxt_palette(const uint8_t *b, int four_color, uint8_t pal[4][4]) {
    uint32_t c0 = rd16(b), c1 = rd16(b + 2);
    uint8_t r0 = expand5(c0 >> 11), g0 = expand6((c0 >> 5) & 63), b0 = expand5(c0 & 31);
    uint8_t r1 = expand5(c1 >> 11), g1 = expand6((c1 >> 5) & 63), b1 = expand5(c1 & 31);
    put(pal[0], b0, g0, r0, 0xFF);
    put(pal[1], b1, g1, r1, 0xFF);
    if (four_color || c0 > c1) {
        put(pal[2], (uint8_t)((2u * b0 + b1) / 3), (uint8_t)((2u * g0 + g1) / 3), (uint8_t)((2u * r0 + r1) / 3), 0xFF);
        put(pal[3], (uint8_t)((b0 + 2u * b1) / 3), (uint8_t)((g0 + 2u * g1) / 3), (uint8_t)((r0 + 2u * r1) / 3), 0xFF);
    } else {
        put(pal[2], (uint8_t)((b0 + b1) / 2), (uint8_t)((g0 + g1) / 2), (uint8_t)((r0 + r1) / 2), 0xFF);
        put(pal[3], 0, 0, 0, 0x00);
    }
}

static void dxt5_alpha_palette(uint8_t a0, uint8_t a1, uint8_t pal[8]) {
    pal[0] = a0;
    pal[1] = a1;
    if (a0 > a1) {
        pal[2] = (uint8_t)((6u * a0 + 1u * a1) / 7);
        pal[3] = (uint8_t)((5u * a0 + 2u * a1) / 7);
        pal[4] = (uint8_t)((4u * a0 + 3u * a1) / 7);
        pal[5] = (uint8_t)((3u * a0 + 4u * a1) / 7);
        pal[6] = (uint8_t)((2u * a0 + 5u * a1) / 7);
        pal[7] = (uint8_t)((1u * a0 + 6u * a1) / 7);
    } else {
        pal[2] = (uint8_t)((4u * a0 + 1u * a1) / 5);
        pal[3] = (uint8_t)((3u * a0 + 2u * a1) / 5);
        pal[4] = (uint8_t)((2u * a0 + 3u * a1) / 5);
        pal[5] = (uint8_t)((1u * a0 + 4u * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
}

/* Decode one block into out[16][4] (row-major 4x4, BGRA). */
static void decode_block(uint32_t format, const uint8_t *blk, uint8_t out[16][4]) {
    uint8_t pal[4][4];
    uint8_t alpha[16];
    const uint8_t *color = blk;
    uint32_t i, indices;

    if (format == HALO_TEXFMT_DXT1) {
        dxt_palette(blk, 0, pal);
        for (i = 0; i < 16; i++) alpha[i] = 0xFF; /* overridden by palette alpha below */
    } else if (format == HALO_TEXFMT_DXT3) {
        color = blk + 8;
        dxt_palette(color, 1, pal);
        for (i = 0; i < 16; i++) {
            uint32_t nib = (blk[i / 2] >> ((i & 1) * 4)) & 15;
            alpha[i] = expand4(nib);
        }
    } else { /* DXT5 */
        uint8_t apal[8];
        uint64_t bits = 0;
        color = blk + 8;
        dxt_palette(color, 1, pal);
        dxt5_alpha_palette(blk[0], blk[1], apal);
        for (i = 0; i < 6; i++) bits |= (uint64_t)blk[2 + i] << (8 * i);
        for (i = 0; i < 16; i++) alpha[i] = apal[(bits >> (3 * i)) & 7];
    }

    indices = rd32(color + 4);
    for (i = 0; i < 16; i++) {
        uint32_t idx = (indices >> (2 * i)) & 3;
        out[i][0] = pal[idx][0];
        out[i][1] = pal[idx][1];
        out[i][2] = pal[idx][2];
        out[i][3] = (format == HALO_TEXFMT_DXT1) ? pal[idx][3] : alpha[i];
    }
}

/* ---- entry point ------------------------------------------------------ */

int halo_texture_decode(uint32_t format, uint32_t width, uint32_t height,
                        const void *source, size_t source_size, size_t source_pitch,
                        void *bgra, size_t bgra_size) {
    /* DXT2/4 retain the stored premultiplied RGB; their block layout is DXT3/5. */
    if(format==0x32545844u)format=HALO_TEXFMT_DXT3;
    if(format==0x34545844u)format=HALO_TEXFMT_DXT5;
    const uint8_t *src = (const uint8_t *)source;
    uint8_t *dst = (uint8_t *)bgra;
    uint32_t bpp = halo_texture_bytes_per_pixel(format);
    uint32_t bsz = halo_texture_block_size(format);
    size_t out_needed;

    if (bpp == 0 && bsz == 0) return HALO_TEXDEC_E_FORMAT;
    out_needed = halo_texture_bgra_size(width, height);
    if (out_needed == 0) return HALO_TEXDEC_E_DIMENSION;
    if (source == NULL || bgra == NULL) return HALO_TEXDEC_E_NULL;
    if (bgra_size < out_needed) return HALO_TEXDEC_E_OUTPUT_SIZE;

    if (bpp != 0) {
        size_t row_bytes = (size_t)width * bpp;
        size_t y;
        if (source_pitch == 0) source_pitch = row_bytes;
        if (source_pitch < row_bytes) return HALO_TEXDEC_E_PITCH;
        /* rows 0..h-2 need a full pitch, last row needs row_bytes */
        if (height > 1 && source_pitch > (SIZE_MAX-row_bytes)/(height-1))
            return HALO_TEXDEC_E_SOURCE_SIZE;
        if (source_size < (size_t)(height - 1) * source_pitch + row_bytes)
            return HALO_TEXDEC_E_SOURCE_SIZE;
        for (y = 0; y < height; y++)
            decode_row(format, src + y * source_pitch, dst + y * (size_t)width * 4, width);
        return HALO_TEXDEC_OK;
    } else {
        uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4;
        size_t row_bytes = (size_t)bw * bsz;
        size_t out_pitch = (size_t)width * 4;
        uint32_t by, bx;
        if (source_pitch == 0) source_pitch = row_bytes;
        if (source_pitch < row_bytes) return HALO_TEXDEC_E_PITCH;
        if (bh > 1 && source_pitch > (SIZE_MAX-row_bytes)/(bh-1))
            return HALO_TEXDEC_E_SOURCE_SIZE;
        if (source_size < (size_t)(bh - 1) * source_pitch + row_bytes)
            return HALO_TEXDEC_E_SOURCE_SIZE;
        for (by = 0; by < bh; by++) {
            const uint8_t *row = src + (size_t)by * source_pitch;
            for (bx = 0; bx < bw; bx++) {
                uint8_t px[16][4];
                uint32_t iy, ix;
                decode_block(format, row + (size_t)bx * bsz, px);
                for (iy = 0; iy < 4; iy++) {
                    uint32_t y = by * 4 + iy;
                    if (y >= height) break;
                    for (ix = 0; ix < 4; ix++) {
                        uint32_t x = bx * 4 + ix;
                        uint8_t *d;
                        if (x >= width) break;
                        d = dst + y * out_pitch + (size_t)x * 4;
                        d[0] = px[iy * 4 + ix][0];
                        d[1] = px[iy * 4 + ix][1];
                        d[2] = px[iy * 4 + ix][2];
                        d[3] = px[iy * 4 + ix][3];
                    }
                }
            }
        }
        return HALO_TEXDEC_OK;
    }
}
