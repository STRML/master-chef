#ifndef HALO_PROCESS_VERTICES_H
#define HALO_PROCESS_VERTICES_H
#include "vertex_shader_cpu.h"
#include <stdlib.h>

typedef struct { const uint8_t *bytes; size_t size; uint32_t offset,stride; } PVStream;
typedef struct { unsigned offset,type,stream; } PVInput;
typedef struct { unsigned offset,components,usage,index; } PVField;
typedef struct { PVField fields[13]; unsigned count,stride; } PVLayout;

static inline unsigned pv_type_bytes(unsigned t) {
    static const uint8_t sizes[]={4,8,12,16,4,4,4,8,4,4,8,4,8,0,0,4,8};
    return t<sizeof sizes?sizes[t]:0;
}
static inline float pv_half(uint16_t h) {
    unsigned e=(h>>10)&31,m=h&1023;
    float f=e==31?(m?NAN:INFINITY):e?ldexpf(1.f+m/1024.f,(int)e-15):ldexpf((float)m,-24);
    return h&32768?-f:f;
}
static inline void pv_decode(const uint8_t *b,unsigned t,float v[4]) {
    v[0]=v[1]=v[2]=0;v[3]=1;
    if(t<=3){memcpy(v,b,(t+1)*4);return;}
    if(t==4){v[0]=b[2]/255.f;v[1]=b[1]/255.f;v[2]=b[0]/255.f;v[3]=b[3]/255.f;return;}
    if(t==5||t==8){for(unsigned i=0;i<4;i++)v[i]=t==8?b[i]/255.f:b[i];return;}
    unsigned n=t==7||t==10||t==12||t==16?4:2;
    for(unsigned i=0;i<n;i++){
        uint16_t u;memcpy(&u,b+2*i,2);
        if(t==15||t==16)v[i]=pv_half(u);
        else if(t==11||t==12)v[i]=u/65535.f;
        else {int16_t s;memcpy(&s,&u,2);v[i]=t==9||t==10?fmaxf(-1.f,s/32767.f):s;}
    }
}
static inline int pv_inputs(const PVProgram *p,const uint8_t *decl,size_t decl_bytes,
                            const PVStream streams[16],uint32_t start,uint32_t count,PVInput inputs[16]) {
    if(!decl||decl_bytes<8||decl_bytes%8||!count)return 0;
    int ended=0;unsigned elements=0;
    for(size_t i=0;i+8<=decl_bytes;i+=8){
        const uint8_t *d=decl+i;unsigned stream=d[0]|(d[1]<<8);
        if(stream==255&&d[4]==17){ended=1;break;}
        if(stream>=16||d[5]||!pv_type_bytes(d[4]))return 0;elements++;
    }
    if(!ended)return 0;
    for(unsigned r=0;r<16;r++)if(p->input_used[r]){
        int found=0;
        for(unsigned j=0;j<elements;j++){
            const uint8_t *d=decl+8*j;
            /* Old VS 1.1 without DCL binds vN by declaration order. */
            if(p->input_declared[r]?(d[6]!=p->usage[r]||d[7]!=p->usage_index[r]):j!=r)continue;
            PVInput in={d[2]|(d[3]<<8),d[4],d[0]|(d[1]<<8)};
            const PVStream *s=&streams[in.stream];unsigned bytes=pv_type_bytes(in.type);
            if(!s->bytes||!s->stride||in.offset+bytes>s->stride)return 0;
            uint64_t base=(uint64_t)s->offset+in.offset+bytes;
            uint64_t last=(uint64_t)start+count-1;
            if(base>s->size||last>(s->size-base)/s->stride)return 0;
            inputs[r]=in;found=1;break;
        }
        if(!found)return 0;
    }
    return 1;
}
static inline void pv_field(PVLayout *l,unsigned components,unsigned usage,unsigned index) {
    l->fields[l->count++]=(PVField){l->stride,components,usage,index};
    l->stride+=components*4;
}
static inline int pv_layout(uint32_t fvf,PVLayout *l) {
    memset(l,0,sizeof *l);
    /* These are programmable stream-output buffers, not transformed XYZRHW
     * screen vertices. Reject unimplemented viewport/clipping semantics. */
    if((fvf&0x400e)!=2||(fvf&0xffffu&~0xff2u))return 0;
    pv_field(l,3,0,0);
    if(fvf&0x10)pv_field(l,3,3,0);
    if(fvf&0x20)pv_field(l,1,4,0);
    if(fvf&0x40)pv_field(l,1,10,0);
    if(fvf&0x80)pv_field(l,1,10,1);
    unsigned n=(fvf>>8)&15;if(n>8)return 0;
    for(unsigned t=0;t<n;t++){
        static const unsigned components[]={2,3,4,1};
        pv_field(l,components[(fvf>>(16+2*t))&3],5,t);
    }
    return 1;
}
static inline void pv_copy_components(uint8_t *dst,const float src[4],unsigned n,unsigned mask) {
    for(unsigned c=0;c<n;c++)if(mask&(1u<<c))memcpy(dst+c*4,src+c,4);
}
/* Atomic commit: invalid shaders, declarations, ranges or dynamic addresses
 * never leave a partially processed destination buffer. */
