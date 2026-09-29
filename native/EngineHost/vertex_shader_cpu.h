#ifndef HALO_VERTEX_SHADER_CPU_H
#define HALO_VERTEX_SHADER_CPU_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

/* Bounded software execution of the original VS 1.1 instruction stream for
 * ProcessVertices. Unsupported instructions fail before destination writes. */
typedef struct { uint32_t op,dst,src[3]; } PVInstruction;
typedef struct {
    PVInstruction instructions[256]; unsigned count;
    uint8_t usage[16],usage_index[16],input_used[16],input_declared[16];
    uint8_t position_mask,color_mask[2],tex_mask[8],point_mask;
    float constants[256][4];
} PVProgram;
typedef struct { float position[4],color[2][4],tex[8][4],fog,point_size; } PVOutput;
static inline unsigned pv_register_type(uint32_t token) {
    return ((token>>28)&7u)|((token>>8)&24u);
}
static inline int pv_compile(PVProgram *p,const uint32_t *words,size_t bytes,const float constants[256][4]) {
    if(!p||!words||bytes<8||bytes%4||words[0]!=0xfffe0101u)return 0;
    memset(p,0,sizeof *p);memcpy(p->constants,constants,sizeof p->constants);
    size_t count=bytes/4,i=1;
    while(i<count) {
        uint32_t token=words[i++],op=token&65535u;
        if(op==65535)return p->count>0;
        if(op==65534){size_t n=(token>>16)&32767u;if(n>count-i)return 0;i+=n;continue;}
        if(op==31){if(count-i<2)return 0;unsigned n=words[i+1]&2047u;
            if(pv_register_type(words[i+1])!=1||n>=16)return 0;
            p->usage[n]=words[i]&31u;p->usage_index[n]=(words[i]>>16)&15u;p->input_declared[n]=1;i+=2;continue;}
        if(op==81){if(count-i<5||pv_register_type(words[i])!=2||(words[i]&2047u)>=256)return 0;
            memcpy(p->constants[words[i]&2047u],words+i+1,16);i+=5;continue;}
        if(op==0)continue;
        unsigned n;
        switch(op){case 1:case 6:case 7:case 14:case 15:case 19:n=1;break;
            case 2:case 3:case 5:case 8:case 9:case 10:case 11:case 12:case 13:n=2;break;
            case 4:case 18:n=3;break;default:return 0;}
        if(count-i<1+n||p->count>=256||(token&0xff000000u))return 0;
        PVInstruction *ins=&p->instructions[p->count++];ins->op=op;ins->dst=words[i++];
        unsigned dt=pv_register_type(ins->dst),di=ins->dst&2047u;
        if(!((dt==0&&di<12)||(dt==3&&di==0)||(dt==4&&di<3)||(dt==5&&di<2)||(dt==6&&di<8)))return 0;
        if(ins->dst&0x0f000000u)return 0; /* destination shifts unsupported */
        if((ins->dst>>20&15u)>1)return 0;
        unsigned mask=ins->dst>>16&15u;
        if(dt==4&&di==0)p->position_mask|=mask;
        if(dt==4&&di==2)p->point_mask|=mask&1;
        if(dt==5)p->color_mask[di]|=mask;
        if(dt==6)p->tex_mask[di]|=mask;
        for(unsigned k=0;k<n;k++) {
            uint32_t src=ins->src[k]=words[i++];unsigned st=pv_register_type(src),si=src&2047u;
            if(!((st==0&&si<12)||(st==1&&si<16)||(st==2&&si<256)||(st==3&&si==0)))return 0;
            if((src>>24&15u)>1||((src&0x2000u)&&st!=2))return 0;
            if(st==1)p->input_used[si]=1;
        }
    }
    return 0;
}
static inline int pv_execute(const PVProgram *p,const float input[16][4],PVOutput *out) {
    float temp[12][4]={{0}},address[4]={0};memset(out,0,sizeof *out);
    for(unsigned i=0;i<p->count;i++) {
        const PVInstruction *ins=&p->instructions[i];float a[3][4]={{0}},r[4]={0};
        unsigned n=ins->op==4||ins->op==18?3:
            (ins->op==1||ins->op==6||ins->op==7||ins->op==14||ins->op==15||ins->op==19)?1:2;
        for(unsigned k=0;k<n;k++) {
            uint32_t token=ins->src[k];unsigned type=pv_register_type(token);int index=token&2047u;
            if(token&0x2000u){if(!isfinite(address[0])||address[0]<-256||address[0]>255)return 0;index+=(int)address[0];}
            if(type==2&&(index<0||index>=256))return 0;
            const float *v=type==0?temp[index]:type==1?input[index]:type==2?p->constants[index]:address;
            for(unsigned c=0;c<4;c++){a[k][c]=v[(token>>(16+c*2))&3u];if((token>>24&15u)==1)a[k][c]=-a[k][c];}
        }
        float dot=0;if(ins->op==8||ins->op==9)for(unsigned c=0;c<(ins->op==8?3u:4u);c++)dot+=a[0][c]*a[1][c];
        for(unsigned c=0;c<4;c++) switch(ins->op) {
            case 1:r[c]=a[0][c];break;case 2:r[c]=a[0][c]+a[1][c];break;case 3:r[c]=a[0][c]-a[1][c];break;
            case 4:r[c]=a[0][c]*a[1][c]+a[2][c];break;case 5:r[c]=a[0][c]*a[1][c];break;
            case 6:r[c]=1.f/a[0][0];break;case 7:r[c]=1.f/sqrtf(fabsf(a[0][0]));break;
            case 8:case 9:r[c]=dot;break;case 10:r[c]=fminf(a[0][c],a[1][c]);break;case 11:r[c]=fmaxf(a[0][c],a[1][c]);break;
            case 12:r[c]=a[0][c]<a[1][c]?1.f:0.f;break;case 13:r[c]=a[0][c]>=a[1][c]?1.f:0.f;break;
            case 14:r[c]=exp2f(a[0][0]);break;case 15:r[c]=log2f(fabsf(a[0][0]));break;
            case 18:r[c]=a[0][c]*a[1][c]+(1.f-a[0][c])*a[2][c];break;
            case 19:r[c]=a[0][c]-floorf(a[0][c]);break;
        }
        unsigned dt=pv_register_type(ins->dst),di=ins->dst&2047u,mask=ins->dst>>16&15u;
        float *dst=dt==0?temp[di]:dt==3?address:dt==4?(di==0?out->position:di==1?&out->fog:&out->point_size):dt==5?out->color[di]:out->tex[di];
        if(dt==4&&di>0)mask&=1;
        for(unsigned c=0;c<4;c++)if(mask&(1u<<c)) {
            /* VS 1.1 MOV to a0 floors; later MOVA has distinct rounding. */
            if(dt==3){if(!isfinite(r[c])||r[c]<-2147483648.f||r[c]>=2147483648.f)return 0;r[c]=floorf(r[c]);}
            dst[c]=(ins->dst&0x00100000u)?fminf(1.f,fmaxf(0.f,r[c])):r[c];
        }
    }
    return 1;
}
#endif
