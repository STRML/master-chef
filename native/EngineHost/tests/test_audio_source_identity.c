#include "../audio_source_identity.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
    unsigned char source[8192],copy[8192];for(size_t i=0;i<sizeof source;i++)source[i]=(unsigned char)(i*73+19);memcpy(copy,source,sizeof source);
    uint64_t h=halo_audio_source_identity(source,sizeof source);assert(h==halo_audio_source_identity(copy,sizeof copy));assert(!memcmp(source,copy,sizeof source));
    copy[sizeof copy-1]^=1;assert(h!=halo_audio_source_identity(copy,sizeof copy));assert(h!=halo_audio_source_identity(source,sizeof source-1));
    puts("audio source identity: equal inputs match; final-byte/length changes differ; source preserved");return 0;
}
