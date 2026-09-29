#ifndef HALO_SHADER_MOD_PACK_H
#define HALO_SHADER_MOD_PACK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define HSM_MAX_ENTRIES 128
#define HSM_MAX_BYTES (4u*1024u*1024u)
typedef struct { const uint8_t *original,*replacement; uint32_t original_size,replacement_size; } HaloShaderMod;
typedef struct { uint32_t count; HaloShaderMod entries[HSM_MAX_ENTRIES]; } HaloShaderPack;
static inline uint32_t hsm_u32(const void *p) { const uint8_t *b=p;return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24); }
/* Bounded ps_2_0 token walk. The release gate also translates and compiles
 * every replacement with the production MojoShader/Metal path. */
static inline int hsm_shader(const uint8_t *b,uint32_t size) {
    if(size<8 || size>262144 || (size&3) || hsm_u32(b)!=0xffff0200u)return 0;
    uint32_t words=size/4;
    for(uint32_t i=1;i<words;) {
        uint32_t t=hsm_u32(b+4*i),op=t&65535u,n=(t>>24)&15u;
        if(op==65535u)return i==words-1;
        if(op==65534u)n=(t>>16)&32767u;
        else if(op>96u || !n)return 0;
        if(n>=words-i)return 0;
        i+=n+1;
    }
    return 0;
}
static inline int hsm_open(HaloShaderPack *pack,const void *data,size_t size) {
    memset(pack,0,sizeof *pack);
    if(size<16 || size>HSM_MAX_BYTES || memcmp(data,"HVSHD001",8))return 0;
    const uint8_t *b=data;uint32_t count=hsm_u32(b+8);size_t offset=16;
    if(!count || count>HSM_MAX_ENTRIES || hsm_u32(b+12))return 0;
    for(uint32_t i=0;i<count;i++) {
        if(size-offset<8)return 0;
        uint32_t a=hsm_u32(b+offset),n=hsm_u32(b+offset+4);offset+=8;
        if(a>size-offset)return 0;
        const uint8_t *old=b+offset;offset+=a;
        if(n>size-offset)return 0;
        const uint8_t *replacement=b+offset;offset+=n;
        if(!hsm_shader(old,a) || !hsm_shader(replacement,n))return 0;
        for(uint32_t j=0;j<i;j++)if(pack->entries[j].original_size==a && !memcmp(pack->entries[j].original,old,a))return 0;
        pack->entries[i]=(HaloShaderMod){old,replacement,a,n};
    }
    if(offset!=size)return 0;
    pack->count=count;return 1;
}
static inline const HaloShaderMod *hsm_find(const HaloShaderPack *pack,const void *original,uint32_t size) {
    for(uint32_t i=0;i<pack->count;i++) {
        const HaloShaderMod *m=&pack->entries[i];
        if(m->original_size==size && !memcmp(m->original,original,size))return m;
    }
    return NULL;
}
#endif
