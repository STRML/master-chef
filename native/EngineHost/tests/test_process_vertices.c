#include "../process_vertices.h"
#include <stdio.h>
#include <assert.h>

static void *read_file(const char *dir,const char *name,size_t *size){
    char path[2048];snprintf(path,sizeof path,"%s/%s",dir,name);FILE *f=fopen(path,"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    void *b=malloc((size_t)n);assert(b&&fread(b,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return b;
}
int main(int argc,char **argv){
    assert(argc==2);size_t vs_size,decl_size,src_size,constants_size;
    uint32_t *shader=read_file(argv[1],"vs-0272CE20.bin",&vs_size);
    uint8_t *decl=read_file(argv[1],"decl-027201D0.bin",&decl_size);
    uint8_t *src=read_file(argv[1],"draw-000.vertices.bin",&src_size);
    float (*constants)[4]=read_file(argv[1],"draw-000.vs-constants.bin",&constants_size);
    assert(constants_size==4096&&src_size%68==0);
    unsigned count=(unsigned)(src_size/68);size_t out_size=(count+2)*32;
    uint8_t *out=malloc(out_size),*before=malloc(out_size);assert(out&&before);
    memset(out,0xA5,out_size);memcpy(before,out,out_size);
    PVProgram p;assert(pv_compile(&p,shader,vs_size,constants));
    PVStream streams[16]={{src,src_size,0,68}};
    assert(pv_process(&p,decl,decl_size,streams,0,count,out,out_size,1,0x112,1));
    assert(!memcmp(out,before,32)&&!memcmp(out+(count+1)*32,before+(count+1)*32,32));
    for(unsigned i=0;i<count;i++){
        float position[3],weight[2],uv[2],actual[3];int16_t bones[2];
        memcpy(position,src+i*68,12);memcpy(bones,src+i*68+56,4);memcpy(weight,src+i*68+60,8);
        memcpy(uv,src+i*68+48,8);memcpy(actual,out+(i+1)*32,12);
        for(unsigned row=0;row<3;row++){
            double expected=0;
            for(unsigned b=0;b<2;b++){
                unsigned ci=29+3*bones[b]+row;assert(ci<256);
                double transformed=constants[ci][3];
                for(unsigned c=0;c<3;c++)transformed+=position[c]*constants[ci][c];
                expected+=weight[b]*transformed;
            }
            assert(fabs(actual[row]-expected)<2e-5);
        }
        assert(!memcmp(out+(i+1)*32+12,before+(i+1)*32+12,12));
        assert(!memcmp(out+(i+1)*32+24,uv,8));
    }
    memcpy(before,out,out_size);
    assert(!pv_process(&p,decl,decl_size,streams,0,count,out,out_size-64,1,0x112,1));
    assert(!pv_process(&p,decl,decl_size,streams,count,1,out,out_size,0,0x112,1));
    assert(!pv_process(&p,decl,decl_size,streams,0,count,out,out_size,0,0x112,0));
    assert(!memcmp(before,out,out_size));
    /* A bad second vertex must not commit the first successfully skinned one. */
    int16_t invalid[2]={32767,32767};memcpy(src+68+56,invalid,4);
    assert(!pv_process(&p,decl,decl_size,streams,0,count,out,out_size,1,0x112,1));
    assert(!memcmp(before,out,out_size));
    float decoded[4];const int16_t signed_indices[2]={-1,7};pv_decode((const uint8_t*)signed_indices,6,decoded);
    assert(decoded[0]==-1&&decoded[1]==7&&decoded[2]==0&&decoded[3]==1);
    /* Product of start/count and stride must never wrap range validation. */
    PVStream huge[16]={{src,4,0,UINT32_MAX}};PVInput ignored[16];
    assert(!pv_inputs(&p,decl,decl_size,huge,UINT32_MAX-3,6,ignored));
    free(out);free(before);free(src);free(shader);free(decl);free(constants);
    printf("PASS: %u captured Halo vertices match independent bone transforms; UVs, preserved normals, offsets and atomic failure verified.\n",count);
}
