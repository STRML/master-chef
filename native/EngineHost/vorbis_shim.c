/* Native implementation of the four libvorbisfile calls used by Halo.
 * The original callback data source is read through translated guest
 * callbacks, then decoded with the public-domain stb_vorbis decoder. */
#include "vorbis_shim.h"
#include "directsound.h"
#include "audio_source_identity.h"
#define STB_VORBIS_NO_STDIO
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-compare"
#include "third_party/stb_vorbis.c"
#pragma clang diagnostic pop
#include <pthread.h>
#include <stdlib.h>

enum { VORBIS_MAX_STREAMS=16, VORBIS_READ_CHUNK=65536, VORBIS_MAX_INPUT=256*1024*1024,
       OV_EREAD=-128, OV_EFAULT=-129, OV_EIMPL=-130, OV_EINVAL=-131, OV_ENOTVORBIS=-132 };
typedef struct {
    uint32_t vf, datasource, close_cb; int alive, channels;
    uint8_t *compressed; stb_vorbis *decoder;
    int16_t tail[4096], overlap[4096]; int tail_samples, overlap_samples, eof_logged;
    uint64_t generation, decoded_bytes, reads, undersized_reads, next_trace_frame;
    uint32_t input_bytes;
    uint64_t input_identity;
} HostVorbis;
static HostVorbis streams[VORBIS_MAX_STREAMS];
static pthread_mutex_t streams_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t stream_generation;
static void vorbis_failure(const char *event,uint32_t vf,uint32_t source,int result,int detail) {
    if(host_dsound_trace_enabled())host_log("[audio-voice] vorbis-%s frames=%llu vf=%08x source=%08x result=%d detail=%d",event,(unsigned long long)host_dsound_output_frames(),vf,source,result,detail);
}
#define OV_FAIL(EVENT,VF,SOURCE,RESULT,DETAIL) do { vorbis_failure(EVENT,VF,SOURCE,RESULT,DETAIL); RET_CDECL(RESULT); } while(0)

static HostVorbis *find_stream(uint32_t vf){for(int i=0;i<VORBIS_MAX_STREAMS;i++)if(streams[i].alive&&streams[i].vf==vf)return &streams[i];return NULL;}
static uint32_t guest_read(EngineCPU *cpu,uint32_t cb,uint32_t dst,uint32_t bytes,uint32_t source){uint32_t a[4]={dst,1,bytes,source};return host_call_guest(cpu,cb,4,a,0);}
static int guest_seek(EngineCPU *cpu,uint32_t cb,uint32_t source,int64_t offset,uint32_t whence){uint32_t a[4]={source,(uint32_t)offset,(uint32_t)((uint64_t)offset>>32),whence};return (int)host_call_guest(cpu,cb,4,a,0);}
static int32_t guest_tell(EngineCPU *cpu,uint32_t cb,uint32_t source){return (int32_t)host_call_guest(cpu,cb,1,&source,0);}

static uint8_t *read_source(EngineCPU *cpu,uint32_t source,uint32_t initial,uint32_t initial_bytes,uint32_t read_cb,uint32_t seek_cb,uint32_t tell_cb,int *size_out){
    if(!read_cb||initial_bytes>VORBIS_MAX_INPUT)return NULL;
    size_t used=initial_bytes,cap=initial_bytes?initial_bytes:VORBIS_READ_CHUNK;
    if(!initial_bytes&&seek_cb&&tell_cb&&guest_seek(cpu,seek_cb,source,0,2)==0){
        int32_t n=guest_tell(cpu,tell_cb,source);
        if(guest_seek(cpu,seek_cb,source,0,0)!=0)return NULL;
        if(n>0&&n<=VORBIS_MAX_INPUT)cap=(size_t)n;
    }
    uint8_t *data=malloc(cap?cap:1);if(!data)return NULL;
    if(initial_bytes)memcpy(data,GPTR(initial),initial_bytes);
    uint32_t scratch=guest_alloc(VORBIS_READ_CHUNK);if(!scratch){free(data);return NULL;}
    /* A callback may return a short read before EOF. Fill every grown buffer;
     * the old growth loop exited early whenever one chunk did not fill it. */
    for(;;){
        if(used==cap){
            if(cap==VORBIS_MAX_INPUT){
                /* Reject over-limit streams instead of silently truncating. */
                if(guest_read(cpu,read_cb,scratch,1,source))goto fail;
                break;
            }
            size_t next=cap*2;if(next>VORBIS_MAX_INPUT)next=VORBIS_MAX_INPUT;
            uint8_t *grown=realloc(data,next);if(!grown)goto fail;
            data=grown;cap=next;
        }
        uint32_t ask=(uint32_t)(cap-used);if(ask>VORBIS_READ_CHUNK)ask=VORBIS_READ_CHUNK;
        uint32_t got=guest_read(cpu,read_cb,scratch,ask,source);
        if(got>ask)goto fail;if(!got)break;
        memcpy(data+used,GPTR(scratch),got);used+=got;
    }
    guest_free(scratch);
    if(!used){free(data);return NULL;}*size_out=(int)used;return data;
fail:
    guest_free(scratch);free(data);return NULL;
}

