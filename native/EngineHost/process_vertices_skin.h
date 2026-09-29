#ifndef HALO_PROCESS_VERTICES_SKIN_H
#define HALO_PROCESS_VERTICES_SKIN_H
#include "process_vertices.h"
/* The complete recorded VS1.1 stream, not a guest object address. Any change
 * (DCL, masks, swizzles, constants, comments/version) uses generic execution. */
static inline int pv_skin_shader_matches(const uint32_t *words,size_t bytes) {
    static const uint32_t signature[]={0xfffe0101u,0x1fu,0x80000000u,0x900f0000u,0x1fu,0x80000003u,0x900f0001u,0x1fu,0x80000007u,0x900f0002u,0x1fu,0x80000006u,0x900f0003u,0x1fu,0x80000005u,0x900f0004u,0x1fu,0x80000002u,0x900f0005u,0x1fu,0x80000001u,0x900f0006u,0x5u,0x80030000u,0x90e40005u,0xa0ff0009u,0x2u,0x80030000u,0x80e40000u,0xa0ff0005u,0x1u,0xb0010000u,0x80000000u,0x5u,0x800f0004u,0x90000006u,0xa0e4201du,0x5u,0x800f0005u,0x90000006u,0xa0e4201eu,0x5u,0x800f0006u,0x90000006u,0xa0e4201fu,0x1u,0xb0010000u,0x80550000u,0x4u,0x800f0004u,0x90550006u,0xa0e4201du,0x80e40004u,0x4u,0x800f0005u,0x90550006u,0xa0e4201eu,0x80e40005u,0x4u,0x800f0006u,0x90550006u,0xa0e4201fu,0x80e40006u,0x9u,0xc0010000u,0x90e40000u,0x80e40004u,0x9u,0xc0020000u,0x90e40000u,0x80e40005u,0x9u,0xc0040000u,0x90e40000u,0x80e40006u,0x1u,0xc0080000u,0x90ff0004u,0x1u,0xe00f0000u,0x90e40004u,0xffffu};
    return words&&bytes==sizeof signature&&!memcmp(words,signature,bytes);
}
/* Match host.h -> engine_cpu.h strict unfused arithmetic and instruction
 * ordering, including failed relative reads and the active FP environment. */
static inline int pv_skin_execute(const PVProgram *p,const float in[16][4],PVOutput *out) {
#pragma STDC FENV_ACCESS ON
#pragma STDC FP_CONTRACT OFF
    memset(out,0,sizeof *out);float r0[4]={0},mixed[3][4];
    /* Original MUL/ADD evaluate all four lanes before masking r0.xy. */
    for(unsigned c=0;c<4;c++){float value=in[5][c]*p->constants[9][3];if(c<2)r0[c]=value;}
    for(unsigned c=0;c<4;c++){float value=r0[c]+p->constants[5][3];if(c<2)r0[c]=value;}
    for(unsigned b=0;b<2;b++) {
        float x=r0[b];
        if(!isfinite(x)||x<-2147483648.f||x>=2147483648.f)return 0;
        x=floorf(x);
        /* Preserve per-read failure order, even zero-weight reads. Bone zero's
         * three MUL instructions execute before the second MOV to a0. */
        if(x<-256.f||x>255.f)return 0;int address=(int)x;
        for(unsigned row=0;row<3;row++) {
            int index=29+address+(int)row;if(index<0||index>=256)return 0;
            for(unsigned c=0;c<4;c++) {
                if(!b)mixed[row][c]=in[6][0]*p->constants[index][c];
                else mixed[row][c]=in[6][1]*p->constants[index][c]+mixed[row][c];
            }
        }
    }
    for(unsigned row=0;row<3;row++) {
        float dot=0;for(unsigned c=0;c<4;c++)dot+=in[0][c]*mixed[row][c];out->position[row]=dot;
    }
    out->position[3]=in[4][3];memcpy(out->tex[0],in[4],16);return 1;
}
/* Differential mode keeps generic authority even after the comparison cap. */
static inline PVExecutor pv_skin_dispatch_executor(const uint32_t *words,size_t bytes,int fast_enabled,int differential_enabled) {
    return fast_enabled&&!differential_enabled&&pv_skin_shader_matches(words,bytes)?pv_skin_execute:pv_execute;
}
/* Exact tokens are required whenever candidate output is authoritative. */
static inline int pv_process_skin_dispatch(const PVProgram *p,const uint32_t *words,size_t shader_bytes,
                             const uint8_t *decl,size_t decl_bytes,const PVStream streams[16],
                             uint32_t start,uint32_t count,uint8_t *dest,size_t dest_bytes,
                             uint32_t dest_index,uint32_t fvf,uint32_t flags,int fast_enabled,int differential_enabled) {
    PVExecutor execute=pv_skin_dispatch_executor(words,shader_bytes,fast_enabled,differential_enabled);
    return pv_process_execute(p,decl,decl_bytes,streams,start,count,dest,dest_bytes,dest_index,fvf,flags,execute);
}
#endif