typedef int (*PVExecutor)(const PVProgram *,const float [16][4],PVOutput *);
static inline int pv_process_execute(const PVProgram *p,const uint8_t *decl,size_t decl_bytes,
                             const PVStream streams[16],uint32_t start,uint32_t count,
                             uint8_t *dest,size_t dest_bytes,uint32_t dest_index,uint32_t fvf,uint32_t flags,PVExecutor execute) {
    PVLayout layout;PVInput inputs[16]={{0}};
    if(!p||!dest||!execute||flags!=1||!pv_layout(fvf,&layout))return 0;
    if((uint64_t)dest_index*layout.stride>dest_bytes||
       (uint64_t)count*layout.stride>dest_bytes-(uint64_t)dest_index*layout.stride)return 0;
    if(!count)return 1;
    if(!pv_inputs(p,decl,decl_bytes,streams,start,count,inputs))return 0;
    size_t bytes=(size_t)count*layout.stride;uint8_t *scratch=malloc(bytes);
    if(!scratch)return 0;
    uint8_t *target=dest+(size_t)dest_index*layout.stride;memcpy(scratch,target,bytes);
    int ok=1;
    for(uint32_t i=0;i<count;i++){
        float input[16][4]={{0}};PVOutput output;
        for(unsigned r=0;r<16;r++)if(p->input_used[r]){
            PVInput in=inputs[r];const PVStream *s=&streams[in.stream];
            pv_decode(s->bytes+s->offset+((size_t)start+i)*s->stride+in.offset,in.type,input[r]);
        }
        if(!execute(p,input,&output)){ok=0;break;}
        for(unsigned j=0;j<layout.count;j++){
            PVField f=layout.fields[j];uint8_t *d=scratch+(size_t)i*layout.stride+f.offset;
            if(f.usage==0)pv_copy_components(d,output.position,f.components,p->position_mask);
            else if(f.usage==5)pv_copy_components(d,output.tex[f.index],f.components,p->tex_mask[f.index]);
            else if(f.usage==4&&p->point_mask)memcpy(d,&output.point_size,4);
            else if(f.usage==10&&p->color_mask[f.index]){
                uint32_t color;memcpy(&color,d,4);
                static const unsigned shifts[]={16,8,0,24};
                for(unsigned c=0;c<4;c++)if(p->color_mask[f.index]&(1u<<c)){
                    uint32_t value=(uint32_t)lroundf(fminf(1.f,fmaxf(0.f,output.color[f.index][c]))*255.f);
                    color=(color&~(255u<<shifts[c]))|(value<<shifts[c]);
                }
                memcpy(d,&color,4);
            }
            /* NORMAL and other unwritten components stay untouched with
             * D3DPV_DONOTCOPYDATA, as requested by the original engine. */
        }
    }
    if(ok)memcpy(target,scratch,bytes);free(scratch);return ok;
}
/* Original generic executor remains authoritative for the default call path. */
static inline int pv_process(const PVProgram *p,const uint8_t *decl,size_t decl_bytes,
                             const PVStream streams[16],uint32_t start,uint32_t count,
                             uint8_t *dest,size_t dest_bytes,uint32_t dest_index,uint32_t fvf,uint32_t flags) {
    return pv_process_execute(p,decl,decl_bytes,streams,start,count,dest,dest_bytes,dest_index,fvf,flags,pv_execute);
}
#endif