void host_vorbis_open_callbacks(EngineCPU *cpu){
    uint32_t source=ARG(0),vf=ARG(1),initial=ARG(2),initial_bytes=ARG(3),read_cb=ARG(4),seek_cb=ARG(5),close_cb=ARG(6),tell_cb=ARG(7);
    if(!source||!vf||(initial_bytes&&!initial))OV_FAIL("open",vf,source,OV_EINVAL,0);
    int size=0;
    uint8_t *data=read_source(cpu,source,initial,initial_bytes,read_cb,seek_cb,tell_cb,&size);
    if(!data)OV_FAIL("open",vf,source,OV_EREAD,0);
    int error=0;stb_vorbis *decoder=stb_vorbis_open_memory(data,size,&error,NULL);
    if(!decoder){free(data);OV_FAIL("open",vf,source,OV_ENOTVORBIS,error);}
    stb_vorbis_info info=stb_vorbis_get_info(decoder);
    if(info.channels<1||info.channels>2){stb_vorbis_close(decoder);free(data);OV_FAIL("open",vf,source,OV_EIMPL,info.channels);}
    pthread_mutex_lock(&streams_lock);HostVorbis*s=NULL;
    if(!find_stream(vf))for(int i=0;i<VORBIS_MAX_STREAMS;i++)if(!streams[i].alive){s=&streams[i];break;}
    uint64_t generation=0,identity=host_dsound_trace_enabled()?halo_audio_source_identity(data,(size_t)size):0;
    if(s){memset(s,0,sizeof*s);s->vf=vf;s->datasource=source;s->close_cb=close_cb;s->compressed=data;s->decoder=decoder;s->channels=info.channels;s->alive=1;s->input_bytes=(uint32_t)size;s->input_identity=identity;s->generation=generation=++stream_generation;}
    pthread_mutex_unlock(&streams_lock);
    if(!s){stb_vorbis_close(decoder);free(data);OV_FAIL("open",vf,source,OV_EFAULT,0);}
    host_log("Vorbis stream opened: %d bytes, %d channels, %u Hz",size,info.channels,info.sample_rate);
    if(host_dsound_trace_enabled())host_log("[audio-voice] vorbis-open frames=%llu vf=%08x source=%08x generation=%llu input=%d channels=%d rate=%u result=0 identity-xxh3=%016llx",(unsigned long long)host_dsound_output_frames(),vf,source,(unsigned long long)generation,size,info.channels,info.sample_rate,(unsigned long long)identity);
    RET_CDECL(0);
}

