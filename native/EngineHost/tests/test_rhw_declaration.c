#include "../rhw_declaration.h"
#include <assert.h>
#include <stdio.h>

static void element(uint8_t *out, unsigned stream, unsigned offset,
                    unsigned type, unsigned usage, unsigned index) {
    out[0] = stream & 255; out[1] = stream >> 8;
    out[2] = offset & 255; out[3] = offset >> 8;
    out[4] = type; out[5] = 0; out[6] = usage; out[7] = index;
}
static void original_layout(uint8_t out[40], int specular) {
    memset(out, 0, 40);
    element(out, 0, 0, 3, 9, 0);
    element(out + 8, 0, 16, 4, 10, 0);
    if (specular) element(out + 16, 0, 20, 4, 10, 1);
    element(out + (specular ? 24 : 16), 0, specular ? 24 : 20, 1, 5, 0);
    element(out + (specular ? 32 : 24), 255, 0, 17, 0, 0);
}
int main(void) {
    uint8_t declaration[40], before[40];
    for (int specular = 0; specular <= 1; specular++) {
        size_t bytes = specular ? 40 : 32;
        uint32_t expected = specular ? 0x1C4 : 0x144;
        original_layout(declaration, specular);
        memcpy(before, declaration, sizeof before);
        assert(halo_rhw_declaration_fvf(declaration, bytes) == expected);
        assert(!memcmp(before, declaration, sizeof before));
        /* Original 3D POSITION, not POSITIONT, must never use RHW. */
        declaration[6] = 0;
        assert(!halo_rhw_declaration_fvf(declaration, bytes));
        memcpy(declaration, before, sizeof before);
        declaration[4] = 2; /* FLOAT3 cannot supply reciprocal W. */
        assert(!halo_rhw_declaration_fvf(declaration, bytes));
        memcpy(declaration, before, sizeof before);
        declaration[5] = 1; /* Non-default declaration method. */
        assert(!halo_rhw_declaration_fvf(declaration, bytes));
        memcpy(declaration, before, sizeof before);
        declaration[8] = 1; /* Diffuse in another stream needs a different reader. */
        assert(!halo_rhw_declaration_fvf(declaration, bytes));
        memcpy(declaration, before, sizeof before);
        declaration[(specular ? 24 : 16) + 2] += 4; /* Different UV offset. */
        assert(!halo_rhw_declaration_fvf(declaration, bytes));
        memcpy(declaration, before, sizeof before);
        assert(!halo_rhw_declaration_fvf(declaration, bytes - 1));
        assert(!halo_rhw_declaration_fvf(declaration, bytes - 8));
        assert(!halo_rhw_declaration_fvf(declaration, bytes + 1));
        assert(!halo_rhw_declaration_fvf(NULL, bytes));
    }
    /* Real BSP layout: FLOAT3 POSITION/NORMAL plus FLOAT2 UV is untouched. */
    element(declaration, 0, 0, 2, 0, 0);
    element(declaration + 8, 0, 12, 2, 3, 0);
    element(declaration + 16, 0, 24, 1, 5, 0);
    element(declaration + 24, 255, 0, 17, 0, 0);
    assert(!halo_rhw_declaration_fvf(declaration, 32));
    puts("PASS: original POSITIONT layouts 0065E380/0065E3A0, immutable input, exact offsets and stream/type validation; ordinary BSP POSITION excluded");
}
