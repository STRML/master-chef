#include "../texture_content_hash.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t fnv(uint64_t hash,const void *bytes,size_t size) {
    const uint8_t *p=bytes;
    for(size_t i=0;i<size;i++){hash^=p[i];hash*=UINT64_C(1099511628211);}
    return hash;
}
static uint64_t key(const void *bytes,size_t size,uint32_t guest,uint32_t format,uint32_t w,uint32_t h) {
    uint64_t hash=UINT64_C(1469598103934665603);
    hash=halo_texture_hash_more(hash,&guest,4);hash=halo_texture_hash_more(hash,&format,4);
    hash=halo_texture_hash_more(hash,&w,4);hash=halo_texture_hash_more(hash,&h,4);
    return halo_texture_hash_more(hash,bytes,size);
}
int main(void) {
    assert(halo_texture_hash_more(0,"",0)==UINT64_C(0x2d06800538d394c2));
    assert(halo_texture_hash_more(0,"a",1)==UINT64_C(0xe6c632b61e964e1f));
    const size_t max=1024*1024;uint8_t *p=malloc(max+64),*q=malloc(max+64);assert(p&&q);
    for(size_t i=0;i<max+64;i++)p[i]=(uint8_t)(i*19+(i>>7));
    size_t sizes[]={0,1,3,4,7,8,15,16,17,31,32,63,64,127,128,239,240,241,1024,65536,1048576};
    for(size_t si=0;si<sizeof sizes/sizeof sizes[0];si++)for(size_t align=0;align<32;align++) {
        size_t n=sizes[si];memcpy(q,p+align,n);
        uint64_t before=key(p+align,n,123,21,512,512);
        assert(before==key(q,n,123,21,512,512));
        assert(before!=key(q,n,124,21,512,512));
        assert(before!=key(q,n,123,22,512,512));
        assert(before!=key(q,n,123,21,256,512));
        assert(before!=key(q,n,123,21,512,256));
        if(n)for(size_t k=0;k<3;k++) {
            size_t pos=k==0?0:k==1?n/2:n-1;q[pos]^=0x81;
            assert(before!=key(q,n,123,21,512,512));q[pos]^=0x81;
        }
    }
#ifdef HALO_HASH_BENCH
    volatile uint64_t sink=0;
    for(size_t size=4096;size<=max;size*=16) {
        size_t iterations=128*1024*1024/size;
        clock_t t=clock();for(size_t i=0;i<iterations;i++)sink^=fnv(i,p,size);
        double old=(double)(clock()-t)/CLOCKS_PER_SEC;
        t=clock();for(size_t i=0;i<iterations;i++)sink^=halo_texture_hash_more(i,p,size);
        double now=(double)(clock()-t)/CLOCKS_PER_SEC;
        printf("bytes=%zu iterations=%zu fnv=%.6f xxh3=%.6f speedup=%.2fx\n",size,iterations,old,now,old/now);
    }
    printf("sink=%llu\n",(unsigned long long)sink);
#else
    (void)fnv;
#endif
    free(p);free(q);puts("PASS: fixed xxHash vectors, unaligned inputs, size boundaries, full-content and metadata key changes.");
}
