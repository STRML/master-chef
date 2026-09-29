/* Recognize the original Halo POSITIONT declarations as existing RHW layouts.
 * SetVertexDeclaration correctly leaves GetFVF at zero; this is a draw-local
 * interpretation, not a change to the bound D3D device state. */
#ifndef HALO_RHW_DECLARATION_H
#define HALO_RHW_DECLARATION_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint32_t halo_rhw_declaration_fvf(const void *elements, size_t bytes) {
    /* Original executable tables 0065E380 / 0065E3A0, created by 005301B0.
     * D3DVERTEXELEMENT9 fields: stream, offset, type, method, usage, index.
     * The byte arrays avoid alignment/host-structure assumptions. */
    static const uint8_t diffuse_uv[] = {
        0,0,  0,0, 3,0, 9,0,  /* FLOAT4 POSITIONT0 */
        0,0, 16,0, 4,0,10,0,  /* D3DCOLOR COLOR0 */
        0,0, 20,0, 1,0, 5,0,  /* FLOAT2 TEXCOORD0 */
      255,0,  0,0,17,0, 0,0   /* D3DDECL_END */
    };
    static const uint8_t diffuse_specular_uv[] = {
        0,0,  0,0, 3,0, 9,0,
        0,0, 16,0, 4,0,10,0,
        0,0, 20,0, 4,0,10,1,  /* D3DCOLOR COLOR1 */
        0,0, 24,0, 1,0, 5,0,
      255,0,  0,0,17,0, 0,0
    };
    if (!elements) return 0;
    if (bytes == sizeof diffuse_uv && !memcmp(elements, diffuse_uv, bytes)) return 0x144u;
    if (bytes == sizeof diffuse_specular_uv && !memcmp(elements, diffuse_specular_uv, bytes)) return 0x1C4u;
    return 0;
}
#endif
