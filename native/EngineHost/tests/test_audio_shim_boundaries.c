/* Offline ABI boundary regression; never starts an AudioQueue or the engine.
 * clang -O1 -g -fsanitize=address,undefined -pthread -I native/EngineReuse \
 *   -I native/EngineHost native/EngineHost/tests/test_audio_shim_boundaries.c \
 *   native/EngineHost/directsound_mixer.c native/EngineHost/haptics.c \
 *   native/EngineHost/halo_settings.c -framework AudioToolbox \
 *   -o /tmp/halo-audio-boundaries
 * /tmp/halo-audio-boundaries <owned sounds.map>
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../directsound.c"
#include "../vorbis_shim.c"

uint8_t *engine_flat_base;
static uint32_t heap_next=0x10000, frees;
static char last_log[2048];
static uint64_t log_count, eof_logs, undersized_logs, rejected_plays;
static const uint8_t *source_data;
static size_t source_length, source_position;
static uint32_t source_chunk=UINT32_MAX;
static int bad_read;
void host_log(const char *format,...) {
    va_list args;va_start(args,format);vsnprintf(last_log,sizeof last_log,format,args);va_end(args);
    log_count++;
    eof_logs+=strstr(last_log,"reason=end-of-input")!=NULL;
    undersized_logs+=strstr(last_log,"reason=undersized")!=NULL;
    rejected_plays+=strstr(last_log," play ")&&strstr(last_log,"result=80070057");
}
uint32_t guest_alloc(uint32_t n) {uint32_t result=heap_next;heap_next+=(n+15u)&~15u;assert(heap_next<64*1024*1024);return result;}
void guest_free(uint32_t p) {assert(p);frees++;}
uint32_t host_proc_address(const char *dll,const char *name){(void)dll;(void)name;return 0;}
uint32_t host_call_guest(EngineCPU *cpu,uint32_t cb,int nargs,const uint32_t *args,int callee_pops) {
    (void)cpu;(void)nargs;(void)callee_pops;
    if(cb==1){
        uint32_t n=args[2];if(bad_read)return n+1;
        if(n>source_length-source_position)n=(uint32_t)(source_length-source_position);
        if(n>source_chunk)n=source_chunk;
        memcpy(GPTR(args[0]),source_data+source_position,n);source_position+=n;return n;
    }
    if(cb==2){int64_t offset=(int64_t)((uint64_t)args[1]|(uint64_t)args[2]<<32);size_t base=args[3]==2?source_length:args[3]==1?source_position:0;source_position=base+offset;assert(source_position<=source_length);return 0;}
    if(cb==3)return (uint32_t)source_position;
    assert(cb==4);return 0;
}
static uint32_t call(HostShim fn,unsigned nargs,...) {
    EngineCPU cpu={0};cpu.gpr[4]=0x1000;S32(0x1000,0x1234);
    va_list args;va_start(args,nargs);for(unsigned i=0;i<nargs;i++)S32(0x1004+4*i,va_arg(args,uint32_t));va_end(args);
    fn(&cpu);assert(cpu.pc==0x1234);return cpu.gpr[0];
}
static void source(const uint8_t *data,size_t length,uint32_t chunk){source_data=data;source_length=length;source_position=0;source_chunk=chunk;}
static void check_input_reader(void) {
    size_t n=700013;uint8_t *data=malloc(n);assert(data);for(size_t i=0;i<n;i++)data[i]=(uint8_t)(i*19+7);
    EngineCPU cpu={0};int length=0;
    for(unsigned seek=0;seek<2;seek++)for(unsigned prefix=0;prefix<2;prefix++) {
        source(data,n,7777);uint32_t initial=prefix?guest_alloc(91):0;
        if(prefix){memcpy(GPTR(initial),data,91);source_position=91;}
        uint32_t before=frees;
        uint8_t *copy=read_source(&cpu,1,initial,prefix?91:0,1,seek?2:0,seek?3:0,&length);
        assert(copy&&length==(int)n&&memcmp(copy,data,n)==0);assert(frees==before+1);free(copy);
    }
    source(data,n,7777);bad_read=1;uint32_t before=frees;
    assert(!read_source(&cpu,1,0,0,1,0,0,&length));assert(frees==before+1);bad_read=0;free(data);
    puts("PASS callback input: >512 KiB, short reads, initial prefix, seek/no-seek, invalid callback and scratch cleanup");
}
static void check_directsound(void) {
    unsetenv("HALO_AUDIO_VOICE_TRACE");
    host_dsound_get_stats(NULL,NULL,NULL);assert(!host_dsound_trace_enabled());
    assert(pthread_mutex_lock(&mixer.lock)==0);pthread_mutex_unlock(&mixer.lock);
    setenv("HALO_AUDIO_VOICE_TRACE","1",1);uint64_t logs=log_count;
    host_dsound_get_stats(NULL,NULL,NULL);assert(host_dsound_trace_enabled()&&log_count>=logs+2);
    assert(strstr(last_log,"summary frames=0")&&strstr(last_log,"detail-dropped=0"));
    logs=log_count;host_dsound_get_stats(NULL,NULL,NULL);assert(log_count==logs);
    atomic_store_explicit(&output_frames,VOICE_TRACE_WINDOW_FRAMES,memory_order_relaxed);
    host_dsound_get_stats(NULL,NULL,NULL);assert(log_count>logs&&strstr(last_log,"summary frames=240000"));
    uint32_t fmt=guest_alloc(18),desc=guest_alloc(36),out=guest_alloc(4),positions=guest_alloc(8),locks=guest_alloc(16);
    for(unsigned channels=1;channels<=2;channels++) {
        DsMixerFormat format={1,channels,16,channels*2,22050,22050*channels*2};write_format(fmt,&format);
        S32(desc,36);S32(desc+4,DSBCAPS_CTRLVOLUME);S32(desc+8,8192);S32(desc+16,fmt);
        assert(call(host_dsound_device_3,4,0u,desc,out,0u)==0);uint32_t guest=G32(out);DsObject *o=object_from_guest(guest);assert(o);
        /* A stopped buffer's two cursors are one position; the write lead is
         * whole frames once it plays. */
        assert(call(host_dsound_buffer_4,3,guest,positions,positions+4)==0);
        assert(G32(positions)==0&&G32(positions+4)==0);
        assert(call(host_dsound_buffer_12,4,guest,0u,0u,1u)==0);
        assert(call(host_dsound_buffer_4,3,guest,positions,positions+4)==0);
        assert(G32(positions+4)==1102u*channels*2&&G32(positions+4)%format.block_align==0);
        assert(call(host_dsound_buffer_11,8,guest,0u,format.block_align*4u,locks,locks+4,locks+8,locks+12,1u)==0);
        assert(G32(locks)-o->guest_data==G32(positions+4));assert(G32(locks+4)%format.block_align==0);
        memset(GPTR(G32(locks)),0,G32(locks+4));
        assert(call(host_dsound_buffer_19,5,guest,G32(locks),G32(locks+4),G32(locks+8),G32(locks+12))==0);
        assert(o->unlock_count==1&&o->unlock_nonzero==0);
        assert(call(host_dsound_buffer_11,8,guest,8188u,8u,locks,locks+4,locks+8,locks+12,0u)==0);
        assert(G32(locks+4)==4&&G32(locks+12)==4);
        memset(GPTR(G32(locks)),0x11,4);memset(GPTR(G32(locks+8)),0x22,4);
        assert(call(host_dsound_buffer_19,5,guest,G32(locks),4u,G32(locks+8),4u)==0);
        assert(o->unlock_count==2&&o->unlock_nonzero==4);
        assert(!memcmp(o->voice.data+8188,"\x11\x11\x11\x11",4));assert(o->voice.data[0]==0x22);
        assert(call(host_dsound_buffer_12,4,guest,0u,0u,128u)==DSERR_INVALIDPARAM);
        assert(call(host_dsound_buffer_13,2,guest,8192u)==DSERR_INVALIDPARAM);
        assert(call(host_dsound_buffer_12,4,guest,0u,0u,1u)==0);
        assert(call(host_dsound_buffer_13,2,guest,100u)==0);double saved=o->voice.cursor_frames;
        assert(call(host_dsound_buffer_18,1,guest)==0&&o->voice.cursor_frames==saved);
        assert(call(host_dsound_buffer_12,4,guest,0u,0u,0u)==0&&o->voice.cursor_frames==saved);
        assert(call(host_dsound_buffer_2,1,guest)==0);
    }
    assert(rejected_plays==2);
    DsMixerFormat f={1,1,16,2,48000,96000};DsObject *o=object_new(0,8,&f,0);assert(o);
    int16_t pcm[]={16384,16384,16384,16384};memcpy(o->voice.data,pcm,sizeof pcm);float mixed[16];
    assert(call(host_dsound_buffer_12,4,o->guest,0u,0u,0u)==0);ds_mixer_render(&mixer,mixed,5,48000);
    assert(!o->voice.playing&&o->voice.cursor_frames==4&&o->voice.end_count==1);
    assert(call(host_dsound_buffer_13,2,o->guest,0u)==0);
    assert(call(host_dsound_buffer_12,4,o->guest,0u,0u,0u)==0);ds_mixer_render(&mixer,mixed,1,48000);
    assert(mixed[0]==0.5f&&o->voice.playing&&o->voice.end_count==1);
    assert(call(host_dsound_buffer_2,1,o->guest)==0);
    atomic_store_explicit(&output_frames,VOICE_TRACE_WINDOW_FRAMES*2u,memory_order_relaxed);
    VoiceTrace bounded={0};bounded.frames=VOICE_TRACE_WINDOW_FRAMES*2u;
    logs=log_count;for(unsigned i=0;i<VOICE_TRACE_EVENT_LIMIT+32;i++)voice_trace_event("fixture",bounded,0,DS_OK);
    assert(log_count-logs==VOICE_TRACE_EVENT_LIMIT&&atomic_load_explicit(&voice_trace_event_dropped,memory_order_relaxed)==32);
    puts("PASS DirectSound ABI: engine-flag late enable, five-second summary cadence, 256-event detail cap, mono/stereo cursor alignment, split refill, rejected operations, Stop/Play resume, natural-end count and explicit seek replay");
}
static uint8_t *ogg_fixture(const char *path,size_t *length) {
    FILE *f=fopen(path,"rb");assert(f);size_t cap=4*1024*1024;uint8_t *data=malloc(cap);assert(data);
    size_t n=fread(data,1,cap,f);fclose(f);size_t start=0;while(start+4<n&&memcmp(data+start,"OggS",4))start++;assert(start+27<n);
    size_t pos=start;for(;;){assert(pos+27<n&&!memcmp(data+pos,"OggS",4));uint32_t seg=data[pos+26];size_t page=27+seg;assert(pos+page<n);for(uint32_t i=0;i<seg;i++)page+=data[pos+27+i];assert(pos+page<=n);int end=data[pos+5]&4;pos+=page;if(end)break;}
    *length=pos-start;memmove(data,data+start,*length);return data;
}
static void check_vorbis(const char *path) {
    size_t n;uint8_t *data=ogg_fixture(path,&n);source(data,n,7777);
    uint32_t vf=guest_alloc(1024),output=guest_alloc(8192),bitstream=guest_alloc(4);
    assert(call(host_vorbis_open_callbacks,8,1u,vf,0u,0u,1u,0u,4u,0u)==0);
    HostVorbis *s=find_stream(vf);assert(s&&s->input_bytes==n&&s->channels==2);uint64_t generation=s->generation;
    for(uint32_t i=0;i<4;i++)assert(call(host_vorbis_read,7,vf,output,i,0u,2u,1u,bitstream)==0&&!s->eof_logged&&s->decoded_bytes==0);
    assert(undersized_logs==4&&eof_logs==0);
    assert(call(host_vorbis_read,7,vf,output,5u,0u,2u,1u,bitstream)==4&&!s->eof_logged);
    uint64_t total=4;for(;;){uint32_t got=call(host_vorbis_read,7,vf,output,8192u,0u,2u,1u,bitstream);assert(got<=8192&&got%4==0);if(!got)break;total+=got;}
    assert(s->eof_logged&&eof_logs==1&&total==s->decoded_bytes&&total>8192);
    assert(call(host_vorbis_read,7,vf,output,8192u,0u,2u,1u,bitstream)==0&&eof_logs==1);
    assert(call(host_vorbis_clear,1,vf)==0&&!find_stream(vf));
    assert(call(host_vorbis_clear,1,vf)==(uint32_t)OV_EFAULT);
    source(data,n,UINT32_MAX);assert(call(host_vorbis_open_callbacks,8,1u,vf,0u,0u,1u,2u,4u,3u)==0);s=find_stream(vf);assert(s->generation>generation);
    uint64_t fresh=0;for(;;){uint32_t got=call(host_vorbis_read,7,vf,output,8192u,0u,2u,1u,bitstream);assert(got<=8192);if(!got)break;fresh+=got;}
    assert(fresh==total);assert(call(host_vorbis_clear,1,vf)==0);free(data);
    printf("PASS Vorbis ABI: %zu-byte owned Ogg, undersized reads do not consume/mark EOF; full drain %llu bytes matches fresh stream; close/reuse generation\n",n,(unsigned long long)total);
}
int main(int argc,char **argv) {
    assert(argc==2);engine_flat_base=calloc(1,64*1024*1024);assert(engine_flat_base);
    check_input_reader();check_directsound();check_vorbis(argv[1]);
    assert(!output_queue && !output_requested);
    HostDsOutputDiagnostics before,after;host_dsound_get_output_diagnostics(&before);
    uint64_t cursor_before=objects[1].voice.cursor_frames;
    host_dsound_pause_output();host_dsound_get_output_diagnostics(&after);
    assert(after.suspended && after.restart_required && after.callback_suspended && after.generation==before.generation);
    assert(objects[1].voice.cursor_frames==cursor_before);
    assert(host_dsound_resume_output());host_dsound_get_output_diagnostics(&after);
    assert(!after.suspended && after.last_recovery==0 && after.generation==before.generation);
    atomic_store(&output_last_enqueue,-12345);host_dsound_get_output_diagnostics(&after);assert(after.last_enqueue==-12345);
    atomic_store(&output_terminated,1);assert(!host_dsound_resume_output());host_dsound_get_output_diagnostics(&after);
    assert(after.terminated && after.last_recovery==-1 && after.last_enqueue==-12345);
    assert(objects[1].voice.cursor_frames==cursor_before);
    puts("PASS output diagnostic snapshots: pause/resume/terminated recovery, atomic enqueue error, no queue or voice mutation");
    free(engine_flat_base);
    puts("PASS offline audio boundaries; no engine/queue/device playback claim");return 0;
}
