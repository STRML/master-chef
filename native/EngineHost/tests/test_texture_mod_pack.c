#include "../texture_mod_pack.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
static uint32_t scalar_crc(const uint32_t table[256],const uint8_t *data,size_t size){
    uint32_t crc=0xffffffffu;for(size_t i=0;i<size;i++)crc=(crc>>8)^table[(crc^data[i])&255u];return crc;
}
static volatile uint32_t crc_sink;
static void check_crc(const uint32_t table[256]){
    uint8_t data[8208];uint32_t random=0x9e3779b9u;unsigned cases=0;
    for(size_t i=0;i<sizeof data;i++){random^=random<<13;random^=random>>17;random^=random<<5;data[i]=(uint8_t)random;}
    for(size_t offset=0;offset<16;offset++)for(size_t n=0;n<=8192;n++){
        assert(htm_crc(table,data+offset,n)==scalar_crc(table,data+offset,n));cases++;
    }
    /* Exact-size buffers let ASan catch overreads at every word-tail length. */
    for(size_t n=1;n<=256;n++){uint8_t *p=malloc(n);assert(p);memcpy(p,data,n);assert(htm_crc(table,p,n)==scalar_crc(table,p,n));free(p);cases++;}
    const size_t size=4u*1024*1024;uint8_t *large=malloc(size);assert(large);
    for(size_t i=0;i<size;i++)large[i]=data[i%sizeof data];
    assert(htm_crc(table,large,size)==scalar_crc(table,large,size));cases++;
    double times[2];
    for(unsigned fast=0;fast<2;fast++){
        clock_t start=clock();for(unsigned i=0;i<16;i++){large[0]=(uint8_t)i;crc_sink^=fast?htm_crc(table,large,size):scalar_crc(table,large,size);}
        times[fast]=1000.0*(clock()-start)/CLOCKS_PER_SEC/16;
    }
    free(large);printf("CRC: %u differential cases, native=%d, 4 MiB scalar %.3f ms / selected %.3f ms (CPU microbenchmark, not FPS)\n",cases,HTM_NATIVE_CRC32,times[0],times[1]);
}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(n>>(i*8));}
int main(void){
    assert(!htm_file_size_valid(31));assert(htm_file_size_valid(32));
    if(SIZE_MAX>UINT32_MAX){
        assert(htm_file_size_valid(UINT64_C(2147483649)));
        assert(htm_file_size_valid(UINT64_C(4294967296)));
    }
    assert(!htm_file_size_valid(UINT64_C(4294967297)));
    assert(!htm_file_size_valid(UINT64_MAX));
    uint32_t table[256];htm_crc_table(table);
    assert(htm_crc(table,(const uint8_t *)"123456789",9)==0x340bc6d9u);
    assert(htm_crc(table,NULL,0)==0xffffffffu);
    check_crc(table);
    uint8_t bytes[100]={0};memcpy(bytes,"HVTEX001",8);
    put32(bytes+8,2);put32(bytes+12,32);put64(bytes+16,32);put64(bytes+24,sizeof bytes);
    for(int i=0;i<2;i++){uint8_t *e=bytes+32+32*i;put32(e,10+10*i);put32(e+4,1);put32(e+8,1);put32(e+12,1);put64(e+16,96+4*i);put64(e+24,4);}
    /* Truncation and overlapping payloads are rejected before lookup. */
    HaloTexturePack p;assert(!htm_open(&p,bytes,sizeof bytes));
    uint8_t full[104];memcpy(full,bytes,100);memset(full+100,0,4);put64(full+24,104);
    assert(htm_open(&p,full,sizeof full));assert(htm_find(&p,10)==1 && htm_find(&p,20)==2);
    assert(!htm_find(&p,0) && !htm_find(&p,15) && !htm_find(&p,21));
    HaloTextureMod m=htm_entry(&p,2);assert(m.width==1 && m.height==1 && m.bgra==full+100 && m.bytes==4);
    assert(!htm_entry(&p,0).bgra && !htm_entry(&p,3).bgra);
    for(size_t i=0;i<sizeof full;i++)assert(!htm_open(&p,full,i));
    uint8_t bad[104];
    memcpy(bad,full,104);put64(bad+80,UINT64_MAX);assert(!htm_open(&p,bad,104));
    memcpy(bad,full,104);put32(bad+64,10);assert(!htm_open(&p,bad,104));
    memcpy(bad,full,104);put32(bad+68,8193);assert(!htm_open(&p,bad,104));
    memcpy(bad,full,104);put64(bad+80,96);assert(!htm_open(&p,bad,104));
    memcpy(bad,full,104);put32(bad+8,UINT32_MAX);assert(!htm_open(&p,bad,104));
    const char *path=getenv("HALO_TEST_TEXTURE_PACK");
    if(path){FILE *f=fopen(path,"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);assert(size>0 && htm_file_size_valid((uint64_t)size));uint8_t *data=mmap(NULL,(size_t)size,PROT_READ,MAP_PRIVATE,fileno(f),0);assert(data!=MAP_FAILED);fclose(f);
        assert(htm_open(&p,data,(size_t)size));for(uint32_t i=1;i<=p.count;i++){m=htm_entry(&p,i);assert(htm_find(&p,m.hash)==i);assert(m.bytes==(size_t)m.width*m.height*4);}
        printf("Validated %u imported entries\n",p.count);munmap(data,(size_t)size);}
    puts("PASS texture pack: TexMod CRC, lookups, bounds, truncation, overlap, duplicate rejection");
}