void host_vorbis_read(EngineCPU *cpu){
    uint32_t vf=ARG(0),out=ARG(1);int length=(int)ARG(2),big=(int)ARG(3),word=(int)ARG(4),sign=(int)ARG(5);uint32_t bitstream=ARG(6);
    if(!out||length<0||big||word!=2||!sign)OV_FAIL("read",vf,0,OV_EINVAL,length);
    int trace=host_dsound_trace_enabled();
    pthread_mutex_lock(&streams_lock);HostVorbis*s=find_stream(vf);
    if(!s){pthread_mutex_unlock(&streams_lock);OV_FAIL("read",vf,0,OV_EFAULT,length);}
    int undersized=length<s->channels*2;
    int frames=undersized?0:stb_vorbis_get_samples_short_interleaved(s->decoder,s->channels,(short*)GPTR(out),length/2);
    int error=undersized?0:stb_vorbis_get_error(s->decoder);
    int produced=frames*s->channels;
    if(s->overlap_samples>0&&produced>0){int n=produced<s->overlap_samples?produced:s->overlap_samples;int start=s->overlap_samples-n;for(int i=0;i<n;i++){float t=(float)(i+1)/(float)(n+1);int old=s->overlap[start+i];int now=((int16_t*)GPTR(out))[i];((int16_t*)GPTR(out))[i]=(int16_t)lrintf(old*(1.f-t)+now*t);}s->overlap_samples=0;}
    if(produced>0){int keep=produced<4096?produced:4096;memcpy(s->tail,(int16_t*)GPTR(out)+produced-keep,(size_t)keep*2);s->tail_samples=keep;}
    s->reads++;s->decoded_bytes+=(uint32_t)produced*2u;if(undersized)s->undersized_reads++;
    int eof=!undersized&&!produced&&!error&&s->decoder->eof;
    uint64_t clock=host_dsound_output_frames();
    int emit=trace&&(error||(undersized&&s->undersized_reads<=8)||(eof&&!s->eof_logged)||s->reads<=2||clock>=s->next_trace_frame);
    if(eof)s->eof_logged=1;
    uint64_t generation=s->generation,decoded=s->decoded_bytes,reads=s->reads,small=s->undersized_reads,identity=s->input_identity;
    uint32_t source=s->datasource,input=s->input_bytes,offset=stb_vorbis_get_file_offset(s->decoder);
    int sample=stb_vorbis_get_sample_offset(s->decoder),channels=s->channels;
    if(emit)s->next_trace_frame=clock+48000u*5u;
    if(bitstream)S32(bitstream,0);pthread_mutex_unlock(&streams_lock);
    if(emit)host_log("[audio-voice] vorbis-read frames=%llu vf=%08x source=%08x generation=%llu requested=%d produced=%d decoded=%llu reads=%llu channels=%d reason=%s decoder-error=%d input-offset=%u input=%u sample-offset=%d undersized=%llu out=%08x identity-xxh3=%016llx",
        (unsigned long long)clock,vf,source,(unsigned long long)generation,length,produced*2,
        (unsigned long long)decoded,(unsigned long long)reads,channels,
        undersized?"undersized":error?"decoder-error":eof?"end-of-input":produced?"pcm":"zero-read",
        error,offset,input,sample,(unsigned long long)small,out,(unsigned long long)identity);
    RET_CDECL(produced*2);
}

void host_vorbis_crosslap(EngineCPU *cpu){
    pthread_mutex_lock(&streams_lock);HostVorbis*old=find_stream(ARG(0)),*next=find_stream(ARG(1));if(!old||!next){pthread_mutex_unlock(&streams_lock);OV_FAIL("crosslap",ARG(1),0,OV_EFAULT,0);}if(old->channels!=next->channels){pthread_mutex_unlock(&streams_lock);OV_FAIL("crosslap",ARG(1),0,OV_EINVAL,0);}int n=old->tail_samples;if(n>4096)n=4096;memcpy(next->overlap,old->tail+(old->tail_samples-n),(size_t)n*2);next->overlap_samples=n;pthread_mutex_unlock(&streams_lock);RET_CDECL(0);
}

void host_vorbis_clear(EngineCPU *cpu){
    uint32_t vf=ARG(0),close_cb=0,source=0;uint8_t*data=NULL;stb_vorbis*decoder=NULL;uint64_t generation=0,decoded=0,reads=0;
    pthread_mutex_lock(&streams_lock);HostVorbis*s=find_stream(vf);
    if(s){close_cb=s->close_cb;source=s->datasource;data=s->compressed;decoder=s->decoder;generation=s->generation;decoded=s->decoded_bytes;reads=s->reads;memset(s,0,sizeof*s);}
    pthread_mutex_unlock(&streams_lock);
    if(!decoder)OV_FAIL("clear",vf,0,OV_EFAULT,0);
    stb_vorbis_close(decoder);free(data);uint32_t result=0;if(close_cb)result=host_call_guest(cpu,close_cb,1,&source,0);
    if(host_dsound_trace_enabled())host_log("[audio-voice] vorbis-clear frames=%llu vf=%08x source=%08x generation=%llu decoded=%llu reads=%llu result=%d",(unsigned long long)host_dsound_output_frames(),vf,source,(unsigned long long)generation,(unsigned long long)decoded,(unsigned long long)reads,(int32_t)result);
    RET_CDECL(result);
}
