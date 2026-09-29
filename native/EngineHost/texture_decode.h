/* texture_decode.h - decode Direct3D9 surface bytes into tightly packed BGRA8.
 *
 * Plain C99. No heap, no platform APIs. Safe to call from any thread.
 *
 * Output layout: width*height*4 bytes, row-major, pixel bytes in memory
 * order B, G, R, A (i.e. a little-endian D3DFMT_A8R8G8B8 pixel).
 * Output is tightly packed: output pitch == width*4.
 *
 * Input layout:
 *   Uncompressed formats: `height` rows, each `source_pitch` bytes apart,
 *   with at least width*bytes_per_pixel valid bytes per row. The last row
 *   only needs width*bpp bytes (trailing pad after the last row is optional).
 *   Block-compressed formats: ceil(height/4) block rows, each `source_pitch`
 *   bytes apart, with at least ceil(width/4)*block_size valid bytes per row.
 *   Pixels outside width/height in edge blocks are discarded (cropped mips).
 *
 * `source_pitch` of 0 means "tightly packed" (width*bpp, or blocks*block_size).
 */
#ifndef HALO_TEXTURE_DECODE_H
#define HALO_TEXTURE_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* D3DFORMAT values accepted. */
#define HALO_TEXFMT_A8R8G8B8   21u
#define HALO_TEXFMT_X8R8G8B8   22u
#define HALO_TEXFMT_R5G6B5     23u
#define HALO_TEXFMT_A1R5G5B5   25u
#define HALO_TEXFMT_A4R4G4B4   26u
#define HALO_TEXFMT_A8         28u
#define HALO_TEXFMT_L8         50u
#define HALO_TEXFMT_A8L8       51u
#define HALO_TEXFMT_DXT1       0x31545844u /* 'DXT1' little-endian FOURCC */
#define HALO_TEXFMT_DXT3       0x33545844u /* 'DXT3' */
#define HALO_TEXFMT_DXT5       0x35545844u /* 'DXT5' */

#define HALO_TEXTURE_MAX_DIM   8192u

/* Error codes (all nonzero). 0 means success. */
enum {
    HALO_TEXDEC_OK              = 0,
    HALO_TEXDEC_E_FORMAT        = 1, /* format not supported */
    HALO_TEXDEC_E_DIMENSION     = 2, /* width/height is 0 or > HALO_TEXTURE_MAX_DIM */
    HALO_TEXDEC_E_NULL          = 3, /* source or bgra pointer is NULL */
    HALO_TEXDEC_E_PITCH         = 4, /* source_pitch smaller than a row's data */
    HALO_TEXDEC_E_SOURCE_SIZE   = 5, /* source_size too small for the described rows */
    HALO_TEXDEC_E_OUTPUT_SIZE   = 6  /* bgra_size < width*height*4 */
};

/* Returns HALO_TEXDEC_OK or one of the HALO_TEXDEC_E_* codes. On error the
 * output buffer contents are unspecified (may be partially written). */
int halo_texture_decode(uint32_t format,
                        uint32_t width,
                        uint32_t height,
                        const void *source,
                        size_t source_size,
                        size_t source_pitch,
                        void *bgra,
                        size_t bgra_size);

/* Bytes needed for the BGRA output of a width x height surface, or 0 if the
 * dimensions are invalid. */
size_t halo_texture_bgra_size(uint32_t width, uint32_t height);

/* Bytes per pixel for uncompressed formats, or 0 for compressed/unknown. */
uint32_t halo_texture_bytes_per_pixel(uint32_t format);

/* Block size in bytes (8 or 16) for DXT formats, or 0 otherwise. */
uint32_t halo_texture_block_size(uint32_t format);

#ifdef __cplusplus
}
#endif

#endif /* HALO_TEXTURE_DECODE_H */
