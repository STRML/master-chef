/* Bounded decoder check against the first Ogg stream in Halo's owned sounds.map. */
#define STB_VORBIS_NO_STDIO
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-compare"
#include "../third_party/stb_vorbis.c"
#pragma clang diagnostic pop
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv){
    assert(argc==2);FILE*f=fopen(argv[1],"rb");assert(f);enum{SCAN=4*1024*1024};uint8_t*b=malloc(SCAN);assert(b);size_t n=fread(b,1,SCAN,f);fclose(f);
    size_t start=0;while(start+4<n&&memcmp(b+start,"OggS",4))start++;assert(start+4<n);
    int error=0;stb_vorbis*v=stb_vorbis_open_memory(b+start,(int)(n-start),&error,NULL);assert(v);
    stb_vorbis_info info=stb_vorbis_get_info(v);assert(info.channels>=1&&info.channels<=2);assert(info.sample_rate>=11025&&info.sample_rate<=48000);
    int16_t pcm[8192];int frames=stb_vorbis_get_samples_short_interleaved(v,info.channels,pcm,8192);assert(frames>0);long long energy=0;for(int i=0;i<frames*info.channels;i++)energy+=(long long)pcm[i]*pcm[i];assert(energy>0);
    printf("Halo Vorbis decoder test passed: %d channels, %u Hz, %d frames, energy %lld\n",info.channels,info.sample_rate,frames,energy);
    stb_vorbis_close(v);free(b);return 0;
}
