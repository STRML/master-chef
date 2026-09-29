#ifndef HALO_TEXTURE_MIPS_H
#define HALO_TEXTURE_MIPS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Exact rounded BGRA box filter, including NPOT edges and padded rows. The
 * common 2x2 path sums independent 16-bit lanes: no carries between channels.
 * It preserves the prior mip pixels while avoiding four repeated box walks. */
static inline void halo_texture_mip(const uint8_t *source, uint32_t width,
                                    uint32_t height, size_t pitch, uint8_t *out) {
    uint32_t nw=width>1?width/2:1, nh=height>1?height/2:1;
    if(width>1 && height>1 && !(width&1) && !(height&1)) {
        for(uint32_t y=0;y<nh;y++) for(uint32_t x=0;x<nw;x++) {
            uint32_t a,b,c,d;
            const uint8_t *row=source+(size_t)(2*y)*pitch+8*x;
            memcpy(&a,row,4);memcpy(&b,row+4,4);memcpy(&c,row+pitch,4);memcpy(&d,row+pitch+4,4);
            uint32_t rb=(a&0x00ff00ffu)+(b&0x00ff00ffu)+(c&0x00ff00ffu)+(d&0x00ff00ffu);
            uint32_t ga=((a>>8)&0x00ff00ffu)+((b>>8)&0x00ff00ffu)+((c>>8)&0x00ff00ffu)+((d>>8)&0x00ff00ffu);
            uint32_t pixel=(((rb+0x00020002u)>>2)&0x00ff00ffu)|((((ga+0x00020002u)>>2)&0x00ff00ffu)<<8);
            memcpy(out+((size_t)y*nw+x)*4,&pixel,4);
        }
        return;
    }
    for(uint32_t y=0;y<nh;y++) for(uint32_t x=0;x<nw;x++) {
        uint32_t x0=x*width/nw,x1=(x+1)*width/nw,y0=y*height/nh,y1=(y+1)*height/nh;
        uint32_t count=(x1-x0)*(y1-y0),sum[4]={0};
        for(uint32_t sy=y0;sy<y1;sy++) for(uint32_t sx=x0;sx<x1;sx++)
            for(unsigned k=0;k<4;k++)sum[k]+=source[(size_t)sy*pitch+sx*4+k];
        for(unsigned k=0;k<4;k++)out[((size_t)y*nw+x)*4+k]=(uint8_t)((sum[k]+count/2)/count);
    }
}
#endif
