#ifndef HALO_TEXTURE_MOD_PACK_H
#define HALO_TEXTURE_MOD_PACK_H
/* Read-only native texture pack. Explicit little-endian fields, checked before
 * any pixel access. CRC is TexMod's uncomplemented IEEE CRC32 of mip zero. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#if defined(__ARM_FEATURE_CRC32) && defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_acle.h>
#define HTM_NATIVE_CRC32 1
#else
#define HTM_NATIVE_CRC32 0
#endif
typedef struct { const uint8_t *data; size_t size; uint32_t count; } HaloTexturePack;
typedef struct { uint32_t hash, width, height; const uint8_t *bgra; size_t bytes; } HaloTextureMod;
/* Packs are demand-mapped, not copied into resident memory. Full-game artwork
 * exceeds 2 GiB; retain a finite file limit and guard size_t conversion. */
static int htm_file_size_valid(uint64_t size) {
    return size>=32 && size<=UINT64_C(4294967296) && size<=SIZE_MAX;
}
static uint32_t htm_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static uint64_t htm_u64(const uint8_t *p) { return htm_u32(p) | (uint64_t)htm_u32(p+4)<<32; }
static int htm_open(HaloTexturePack *pack, const void *data, size_t size) {
    *pack=(HaloTexturePack){0};
    if(!data || size<32)return 0;
    const uint8_t *p=data;
    uint32_t count=htm_u32(p+8);
    if(memcmp(p,"HVTEX001",8) || !count || count>65536 || htm_u32(p+12)!=32 ||
       htm_u64(p+16)!=32 || htm_u64(p+24)!=size || count>(size-32)/32)return 0;
    uint64_t end=32+(uint64_t)count*32;
    uint32_t prior=0;
    for(uint32_t i=0;i<count;i++) {
        const uint8_t *e=p+32+(size_t)i*32;
        uint32_t hash=htm_u32(e), w=htm_u32(e+4), h=htm_u32(e+8);
        uint64_t offset=htm_u64(e+16), bytes=htm_u64(e+24);
        if((i && hash<=prior) || !w || !h || w>8192 || h>8192 || htm_u32(e+12)!=1 ||
           bytes!=(uint64_t)w*h*4 || offset<end || offset>size || bytes>size-offset)return 0;
        prior=hash;end=offset+bytes;
    }
    *pack=(HaloTexturePack){p,size,count};return 1;
}
/* Return a one-based entry index; zero means keep the original texture. */
static uint32_t htm_find(const HaloTexturePack *pack, uint32_t hash) {
    uint32_t lo=0,hi=pack->count;
    while(lo<hi){uint32_t mid=lo+(hi-lo)/2, found=htm_u32(pack->data+32+(size_t)mid*32);
        if(found<hash)lo=mid+1;else hi=mid;}
    return lo<pack->count && htm_u32(pack->data+32+(size_t)lo*32)==hash?lo+1:0;
}
static HaloTextureMod htm_entry(const HaloTexturePack *pack, uint32_t index) {
    if(!index || index>pack->count)return (HaloTextureMod){0};
    const uint8_t *e=pack->data+32+(size_t)(index-1)*32;
    return (HaloTextureMod){htm_u32(e),htm_u32(e+4),htm_u32(e+8),
        pack->data+(size_t)htm_u64(e+16),(size_t)htm_u64(e+24)};
}
static void htm_crc_table(uint32_t table[256]) {
    for(uint32_t i=0;i<256;i++){uint32_t c=i;for(unsigned b=0;b<8;b++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);table[i]=c;}
}
static uint32_t htm_crc_scalar(const uint32_t table[256], const uint8_t *data, size_t size) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<size;i++)crc=(crc>>8)^table[(crc^data[i])&255u];
    return crc;
}
static uint32_t htm_crc(const uint32_t table[256], const uint8_t *data, size_t size) {
    uint32_t crc=0xffffffffu;
#if HTM_NATIVE_CRC32
    /* IEEE CRC32, not CRC32C. The pack stores the uncomplemented result.
     * memcpy permits unaligned input; bounded loads never read past mip zero.
     * Only targets advertising the CRC extension compile this path. */
    (void)table;
    while(size>=8){uint64_t word;memcpy(&word,data,8);crc=__crc32d(crc,word);data+=8;size-=8;}
    while(size--){crc=__crc32b(crc,*data++);}
#else
    crc=htm_crc_scalar(table,data,size);
#endif
    return crc;
}
#endif
