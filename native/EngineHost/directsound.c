/* DirectSound 8 compatibility for the translated original engine.
 *
 * COM objects and writable buffer locks live in 32-bit guest memory. Audio is
 * copied into host-owned storage at Unlock, mixed as stereo float PCM, and
 * delivered by AudioQueue on both macOS and visionOS. */
#include "directsound.h"
#include "directsound_mixer.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#define DS_OK                   0u
#define E_NOINTERFACE           0x80004002u
#define DSERR_INVALIDPARAM      0x80070057u
#define DSERR_OUTOFMEMORY       0x8007000Eu
#define DSERR_UNSUPPORTED       0x80004001u
#define DSERR_CONTROLUNAVAIL    0x8878001Eu
#define DSERR_INVALIDCALL       0x88780032u
#define DSERR_BADFORMAT         0x88780064u
#define DSERR_NODRIVER          0x88780078u
#define DSERR_ALREADYINITIALIZED 0x88780082u
#define DSBCAPS_PRIMARYBUFFER   0x00000001u
#define DSBCAPS_CTRL3D          0x00000010u
#define DSBCAPS_CTRLFREQUENCY   0x00000020u
#define DSBCAPS_CTRLPAN         0x00000040u
#define DSBCAPS_CTRLVOLUME      0x00000080u
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000u
#define DSBPLAY_LOOPING         0x00000001u
#define DSBSTATUS_PLAYING       0x00000001u
#define DSBSTATUS_LOOPING       0x00000004u
#define DSLOCK_FROMWRITECURSOR  0x00000001u
#define DSLOCK_ENTIREBUFFER     0x00000002u

enum { DS_OBJECT_MAX = 1024, OUTPUT_RATE = 48000, OUTPUT_FRAMES = 2048, OUTPUT_BUFFERS = 3 };
/* Build80 recorded callback gaps up to 50 ms. Two remaining 512-frame
 * buffers cover only 21.3 ms, allowing a healthy queue to run dry without
 * an enqueue error. Keep 64 ms of reserve after each completed buffer.
 * Total queued latency is 96 ms; HALO_AUDIO_FRAMES retains A/B control. */
static uint32_t output_frames_per_buffer = 1536;

typedef struct {
    uint32_t guest, guest_3d, guest_data, refs, caps;
    uint32_t lock1, lock1_bytes, lock2, lock2_bytes;
    uint8_t primary, alive, locked;
    uint64_t unlock_count, unlock_bytes, unlock_nonzero, last_unlock_frame;
    uint64_t last_nonzero_frame, trace_unlock_count;
    uint32_t last_unlock_nonzero;
    DsMixerVoice voice;
} DsObject;

static DsMixer mixer;
static pthread_once_t mixer_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t objects_lock = PTHREAD_MUTEX_INITIALIZER;
static DsObject objects[DS_OBJECT_MAX];
static uint32_t object_count = 1, device_guest, device_refs;
static uint32_t vt_device, vt_buffer, vt_listener, vt_buffer3d;
static AudioQueueRef output_queue;
static AudioQueueBufferRef output_buffers[OUTPUT_BUFFERS];
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static int output_requested, output_suspended, output_diagnostics_initialized;
static int output_restart_required=1;
static uint32_t output_generation;
static uint64_t output_started_ns;
static int32_t output_last_prepare=-1,output_last_start=-1,output_last_pause=-1,output_last_dispose=-1,output_last_recovery=-1;
static _Atomic int32_t output_last_enqueue=-1;
/* A failed callback enqueue removes a buffer from circulation. Other buffers
 * can keep making progress, so retain the failure until the queue is rebuilt. */
static _Atomic int32_t output_enqueue_fault;
static pthread_mutex_t output_callback_lock=PTHREAD_MUTEX_INITIALIZER;
static int output_callbacks_suspended=1, output_callbacks_discarding;
static AudioQueueBufferRef output_deferred_buffers[OUTPUT_BUFFERS];
static unsigned output_deferred_count;
static _Atomic int output_terminated;
static _Atomic uint32_t watchdog_rebuilds;
/* Protected by output_callback_lock; no allocation or new I/O in callback. */
static uint64_t telemetry_callbacks, telemetry_late_callbacks, telemetry_enqueue_failures;
static uint64_t telemetry_last_callback_ns, telemetry_max_gap_ns, telemetry_max_work_ns;
static _Atomic uint64_t output_frames, output_nonzero;
static _Atomic uint32_t output_peak_bits, output_logged_active;
static FILE *capture_file;
static uint64_t capture_frames, capture_limit_frames;
static _Atomic int voice_trace_enabled;
static _Atomic uint64_t voice_trace_next_frames;
enum { VOICE_TRACE_WINDOW_FRAMES = OUTPUT_RATE * 5u, VOICE_TRACE_EVENT_LIMIT = 256 };
static _Atomic uint64_t voice_trace_event_window;
static _Atomic uint32_t voice_trace_event_count, voice_trace_event_dropped;
/* How the engine drives its voices, cumulative for the run and always on:
 * Play and Stop calls accepted, GetStatus calls and those that answered
 * PLAYING, and calls refused. The report differences them per second. The
 * engine's voice allocator asks GetStatus of each stopped voice it
 * considers (00547FF0), so busy answers with no Plays are sounds finding no
 * voice. */
static _Atomic uint64_t voice_plays, voice_stops, voice_status_probes, voice_status_busy, voice_calls_rejected;
static _Atomic uint32_t voices_playing_peak;
/* Set once the guest has created its DirectSound device; from then on the
 * engine's own sound tables are in guest memory to be read. */
static _Atomic int engine_sound_observable;
int host_dsound_trace_enabled(void) {
    if (atomic_load_explicit(&voice_trace_enabled,memory_order_relaxed)) return 1;
    const char *v=getenv("HALO_AUDIO_VOICE_TRACE");
    if (!v || !*v || !strcmp(v,"0")) return 0;
    if (!atomic_exchange_explicit(&voice_trace_enabled,1,memory_order_relaxed))
        host_log("[audio-voice] trace-enabled frames=%llu",(unsigned long long)host_dsound_output_frames());
    return 1;
}
uint64_t host_dsound_output_frames(void) {
    return atomic_load_explicit(&output_frames,memory_order_relaxed);
}
/* Snapshot at the mutation's lock boundary, emit only after releasing it. */
typedef struct {
    uint32_t guest, bytes, rate, align, channels, lock_offset, lock_bytes1, lock_bytes2, guest_data;
    uint8_t playing, looping;
    int32_t volume; double cursor;
    float gain_l, gain_r, onset_level_max; uint32_t onset_reported;
    /* Where the sound is, where the listener is, and how far apart the mixer
     * thinks they are. Distance is the whole 3D attenuation, so a listener
     * that never follows the player shows up here and nowhere else. */
    float pos[3], listener[3], distance, min_distance, max_distance;
    uint8_t has_3d, mode;
    uint64_t frames, ends, unlocks, written, nonzero, last_write, last_signal;
} VoiceTrace;
static VoiceTrace voice_trace_snapshot(DsObject *o) {
    VoiceTrace t={0}; t.frames=host_dsound_output_frames();
    if (!o) return t;
    DsMixerVoice *v=&o->voice;
    t.guest=o->guest;t.guest_data=o->guest_data; t.bytes=v->bytes; t.rate=v->format.sample_rate;
    t.align=v->format.block_align; t.channels=v->format.channels;
    t.playing=v->playing; t.looping=v->looping; t.volume=v->volume;
    t.cursor=v->cursor_frames; t.ends=v->end_count;
    t.unlocks=o->unlock_count; t.written=o->unlock_bytes; t.nonzero=o->unlock_nonzero;
    t.last_write=o->last_unlock_frame; t.last_signal=o->last_nonzero_frame;
    t.lock_offset=o->lock1;t.lock_bytes1=o->lock1_bytes;t.lock_bytes2=o->lock2_bytes;
    ds_mixer_voice_gains(&mixer,v,&t.gain_l,&t.gain_r);
    t.onset_level_max=v->onset_level_max; t.onset_reported=v->onset_reported;
    t.has_3d=v->has_3d; t.mode=(uint8_t)v->current_3d.mode;
    t.min_distance=v->current_3d.min_distance; t.max_distance=v->current_3d.max_distance;
    for(int i=0;i<3;i++){t.pos[i]=v->current_3d.position[i];t.listener[i]=mixer.current_listener.position[i];}
    float dx=t.pos[0]-(v->current_3d.mode==1?0.f:t.listener[0]);
    float dy=t.pos[1]-(v->current_3d.mode==1?0.f:t.listener[1]);
    float dz=t.pos[2]-(v->current_3d.mode==1?0.f:t.listener[2]);
    t.distance=sqrtf(dx*dx+dy*dy+dz*dz);
    return t;
}
static void voice_trace_event(const char *event, VoiceTrace t, uint32_t value, uint32_t result) {
    if (!host_dsound_trace_enabled()) return;
    uint64_t window=t.frames/VOICE_TRACE_WINDOW_FRAMES;
    uint64_t prior=atomic_load_explicit(&voice_trace_event_window,memory_order_relaxed);
    if(prior!=window&&atomic_compare_exchange_strong(&voice_trace_event_window,&prior,window)){
        atomic_store_explicit(&voice_trace_event_count,0,memory_order_relaxed);
        atomic_store_explicit(&voice_trace_event_dropped,0,memory_order_relaxed);
    }
    if(atomic_fetch_add_explicit(&voice_trace_event_count,1,memory_order_relaxed)>=VOICE_TRACE_EVENT_LIMIT){
        atomic_fetch_add_explicit(&voice_trace_event_dropped,1,memory_order_relaxed);return;
    }
    host_log("[audio-voice] %s frames=%llu guest=%08x value=%u result=%08x playing=%u looping=%u cursor=%.3f bytes=%u rate=%u channels=%u align=%u volume=%d gains=%.6g,%.6g ends=%llu unlocks=%llu written=%llu nonzero=%llu last-write=%llu last-signal=%llu lock-offset=%u lock-split=%u,%u pcm-base=%08x 3d=%u mode=%u at=%.2f,%.2f,%.2f listener=%.2f,%.2f,%.2f distance=%.3f minmax=%.3f,%.3f onset-max=%.4f onsets=%u",
        event,(unsigned long long)t.frames,t.guest,value,result,t.playing,t.looping,t.cursor,
        t.bytes,t.rate,t.channels,t.align,t.volume,t.gain_l,t.gain_r,
        (unsigned long long)t.ends,(unsigned long long)t.unlocks,(unsigned long long)t.written,
        (unsigned long long)t.nonzero,(unsigned long long)t.last_write,(unsigned long long)t.last_signal,t.lock_offset,t.lock_bytes1,t.lock_bytes2,t.guest_data,
        t.has_3d,t.mode,t.pos[0],t.pos[1],t.pos[2],t.listener[0],t.listener[1],t.listener[2],t.distance,t.min_distance,t.max_distance,t.onset_level_max,t.onset_reported);
}
static void voice_trace_failure(const char *event, uint32_t guest, uint32_t value, uint32_t result) {
    atomic_fetch_add_explicit(&voice_calls_rejected,1,memory_order_relaxed);
    VoiceTrace t={0}; t.guest=guest; t.frames=host_dsound_output_frames();
    voice_trace_event(event,t,value,result);
}
/* IDirectSoundBuffer::GetStatus. LOOPING is only ever reported together
 * with PLAYING (Windows, and Wine's dsound). Reporting it for a buffer that
 * was stopped after a looping Play is what silenced Build75: Halo plays every
 * voice looping, and its allocator (005482E0) reuses a stopped voice only
 * when 00547FF0 reads neither PLAYING nor LOOPING, so each voice started one
 * sound and was then held until the next sound_stop_all (a level load, the
 * pause menu, a checkpoint revert) cleared its stopped flag. With 24
 * positional voices that was about 35 seconds of effects after each one.
 * Caller holds mixer.lock. */
static uint32_t voice_status(const DsMixerVoice *v) {
    if (!v->playing) return 0;
    return DSBSTATUS_PLAYING | (v->looping ? DSBSTATUS_LOOPING : 0u);
}
/* Play and write cursors in bytes. The write cursor leads the play cursor
 * only while the buffer plays; a stopped buffer's cursors are the same
 * position (Wine applies its write lead only when the buffer is not
 * stopped). Halo reads the pair to decide whether a voice it left draining
 * is still running (00547C80): unequal cursors on a stopped voice made it
 * skip the Play and leave that voice silent while it waited for a play
 * cursor that never moved. Caller holds mixer.lock. */
static uint32_t voice_play_cursor(const DsMixerVoice *v) {
    uint32_t pos=v->format.block_align?(uint32_t)v->cursor_frames*v->format.block_align:0;
    if(v->bytes)pos%=v->bytes;
    return pos;
}
static uint32_t voice_write_cursor(const DsMixerVoice *v) {
    return v->playing?ds_mixer_write_cursor(v):voice_play_cursor(v);
}
#define DS_FAIL(EVENT,VALUE,RESULT,N) do { voice_trace_failure(EVENT,ARG(0),VALUE,RESULT); RET_STDCALL(RESULT,N); } while(0)

static void capture_wav_header(FILE *f, uint32_t data_bytes) {
    uint8_t h[44]={ 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                    0x80,0xbb,0,0,0xee,2,0,0,4,0,16,0,'d','a','t','a',0,0,0,0 };
    uint32_t riff=36u+data_bytes; memcpy(h+4,&riff,4); memcpy(h+40,&data_bytes,4);
    fseek(f,0,SEEK_SET); fwrite(h,1,sizeof h,f); fseek(f,0,SEEK_END);
}

static const char *device_methods[] = { "QueryInterface","AddRef","Release","CreateSoundBuffer","GetCaps","DuplicateSoundBuffer","SetCooperativeLevel","Compact","GetSpeakerConfig","SetSpeakerConfig","Initialize","VerifyCertification" };
static const char *buffer_methods[] = { "QueryInterface","AddRef","Release","GetCaps","GetCurrentPosition","GetFormat","GetVolume","GetPan","GetFrequency","GetStatus","Initialize","Lock","Play","SetCurrentPosition","SetFormat","SetVolume","SetPan","SetFrequency","Stop","Unlock","Restore","SetFX","AcquireResources","GetObjectInPath" };
static const char *listener_methods[] = { "QueryInterface","AddRef","Release","GetAllParameters","GetDistanceFactor","GetDopplerFactor","GetOrientation","GetPosition","GetRolloffFactor","GetVelocity","SetAllParameters","SetDistanceFactor","SetDopplerFactor","SetOrientation","SetPosition","SetRolloffFactor","SetVelocity","CommitDeferredSettings" };
static const char *buffer3d_methods[] = { "QueryInterface","AddRef","Release","GetAllParameters","GetConeAngles","GetConeOrientation","GetConeOutsideVolume","GetMaxDistance","GetMinDistance","GetMode","GetPosition","GetVelocity","SetAllParameters","SetConeAngles","SetConeOrientation","SetConeOutsideVolume","SetMaxDistance","SetMinDistance","SetMode","SetPosition","SetVelocity" };

static void mixer_initialize_once(void) { ds_mixer_init(&mixer); }
static uint32_t make_vtable(const char *iface, const char *const *methods, uint32_t count) {
    uint32_t table = guest_alloc(4u*count); char name[96];
    for (uint32_t i = 0; i < count; ++i) {
        snprintf(name, sizeof name, "%s::%s", iface, methods[i]);
        S32(table + 4u*i, host_proc_address("DSOUND.dll", name));
    }
    return table;
}
static void build_vtables(void) {
    if (vt_device) return;
    vt_device = make_vtable("IDirectSound8", device_methods, sizeof device_methods/sizeof *device_methods);
    vt_buffer = make_vtable("IDirectSoundBuffer8", buffer_methods, sizeof buffer_methods/sizeof *buffer_methods);
    vt_listener = make_vtable("IDirectSound3DListener8", listener_methods, sizeof listener_methods/sizeof *listener_methods);
    vt_buffer3d = make_vtable("IDirectSound3DBuffer8", buffer3d_methods, sizeof buffer3d_methods/sizeof *buffer3d_methods);
}

static void audio_callback(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)context;
    pthread_mutex_lock(&output_callback_lock);
    if (atomic_load_explicit(&output_terminated,memory_order_acquire) || output_callbacks_discarding) {
        pthread_mutex_unlock(&output_callback_lock); return;
    }
    if (output_callbacks_suspended) {
        /* A completed buffer may arrive concurrently with Pause. Retain it for
         * re-enqueue on resume, without advancing any mixer voice while paused. */
        unsigned i=0;
        while(i<output_deferred_count && output_deferred_buffers[i]!=buffer)i++;
        if(i==output_deferred_count && i<OUTPUT_BUFFERS)output_deferred_buffers[output_deferred_count++]=buffer;
        pthread_mutex_unlock(&output_callback_lock); return;
    }
    uint64_t callback_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (telemetry_last_callback_ns && callback_start >= telemetry_last_callback_ns) {
        uint64_t gap = callback_start - telemetry_last_callback_ns;
        if (gap > telemetry_max_gap_ns) telemetry_max_gap_ns = gap;
        if (gap > 2ull * output_frames_per_buffer * 1000000000ull / OUTPUT_RATE) telemetry_late_callbacks++;
    }
    telemetry_last_callback_ns = callback_start;
    telemetry_callbacks++;
    uint32_t frames = (uint32_t)(buffer->mAudioDataBytesCapacity / (2u*sizeof(float)));
    if (frames > OUTPUT_FRAMES) frames = OUTPUT_FRAMES;
    ds_mixer_render(&mixer, (float *)buffer->mAudioData, frames, OUTPUT_RATE);
    float *samples=(float *)buffer->mAudioData, peak=0.f; uint64_t nonzero=0;
    for(uint32_t i=0;i<frames*2u;i++){float a=fabsf(samples[i]);if(a>peak)peak=a;if(a>1.0e-7f)nonzero++;}
    atomic_fetch_add_explicit(&output_frames,frames,memory_order_relaxed);
    atomic_fetch_add_explicit(&output_nonzero,nonzero,memory_order_relaxed);
    uint32_t peakbits; memcpy(&peakbits,&peak,4); uint32_t old=atomic_load_explicit(&output_peak_bits,memory_order_relaxed);
    while(old<peakbits&&!atomic_compare_exchange_weak_explicit(&output_peak_bits,&old,peakbits,memory_order_relaxed,memory_order_relaxed)){}
    /* Never write host.log from the output callback: stderr shares its file
     * lock with engine/diagnostic writers and may block behind disk I/O.
     * The control-side stats sampler reports activity and limiter totals. */
    if(capture_file&&capture_frames<capture_limit_frames){
        int16_t pcm[OUTPUT_FRAMES*2];uint32_t take=frames;if(take>capture_limit_frames-capture_frames)take=(uint32_t)(capture_limit_frames-capture_frames);
        for(uint32_t i=0;i<take*2u;i++){float x=samples[i];if(x>1.f)x=1.f;if(x< -1.f)x=-1.f;pcm[i]=(int16_t)lrintf(x*32767.f);} fwrite(pcm,sizeof(int16_t),take*2u,capture_file);capture_frames+=take;
    }
    buffer->mAudioDataByteSize = frames * 2u * (uint32_t)sizeof(float);
    OSStatus status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
    atomic_store_explicit(&output_last_enqueue,status,memory_order_relaxed);
    if (status != noErr) {
        telemetry_enqueue_failures++;
        atomic_store_explicit(&output_enqueue_fault,status,memory_order_relaxed);
    }
    uint64_t work = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - callback_start;
    if (work > telemetry_max_work_ns) telemetry_max_work_ns = work;
    pthread_mutex_unlock(&output_callback_lock);
}

/* output_lock owns queue control. Finish an in-flight render before changing
 * callback state, then release callback_lock before Pause/Dispose can invoke
 * returned-buffer callbacks synchronously. Those callbacks must not mix again. */
static void audio_suspend_callbacks_locked(int discard) {
    pthread_mutex_lock(&output_callback_lock);
    output_callbacks_suspended=1; output_callbacks_discarding=discard;
    telemetry_last_callback_ns=0;
    if(discard)output_deferred_count=0;
    pthread_mutex_unlock(&output_callback_lock);
}

static void audio_dispose_locked(void) {
    audio_suspend_callbacks_locked(1);
    /* Discarding now excludes any later callback from rendering/enqueuing. */
    atomic_store_explicit(&output_enqueue_fault,0,memory_order_relaxed);
    output_restart_required=1;
    if (output_queue) {
        OSStatus status = AudioQueueDispose(output_queue, true);
        output_last_dispose=status;
        if (status != noErr) host_log("DirectSound dispose failed: OSStatus %d", (int)status);
        output_queue = NULL;
    }
    memset(output_buffers, 0, sizeof output_buffers);
}

static int audio_prepare_locked(void) {
    if (output_queue) return 1;
    if (!output_diagnostics_initialized) {
        output_diagnostics_initialized = 1;
        const char *size=getenv("HALO_AUDIO_FRAMES");
        if(size&&*size){long value=strtol(size,NULL,10);if(value>=64&&value<=OUTPUT_FRAMES)output_frames_per_buffer=(uint32_t)value;}
        host_log("DirectSound output: %u frames per buffer x %d buffers (%.1f ms)",output_frames_per_buffer,OUTPUT_BUFFERS,(double)output_frames_per_buffer*OUTPUT_BUFFERS*1000.0/OUTPUT_RATE);
        const char *capture=getenv("HALO_AUDIO_CAPTURE_WAV");
        if(capture&&*capture){capture_file=fopen(capture,"wb");capture_frames=0;
            /* Thirty seconds only reaches the opening cutscene. Weapon sound
             * questions need the window to run into gameplay. */
            capture_limit_frames=30u*OUTPUT_RATE;
            {const char *seconds=getenv("HALO_AUDIO_CAPTURE_SECONDS");
             if(seconds&&*seconds){long value=strtol(seconds,NULL,10);if(value>=1&&value<=1800)capture_limit_frames=(uint64_t)value*OUTPUT_RATE;}}if(capture_file){capture_wav_header(capture_file,0);host_log("DirectSound diagnostic capture: %s (maximum %llu seconds)",capture,(unsigned long long)(capture_limit_frames/OUTPUT_RATE));}else host_log("DirectSound diagnostic capture open failed: %s",capture);}
    }
    AudioStreamBasicDescription format = {0};
    format.mSampleRate = OUTPUT_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mBytesPerPacket = format.mBytesPerFrame = 2u*sizeof(float);
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    OSStatus status = AudioQueueNewOutput(&format, audio_callback, NULL, NULL, NULL, 0, &output_queue);
    if(status==noErr)output_generation++;
    for (int i = 0; status == noErr && i < OUTPUT_BUFFERS; ++i) {
        status = AudioQueueAllocateBuffer(output_queue, output_frames_per_buffer*2u*sizeof(float), &output_buffers[i]);
        if (status == noErr) {
            memset(output_buffers[i]->mAudioData, 0, output_frames_per_buffer*2u*sizeof(float));
            output_buffers[i]->mAudioDataByteSize = output_frames_per_buffer*2u*sizeof(float);
            status = AudioQueueEnqueueBuffer(output_queue, output_buffers[i], 0, NULL);
            atomic_store_explicit(&output_last_enqueue,status,memory_order_relaxed);
        }
    }
    output_last_prepare=status;
    if (status != noErr) {
        host_log("DirectSound queue preparation failed: OSStatus %d", (int)status);
        audio_dispose_locked();
        return 0;
    }
    return 1;
}

static int audio_resume_locked(void) {
    if (atomic_load_explicit(&output_terminated,memory_order_acquire)) return 0;
    if (!output_requested || output_suspended) return 1;
    if (!audio_prepare_locked()) return 0;
    UInt32 running=0,size=sizeof running;
    OSStatus status=AudioQueueGetProperty(output_queue,kAudioQueueProperty_IsRunning,&running,&size);
    /* IsRunning remains true for a paused AudioQueue on macOS. A known pause
     * therefore needs an explicit Start regardless of that property's value. */
    if (status==noErr && (output_restart_required || !running)) {
        pthread_mutex_lock(&output_callback_lock);
        output_callbacks_discarding=0;
        while(status==noErr && output_deferred_count) {
            AudioQueueBufferRef buffer=output_deferred_buffers[0];
            memset(buffer->mAudioData,0,buffer->mAudioDataByteSize);
            status=AudioQueueEnqueueBuffer(output_queue,buffer,0,NULL);
            atomic_store_explicit(&output_last_enqueue,status,memory_order_relaxed);
            if(status==noErr) {
                output_deferred_count--;
                memmove(output_deferred_buffers,output_deferred_buffers+1,
                        output_deferred_count*sizeof *output_deferred_buffers);
            }
        }
        if(status==noErr)output_callbacks_suspended=0;
        pthread_mutex_unlock(&output_callback_lock);
        if(status==noErr){status=AudioQueueStart(output_queue,NULL);output_last_start=status;}
        if(status==noErr) {
            output_restart_required=0;
            output_started_ns=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            host_log("DirectSound output started: stereo float PCM at %u Hz",OUTPUT_RATE);
        } else {
            output_restart_required=1;
            audio_suspend_callbacks_locked(0);
        }
    }
    if (status!=noErr) {
        host_log("DirectSound output resume failed: OSStatus %d",(int)status);
        return 0;
    }
    return 1;
}

static int audio_start(void) {
    pthread_once(&mixer_once, mixer_initialize_once);
    pthread_mutex_lock(&output_lock);
    if (atomic_load_explicit(&output_terminated,memory_order_acquire)) { pthread_mutex_unlock(&output_lock); return 0; }
    output_requested=1;
    int ok=output_suspended ? 1 : audio_resume_locked();
    pthread_mutex_unlock(&output_lock);
    return ok;
}

void host_dsound_pause_output(void) {
    pthread_mutex_lock(&output_lock);
    output_suspended=1; output_restart_required=1;
    audio_suspend_callbacks_locked(0);
    if (output_queue) {
        OSStatus status=AudioQueuePause(output_queue);
        output_last_pause=status;
        if (status!=noErr) host_log("DirectSound output pause failed: OSStatus %d",(int)status);
    }
    pthread_mutex_unlock(&output_lock);
}

int host_dsound_resume_output(void) {
    pthread_mutex_lock(&output_lock);
    output_suspended=0;
    /* A resume is a request to restart. The headset's Build60 report shows
     * the queue's IsRunning property staying true after the app had been in
     * the background while its callbacks had stopped for good; trusting the
     * property here left the mix dead until a level reload. */
    output_restart_required=1;
    int ok=audio_resume_locked();
    output_last_recovery=ok?0:-1;
    pthread_mutex_unlock(&output_lock);
    return ok;
}

static _Atomic int watchdog_external;
void host_dsound_watchdog_set_external(void) { atomic_store_explicit(&watchdog_external,1,memory_order_release); }
int host_dsound_watchdog_is_external(void) { return atomic_load_explicit(&watchdog_external,memory_order_acquire); }

/* One caller: the engine thread once a frame, or the headset app's audio queue
 * on a timer once it has taken over. Rebuild after a callback enqueue failure
 * or half a second without callback progress while playback is requested.
 * The stall path covers the Build60 report (frames frozen, IsRunning still
 * true). Rate limit both cases so a dead device does not thrash. */
void host_dsound_watchdog(void) {
    static uint64_t last_frames, last_change_ns, last_rebuild_ns;
    uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW), frames=host_dsound_output_frames();
    if(frames!=last_frames||!last_change_ns){ last_frames=frames; last_change_ns=now; }
    if(!atomic_load_explicit(&output_enqueue_fault,memory_order_relaxed) && now-last_change_ns<500000000ull) return;
    /* Keep eligibility, disposal and restart in one lifecycle transaction.
     * A failed preparation leaves no queue, but still needs a later retry. */
    pthread_mutex_lock(&output_lock);
    int playing=output_requested&&!output_suspended&&!atomic_load_explicit(&output_terminated,memory_order_acquire);
    /* Resume may have just restarted a queue after a long suspension. Give it
     * a full callback window, and recheck progress after acquiring the lock:
     * neither paused time nor time waiting behind recovery proves a stall. */
    uint64_t current_frames=host_dsound_output_frames();
    if(current_frames!=last_frames) {
        last_frames=current_frames; last_change_ns=now;
    }
    int32_t enqueue_fault=atomic_load_explicit(&output_enqueue_fault,memory_order_relaxed);
    /* Callback progress and resume grace protect a healthy queue. They cannot
     * restore a buffer lost to a known enqueue error. Recheck the fault here
     * because another lifecycle transaction may already have replaced it. */
    if(!playing || (!enqueue_fault && (now-last_change_ns<500000000ull ||
       now<=output_started_ns || now-output_started_ns<500000000ull)) ||
       now-last_rebuild_ns<2000000000ull) {
        pthread_mutex_unlock(&output_lock); return;
    }
    last_rebuild_ns=now; last_change_ns=now; atomic_fetch_add_explicit(&watchdog_rebuilds,1u,memory_order_relaxed);
    unsigned rebuilds=atomic_load_explicit(&watchdog_rebuilds,memory_order_relaxed);
    audio_dispose_locked();
    int ok=audio_resume_locked();
    output_last_recovery=ok?0:-1;
    pthread_mutex_unlock(&output_lock);
    if(enqueue_fault)
        host_log("[audio-watchdog] enqueue failed (%d) at %llu frames; rebuilt the queue (%u so far): %s",(int)enqueue_fault,(unsigned long long)current_frames,rebuilds,ok?"playing":"FAILED");
    else
        host_log("[audio-watchdog] output stalled at %llu frames; rebuilt the queue (%u so far): %s",(unsigned long long)current_frames,rebuilds,ok?"playing":"FAILED");
}
uint32_t host_dsound_watchdog_rebuilds(void) { return atomic_load_explicit(&watchdog_rebuilds,memory_order_relaxed); }

int host_dsound_rebuild_output(void) {
    pthread_mutex_lock(&output_lock);
    int ok=0;
    if (!atomic_load_explicit(&output_terminated,memory_order_acquire)) {
        audio_dispose_locked();
        ok=!output_requested || output_suspended || audio_prepare_locked();
    }
    output_last_recovery=ok?0:-1;
    pthread_mutex_unlock(&output_lock);
    return ok;
}

static DsObject *object_from_guest(uint32_t guest) {
    if (!guest) return NULL;
    uint32_t index = G32(guest + 4);
    if (!index || index >= object_count) return NULL;
    DsObject *o = &objects[index];
    return o->alive && (o->guest == guest || o->guest_3d == guest) ? o : NULL;
}
static void format_default(DsMixerFormat *f) {
    *f = (DsMixerFormat){ .tag=1, .channels=2, .bits=16, .block_align=4,
                         .sample_rate=44100, .avg_bytes_per_sec=176400 };
}
static int read_format(uint32_t address, DsMixerFormat *f) {
    if (!address) return 0;
    f->tag = G16(address); f->channels = G16(address+2); f->sample_rate = G32(address+4);
    f->avg_bytes_per_sec = G32(address+8); f->block_align = G16(address+12); f->bits = G16(address+14);
    if (f->tag == 0xfffe && G16(address+16) >= 22) {
        uint32_t subformat = G32(address+24);
        if (subformat == 1 || subformat == 3) f->tag = (uint16_t)subformat; else return 0;
    }
    uint32_t sample_bytes = f->bits/8u;
    if (!(f->channels == 1 || f->channels == 2) || f->sample_rate < 100 || f->sample_rate > 200000 ||
        !((f->tag == 1 && (f->bits == 8 || f->bits == 16)) || (f->tag == 3 && f->bits == 32)) ||
        !sample_bytes || f->block_align != f->channels*sample_bytes ||
        f->avg_bytes_per_sec != f->sample_rate*f->block_align) return 0;
    return 1;
}
static void write_format(uint32_t address, const DsMixerFormat *f) {
    S16(address, f->tag); S16(address+2, f->channels); S32(address+4, f->sample_rate);
    S32(address+8, f->avg_bytes_per_sec); S16(address+12, f->block_align); S16(address+14, f->bits); S16(address+16, 0);
}
static void voice_defaults(DsMixerVoice *v) {
    memset(v, 0, sizeof *v); v->volume = 0; v->pan = 0;
    v->current_3d.min_distance = v->deferred_3d.min_distance = 1.f;
    v->current_3d.max_distance = v->deferred_3d.max_distance = 1000000000.f;
    v->current_3d.inside_angle = v->deferred_3d.inside_angle = 360;
    v->current_3d.outside_angle = v->deferred_3d.outside_angle = 360;
}
static DsObject *object_new(uint32_t caps, uint32_t bytes, const DsMixerFormat *format, int primary) {
    pthread_once(&mixer_once, mixer_initialize_once);
    pthread_mutex_lock(&objects_lock);
    /* The limit is concurrent buffers, not every buffer ever created. Mission
     * changes and device recreation may release and rebuild the voice pool. */
    uint32_t index = 1;
    while (index < object_count && objects[index].alive) index++;
    if (index >= DS_OBJECT_MAX) { pthread_mutex_unlock(&objects_lock); return NULL; }
    DsObject *o = &objects[index]; memset(o, 0, sizeof *o);
    o->refs = 1; o->caps = caps; o->primary = primary;
    voice_defaults(&o->voice); o->voice.caps = caps; o->voice.format = *format; o->voice.frequency = format->sample_rate;
    o->voice.has_3d = (caps & DSBCAPS_CTRL3D) != 0;
    if (!primary) {
        o->voice.bytes = bytes; o->voice.data = calloc(1, bytes);
        if (!o->voice.data || !ds_mixer_add(&mixer, &o->voice)) goto failed;
        o->guest_data = guest_alloc(bytes);
        if (!o->guest_data) goto failed;
    }
    o->guest = guest_alloc(16);
    if (!o->guest) goto failed;
    S32(o->guest, vt_buffer); S32(o->guest+4, index);
    o->alive = 1;
    if (index == object_count) object_count++;
    pthread_mutex_unlock(&objects_lock); return o;
failed:
    ds_mixer_remove(&mixer, &o->voice);
    free(o->voice.data);
    if (o->guest_data) guest_free(o->guest_data);
    if (o->guest) guest_free(o->guest);
    memset(o, 0, sizeof *o);
    pthread_mutex_unlock(&objects_lock); return NULL;
}

static float gf32(uint32_t a) { uint32_t u = G32(a); float f; memcpy(&f, &u, 4); return f; }
static void sf32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); S32(a, u); }
static int finite_vector(uint32_t a) { return isfinite(gf32(a)) && isfinite(gf32(a+4)) && isfinite(gf32(a+8)); }
static void get_vector(uint32_t a, float v[3]) { v[0]=gf32(a); v[1]=gf32(a+4); v[2]=gf32(a+8); }
static void put_vector(uint32_t a, const float v[3]) { sf32(a,v[0]); sf32(a+4,v[1]); sf32(a+8,v[2]); }

static uint32_t object_addref(DsObject *o) {
    uint32_t refs = 0;
    pthread_mutex_lock(&objects_lock);
    if (o && o->alive) refs = ++o->refs;
    pthread_mutex_unlock(&objects_lock);
    return refs;
}

static uint32_t object_release(DsObject *o) {
    uint32_t refs = 0;
    pthread_mutex_lock(&objects_lock);
    if (o && o->alive && o->refs) {
        refs = --o->refs;
        if (!refs) {
            /* Keep the slot owned until the callback can no longer see its
             * voice; otherwise a creator could overwrite it during removal. */
            if (!o->primary) ds_mixer_remove(&mixer, &o->voice);
            free(o->voice.data);
            if (o->guest_data) guest_free(o->guest_data);
            if (o->guest_3d) guest_free(o->guest_3d);
            if (o->guest) guest_free(o->guest);
            memset(o, 0, sizeof *o);
        }
    }
    pthread_mutex_unlock(&objects_lock);
    return refs;
}

static int iid_d1(uint32_t iid, uint32_t d1) { return iid && G32(iid) == d1; }
static int buffer_qi(DsObject *o, uint32_t iid, uint32_t out) {
    if (!o || !out) return 0;
    uint32_t result = 0;
    /* IUnknown, IDirectSoundBuffer/8, IDirectSound3DListener, IDirectSound3DBuffer. */
    if (iid_d1(iid,0) || iid_d1(iid,0x279afa85u) || iid_d1(iid,0x6825a449u)) result = o->guest;
    else if (o->primary && iid_d1(iid,0x279afa84u)) {
        if (!o->guest_3d) { o->guest_3d=guest_alloc(16); S32(o->guest_3d,vt_listener); S32(o->guest_3d+4,G32(o->guest+4)); }
        result = o->guest_3d;
    } else if (!o->primary && o->voice.has_3d && iid_d1(iid,0x279afa86u)) {
        if (!o->guest_3d) { o->guest_3d=guest_alloc(16); S32(o->guest_3d,vt_buffer3d); S32(o->guest_3d+4,G32(o->guest+4)); }
        result = o->guest_3d;
    }
    S32(out,result); if (result) object_addref(o); return result != 0;
}

void host_dsound_shutdown(void) {
    atomic_store_explicit(&output_terminated,1,memory_order_release);
    pthread_mutex_lock(&output_lock);
    audio_dispose_locked();
    output_requested=0; output_suspended=1;
    if(capture_file){uint64_t bytes64=capture_frames*4u;uint32_t bytes=bytes64>0xffffffffu?0xffffffffu:(uint32_t)bytes64;capture_wav_header(capture_file,bytes);fclose(capture_file);capture_file=NULL;host_log("DirectSound diagnostic capture closed: %llu frames",(unsigned long long)capture_frames);}
    pthread_mutex_unlock(&output_lock);
}

void host_dsound_get_output_diagnostics(HostDsOutputDiagnostics *state) {
    if(!state)return;
    pthread_mutex_lock(&output_lock);
    pthread_mutex_lock(&output_callback_lock);
    *state=(HostDsOutputDiagnostics){output_generation,output_requested,output_suspended,
        output_restart_required,output_callbacks_suspended,output_deferred_count,
        atomic_load_explicit(&output_terminated,memory_order_acquire),
        output_last_prepare,output_last_start,atomic_load_explicit(&output_last_enqueue,memory_order_relaxed),
        output_last_pause,output_last_dispose,output_last_recovery,
        telemetry_callbacks,telemetry_late_callbacks,telemetry_enqueue_failures,telemetry_last_callback_ns,
        telemetry_max_gap_ns,telemetry_max_work_ns,OUTPUT_RATE,output_frames_per_buffer};
    pthread_mutex_unlock(&output_callback_lock);
    pthread_mutex_unlock(&output_lock);
}
void host_dsound_get_output_state(uint32_t *running,int32_t *status) {
    UInt32 value=0,size=sizeof value;OSStatus result=-1;
    pthread_mutex_lock(&output_lock);
    if(output_queue)result=AudioQueueGetProperty(output_queue,kAudioQueueProperty_IsRunning,&value,&size);
    pthread_mutex_unlock(&output_lock);
    if(running)*running=value;if(status)*status=(int32_t)result;
}
#include "directsound_engine_state.inc"
void host_dsound_get_stats(uint64_t *frames, uint64_t *nonzero_samples, float *peak) {
    pthread_once(&mixer_once,mixer_initialize_once);
    if(frames)*frames=host_dsound_output_frames();
    if(nonzero_samples)*nonzero_samples=atomic_load_explicit(&output_nonzero,memory_order_relaxed);
    if(peak){uint32_t bits=atomic_load_explicit(&output_peak_bits,memory_order_relaxed);memcpy(peak,&bits,4);}
    if(atomic_load_explicit(&output_nonzero,memory_order_relaxed) &&
       !atomic_exchange_explicit(&output_logged_active,1,memory_order_relaxed))
        host_log("DirectSound mix became active (reported by diagnostics thread)");
    /* A short snapshot only; logging must happen after releasing mixer.lock. */
    static _Atomic uint64_t next_limiter_report;
    uint64_t now_frames=host_dsound_output_frames();
    uint64_t next_report=atomic_load_explicit(&next_limiter_report,memory_order_relaxed);
    if(now_frames>=next_report && atomic_compare_exchange_strong(&next_limiter_report,&next_report,now_frames+10ull*OUTPUT_RATE)) {
        pthread_mutex_lock(&mixer.lock);
        uint64_t limited=mixer.limited_samples,rendered=mixer.rendered_samples;
        pthread_mutex_unlock(&mixer.lock);
        host_log("DirectSound limiter: %llu of %llu samples past full scale (%.4f%%)",
            (unsigned long long)limited,(unsigned long long)rendered,rendered?100.0*(double)limited/(double)rendered:0.0);
    }
    if(!host_dsound_trace_enabled()) return;
    uint64_t now=host_dsound_output_frames(),next=atomic_load_explicit(&voice_trace_next_frames,memory_order_relaxed);
    if(now<next||!atomic_compare_exchange_strong(&voice_trace_next_frames,&next,now+OUTPUT_RATE*5u)) return;
    VoiceTrace snapshots[DS_OBJECT_MAX]; unsigned count=0,active=0,looping=0; uint64_t ends=0;
    HostDsVoiceStats voices={0}; sound_engine_tables(&voices);
    pthread_mutex_lock(&objects_lock);
    pthread_mutex_lock(&mixer.lock);
    ends=mixer.ended_voices;
    voice_stats_locked(&voices);
    for(uint32_t i=1;i<object_count;i++) {
        DsObject *o=&objects[i]; if(!o->alive||o->primary)continue;
        DsMixerVoice *v=&o->voice;
        active+=v->playing; looping+=v->playing&&v->looping;
        if(v->playing||o->unlock_count!=o->trace_unlock_count) snapshots[count++]=voice_trace_snapshot(o);
        o->trace_unlock_count=o->unlock_count;
    }
    pthread_mutex_unlock(&mixer.lock);
    pthread_mutex_unlock(&objects_lock);
    /* The engine's side follows the mixer's: sounds it holds, channels in
     * use, and voices it could take now, so a quiet stretch in the trace
     * says whether sounds were asked for and whether they found voices. */
    host_log("[audio-voice] summary frames=%llu active=%u looping=%u ends=%llu nonzero=%llu detail-dropped=%u plays=%llu stops=%llu busy-probes=%llu/%llu rejected=%llu engine=%d sources=%d looping-sounds=%d channels=%d/%d starved=%d voices=%d assigned=%d free=%d held=%d free-by-type=%d/%d,%d/%d,%d/%d,%d/%d cache=%d loaded=%d locked=%d reads-queued=%d,%d,%d",
        (unsigned long long)now,active,looping,(unsigned long long)ends,
        (unsigned long long)atomic_load_explicit(&output_nonzero,memory_order_relaxed),
        atomic_load_explicit(&voice_trace_event_dropped,memory_order_relaxed),
        (unsigned long long)voices.plays,(unsigned long long)voices.stops,
        (unsigned long long)voices.status_busy,(unsigned long long)voices.status_probes,(unsigned long long)voices.rejected,
        voices.engine_flags,voices.sources,voices.looping_sounds,voices.channels_busy,voices.channels,voices.channels_starved,
        voices.voices,voices.voices_assigned,voices.voices_free,voices.voices_held,
        voices.free_by_type[0],voices.voices_by_type[0],voices.free_by_type[1],voices.voices_by_type[1],
        voices.free_by_type[2],voices.voices_by_type[2],voices.free_by_type[3],voices.voices_by_type[3],
        voices.cache_sounds,voices.cache_loaded,voices.cache_locked,
        voices.reads_queued[0],voices.reads_queued[1],voices.reads_queued[2]);
    for(unsigned i=0;i<count;i++)voice_trace_event("voice",snapshots[i],0,DS_OK);
    voice_exhaustion_note(&voices);
}

void host_dsound_create8(EngineCPU *cpu) {
    uint32_t guid=ARG(0), out=ARG(1), outer=ARG(2);
    if (!out || outer) RET_STDCALL(DSERR_INVALIDPARAM,3);
    S32(out,0); if (guid) RET_STDCALL(DSERR_NODRIVER,3);
    build_vtables(); if (!audio_start()) RET_STDCALL(DSERR_NODRIVER,3);
    pthread_mutex_lock(&objects_lock);
    if (!device_guest) { device_guest=guest_alloc(16); S32(device_guest,vt_device); device_refs=1; }
    else ++device_refs;
    S32(out,device_guest); pthread_mutex_unlock(&objects_lock);
    atomic_store_explicit(&engine_sound_observable,1,memory_order_release);
    host_log("DirectSoundCreate8: native AudioQueue device ready"); RET_STDCALL(DS_OK,3);
}
void host_dsound_create(EngineCPU *cpu) { host_dsound_create8(cpu); }

/* IDirectSound8 */
void host_dsound_device_0(EngineCPU *cpu) { uint32_t out=ARG(2); if(!out) RET_STDCALL(DSERR_INVALIDPARAM,3); S32(out,device_guest); ++device_refs; RET_STDCALL(DS_OK,3); }
void host_dsound_device_1(EngineCPU *cpu) { (void)ARG(0); RET_STDCALL(++device_refs,1); }
void host_dsound_device_2(EngineCPU *cpu) { (void)ARG(0); if(device_refs) --device_refs; RET_STDCALL(device_refs,1); }
void host_dsound_device_3(EngineCPU *cpu) {
    uint32_t desc=ARG(1), out=ARG(2), outer=ARG(3); if(!desc||!out||outer) DS_FAIL("allocate",desc,DSERR_INVALIDPARAM,4); S32(out,0);
    uint32_t size=G32(desc), caps=G32(desc+4), bytes=G32(desc+8), fmtp=G32(desc+16);
    if(size<20) DS_FAIL("allocate",desc,DSERR_INVALIDPARAM,4);
    int primary=(caps&DSBCAPS_PRIMARYBUFFER)!=0; DsMixerFormat fmt; format_default(&fmt);
    if (primary) { if(bytes||fmtp) DS_FAIL("allocate",desc,DSERR_INVALIDPARAM,4); }
    else {
        /* DSBCAPS_LOCHARDWARE: Windows Vista and later mix every DirectSound
         * buffer in software and never fail this, so honour the request with a
         * software voice instead of refusing it. Refusing made Halo believe the
         * device had no hardware voices at startup. */
        if(!bytes||bytes>(128u<<20)||!read_format(fmtp,&fmt)||bytes%fmt.block_align) DS_FAIL("allocate",desc,DSERR_BADFORMAT,4);
    }
    DsObject *o=object_new(caps,bytes,&fmt,primary); if(!o) DS_FAIL("allocate",desc,DSERR_OUTOFMEMORY,4);
    host_log("DirectSound buffer created: %s flags=%08x bytes=%u format=%u-channel %u-bit %u Hz",primary?"primary":"secondary",caps,bytes,fmt.channels,fmt.bits,fmt.sample_rate);
    if(host_dsound_trace_enabled()){pthread_mutex_lock(&mixer.lock);VoiceTrace t=voice_trace_snapshot(o);pthread_mutex_unlock(&mixer.lock);voice_trace_event("allocate",t,caps,DS_OK);}
    S32(out,o->guest); RET_STDCALL(DS_OK,4);
}
void host_dsound_device_4(EngineCPU *cpu) {
    uint32_t p=ARG(1); if(!p||G32(p)<4) RET_STDCALL(DSERR_INVALIDPARAM,2); uint32_t n=G32(p); if(n>96)n=96; memset(GPTR(p),0,n); S32(p,n);
    /* The output supports every primary PCM mode and continuous sample rates.
     * Secondary format flags describe hardware mixing, so they remain clear
     * together with the hardware voice counts for this software mixer. */
    if(n>=8)S32(p+4,0x0000001fu); if(n>=12)S32(p+8,100); if(n>=16)S32(p+12,200000); if(n>=20)S32(p+16,1); RET_STDCALL(DS_OK,2);
}
void host_dsound_device_5(EngineCPU *cpu) {
    DsObject *source=object_from_guest(ARG(1)); uint32_t out=ARG(2); if(!source||source->primary||!out) RET_STDCALL(DSERR_INVALIDPARAM,3); S32(out,0);
    DsObject *o=object_new(source->caps,source->voice.bytes,&source->voice.format,0); if(!o) RET_STDCALL(DSERR_OUTOFMEMORY,3);
    pthread_mutex_lock(&mixer.lock); memcpy(o->voice.data,source->voice.data,source->voice.bytes); pthread_mutex_unlock(&mixer.lock); S32(out,o->guest); RET_STDCALL(DS_OK,3);
}
void host_dsound_device_6(EngineCPU *cpu) { (void)ARG(0); (void)ARG(1); if(!ARG(2)) RET_STDCALL(DSERR_INVALIDPARAM,3); RET_STDCALL(DS_OK,3); }
void host_dsound_device_7(EngineCPU *cpu) { (void)ARG(0); RET_STDCALL(DS_OK,1); }
void host_dsound_device_8(EngineCPU *cpu) { uint32_t p=ARG(1); if(!p) RET_STDCALL(DSERR_INVALIDPARAM,2); S32(p,4u); RET_STDCALL(DS_OK,2); }
void host_dsound_device_9(EngineCPU *cpu) { uint32_t c=ARG(1); if((c&0xffffu)>7u) RET_STDCALL(DSERR_INVALIDPARAM,2); RET_STDCALL(DS_OK,2); }
void host_dsound_device_10(EngineCPU *cpu) { if(ARG(1)) RET_STDCALL(DSERR_NODRIVER,2); RET_STDCALL(DS_OK,2); }
void host_dsound_device_11(EngineCPU *cpu) { uint32_t p=ARG(1); if(!p) RET_STDCALL(DSERR_INVALIDPARAM,2); S32(p,0); RET_STDCALL(DS_OK,2); }

/* IDirectSoundBuffer8 */
void host_dsound_buffer_0(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); uint32_t out=ARG(2); if(!out) RET_STDCALL(DSERR_INVALIDPARAM,3); S32(out,0); RET_STDCALL(buffer_qi(o,ARG(1),out)?DS_OK:E_NOINTERFACE,3); }
void host_dsound_buffer_1(EngineCPU *cpu) { RET_STDCALL(object_addref(object_from_guest(ARG(0))),1); }
void host_dsound_buffer_2(EngineCPU *cpu) { RET_STDCALL(object_release(object_from_guest(ARG(0))),1); }
void host_dsound_buffer_3(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); uint32_t p=ARG(1); if(!o||!p||G32(p)<4) RET_STDCALL(DSERR_INVALIDPARAM,2); uint32_t n=G32(p); if(n>20)n=20; memset(GPTR(p),0,n); S32(p,n); if(n>=8)S32(p+4,o->caps); if(n>=12)S32(p+8,o->voice.bytes); RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_4(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); if(!o) RET_STDCALL(DSERR_INVALIDPARAM,3); pthread_mutex_lock(&mixer.lock); uint32_t pos=voice_play_cursor(&o->voice); uint32_t write=voice_write_cursor(&o->voice); pthread_mutex_unlock(&mixer.lock); if(ARG(1))S32(ARG(1),pos); if(ARG(2))S32(ARG(2),write); RET_STDCALL(DS_OK,3); }
void host_dsound_buffer_5(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); uint32_t p=ARG(1),cap=ARG(2),written=ARG(3); if(!o) RET_STDCALL(DSERR_INVALIDPARAM,4); if(written)S32(written,18); if(!p) RET_STDCALL(cap?DSERR_INVALIDPARAM:DS_OK,4); if(cap<18) RET_STDCALL(DSERR_INVALIDPARAM,4); write_format(p,&o->voice.format); RET_STDCALL(DS_OK,4); }
void host_dsound_buffer_6(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2); pthread_mutex_lock(&mixer.lock); S32(ARG(1),(uint32_t)o->voice.volume); pthread_mutex_unlock(&mixer.lock); RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_7(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2); pthread_mutex_lock(&mixer.lock); S32(ARG(1),(uint32_t)o->voice.pan); pthread_mutex_unlock(&mixer.lock); RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_8(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2); pthread_mutex_lock(&mixer.lock); S32(ARG(1),o->voice.frequency); pthread_mutex_unlock(&mixer.lock); RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_9(EngineCPU *cpu) {
    DsObject*o=object_from_guest(ARG(0)); if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);
    pthread_mutex_lock(&mixer.lock); uint32_t s=voice_status(&o->voice); pthread_mutex_unlock(&mixer.lock);
    atomic_fetch_add_explicit(&voice_status_probes,1,memory_order_relaxed);
    if(s&DSBSTATUS_PLAYING)atomic_fetch_add_explicit(&voice_status_busy,1,memory_order_relaxed);
    S32(ARG(1),s); RET_STDCALL(DS_OK,2);
}
void host_dsound_buffer_10(EngineCPU *cpu) { RET_STDCALL(DSERR_ALREADYINITIALIZED,3); }
void host_dsound_buffer_11(EngineCPU *cpu) {
    DsObject*o=object_from_guest(ARG(0)); uint32_t off=ARG(1),bytes=ARG(2),p1=ARG(3),b1=ARG(4),p2=ARG(5),b2=ARG(6),flags=ARG(7);
    if(!o||o->primary||!p1||!b1||!p2||!b2||!o->voice.bytes) DS_FAIL("lock",bytes,DSERR_INVALIDPARAM,8);
    pthread_mutex_lock(&mixer.lock); if(o->locked){pthread_mutex_unlock(&mixer.lock);DS_FAIL("lock",bytes,DSERR_INVALIDCALL,8);} uint32_t size=o->voice.bytes;
    if(flags&DSLOCK_FROMWRITECURSOR)off=voice_write_cursor(&o->voice);
    if(flags&DSLOCK_ENTIREBUFFER){off=0;bytes=size;} if(off>=size||!bytes||bytes>size){pthread_mutex_unlock(&mixer.lock);DS_FAIL("lock",bytes,DSERR_INVALIDPARAM,8);}
    o->lock1=off;o->lock1_bytes=bytes>(size-off)?size-off:bytes;o->lock2=0;o->lock2_bytes=bytes-o->lock1_bytes;o->locked=1;
    S32(p1,o->guest_data+o->lock1);S32(b1,o->lock1_bytes);S32(p2,o->lock2_bytes?o->guest_data:0);S32(b2,o->lock2_bytes);pthread_mutex_unlock(&mixer.lock);
    if(host_dsound_trace_enabled()&&((off%o->voice.format.block_align)||(bytes%o->voice.format.block_align)||!o->unlock_count))
        host_log("[audio-voice] lock frames=%llu guest=%08x offset=%u bytes=%u split=%u,%u align=%u flags=%u",(unsigned long long)host_dsound_output_frames(),o->guest,off,bytes,o->lock1_bytes,o->lock2_bytes,o->voice.format.block_align,flags);
    RET_STDCALL(DS_OK,8);
}
void host_dsound_buffer_12(EngineCPU *cpu) {
    DsObject*o=object_from_guest(ARG(0)); uint32_t flags=ARG(3);
    if(!o||o->primary||flags&~DSBPLAY_LOOPING)DS_FAIL("play",flags,DSERR_INVALIDPARAM,4);
    int trace=host_dsound_trace_enabled(); VoiceTrace t={0};
    pthread_mutex_lock(&mixer.lock);
    o->voice.looping=(flags&DSBPLAY_LOOPING)!=0;o->voice.playing=1;
    ds_mixer_voice_restart(&o->voice);
    if(trace)t=voice_trace_snapshot(o);
    pthread_mutex_unlock(&mixer.lock);
    atomic_fetch_add_explicit(&voice_plays,1,memory_order_relaxed);
    if(trace)voice_trace_event("play",t,flags,DS_OK);RET_STDCALL(DS_OK,4);
}
void host_dsound_buffer_13(EngineCPU *cpu) {
    DsObject*o=object_from_guest(ARG(0));uint32_t pos=ARG(1);
    if(!o||o->primary||pos>=o->voice.bytes)DS_FAIL("position",pos,DSERR_INVALIDPARAM,2);
    int trace=host_dsound_trace_enabled(); VoiceTrace t={0};
    pthread_mutex_lock(&mixer.lock);o->voice.cursor_frames=pos/o->voice.format.block_align;
    if(trace)t=voice_trace_snapshot(o);pthread_mutex_unlock(&mixer.lock);
    if(trace)voice_trace_event("position",t,pos,DS_OK);RET_STDCALL(DS_OK,2);
}
void host_dsound_buffer_14(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0));DsMixerFormat f;if(!o||!o->primary||!read_format(ARG(1),&f))RET_STDCALL(DSERR_BADFORMAT,2);pthread_mutex_lock(&mixer.lock);o->voice.format=f;o->voice.frequency=f.sample_rate;pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_15(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0));int32_t v=(int32_t)ARG(1);if(!o||!(o->caps&DSBCAPS_CTRLVOLUME)||v < -10000||v>0)RET_STDCALL(DSERR_CONTROLUNAVAIL,2);pthread_mutex_lock(&mixer.lock);o->voice.volume=v;pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_16(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0));int32_t v=(int32_t)ARG(1);if(!o||!(o->caps&DSBCAPS_CTRLPAN)||v < -10000||v>10000)RET_STDCALL(DSERR_CONTROLUNAVAIL,2);pthread_mutex_lock(&mixer.lock);o->voice.pan=v;pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_17(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0));uint32_t v=ARG(1);if(!o||!(o->caps&DSBCAPS_CTRLFREQUENCY)||(v&& (v<100||v>200000)))RET_STDCALL(DSERR_CONTROLUNAVAIL,2);pthread_mutex_lock(&mixer.lock);o->voice.frequency=v?v:o->voice.format.sample_rate;pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2); }
void host_dsound_buffer_18(EngineCPU *cpu) {
    DsObject*o=object_from_guest(ARG(0));if(!o||o->primary)DS_FAIL("stop",0,DSERR_INVALIDPARAM,1);
    int trace=host_dsound_trace_enabled();VoiceTrace t={0};
    pthread_mutex_lock(&mixer.lock);o->voice.playing=0;if(trace)t=voice_trace_snapshot(o);
    pthread_mutex_unlock(&mixer.lock);atomic_fetch_add_explicit(&voice_stops,1,memory_order_relaxed);
    if(trace)voice_trace_event("stop",t,0,DS_OK);RET_STDCALL(DS_OK,1);
}
static uint32_t pcm_nonzero(const uint8_t *p,uint32_t bytes,const DsMixerFormat *f) {
    uint32_t count=0;
    if(f->bits==8) {for(uint32_t i=0;i<bytes;i++)count+=p[i]!=128;}
    else if(f->bits==16) {for(uint32_t i=0;i+1<bytes;i+=2){int16_t v;memcpy(&v,p+i,2);count+=v!=0;}}
    else if(f->bits==32) {for(uint32_t i=0;i+3<bytes;i+=4){float v;memcpy(&v,p+i,4);count+=isfinite(v)&&fabsf(v)>1.e-7f;}}
    return count;
}
void host_dsound_buffer_19(EngineCPU *cpu) {
    DsObject*o=object_from_guest(ARG(0));
    if(!o||!o->locked||ARG(1)!=o->guest_data+o->lock1||ARG(2)!=o->lock1_bytes||ARG(4)!=o->lock2_bytes||(o->lock2_bytes&&ARG(3)!=o->guest_data))DS_FAIL("unlock",ARG(2),DSERR_INVALIDPARAM,5);
    int trace=host_dsound_trace_enabled(),emit=0;VoiceTrace t={0};uint32_t nz=0;
    /* Guest has finished writing these regions; scan outside the mixer lock. */
    if(trace){nz=pcm_nonzero(GPTR(o->guest_data+o->lock1),o->lock1_bytes,&o->voice.format);if(o->lock2_bytes)nz+=pcm_nonzero(GPTR(o->guest_data),o->lock2_bytes,&o->voice.format);}
    pthread_mutex_lock(&mixer.lock);
    memcpy(o->voice.data+o->lock1,GPTR(o->guest_data+o->lock1),o->lock1_bytes);
    if(o->lock2_bytes)memcpy(o->voice.data,GPTR(o->guest_data),o->lock2_bytes);
    o->locked=0;
    if(trace){
        emit=!o->unlock_count||(!nz != !o->last_unlock_nonzero);
        o->unlock_count++;o->unlock_bytes+=o->lock1_bytes+o->lock2_bytes;o->unlock_nonzero+=nz;
        o->last_unlock_frame=host_dsound_output_frames();if(nz)o->last_nonzero_frame=o->last_unlock_frame;
        o->last_unlock_nonzero=nz;t=voice_trace_snapshot(o);
    }
    pthread_mutex_unlock(&mixer.lock);
    if(emit)voice_trace_event("refill",t,nz,DS_OK);RET_STDCALL(DS_OK,5);
}
void host_dsound_buffer_20(EngineCPU *cpu) { RET_STDCALL(object_from_guest(ARG(0))?DS_OK:DSERR_INVALIDPARAM,1); }
void host_dsound_buffer_21(EngineCPU *cpu) { RET_STDCALL(DSERR_UNSUPPORTED,4); }
void host_dsound_buffer_22(EngineCPU *cpu) { RET_STDCALL(DSERR_UNSUPPORTED,3); }
void host_dsound_buffer_23(EngineCPU *cpu) { RET_STDCALL(DSERR_UNSUPPORTED,6); }

static void listener_get(DsMixerListener *l,uint32_t p){S32(p,64);put_vector(p+4,l->position);put_vector(p+16,l->velocity);put_vector(p+28,l->front);put_vector(p+40,l->up);sf32(p+52,l->distance_factor);sf32(p+56,l->rolloff_factor);sf32(p+60,l->doppler_factor);}
static int listener_set(DsMixerListener*l,uint32_t p){if(!p||G32(p)<64||!finite_vector(p+4)||!finite_vector(p+16)||!finite_vector(p+28)||!finite_vector(p+40))return 0;float d=gf32(p+52),r=gf32(p+56),dop=gf32(p+60);if(!(d>0)||r<0||dop<0||!isfinite(d+r+dop))return 0;get_vector(p+4,l->position);get_vector(p+16,l->velocity);get_vector(p+28,l->front);get_vector(p+40,l->up);l->distance_factor=d;l->rolloff_factor=r;l->doppler_factor=dop;return 1;}
/* Halo carries its own distance attenuation in each voice's millibel volume.
 * Whether DirectSound is also meant to attenuate depends entirely on the
 * rolloff factor the game sets here, so record what it actually asks for. */
static uint64_t listener_writes;
static void listener_changed(int deferred){
    if(deferred)mixer.pending_listener=1;else mixer.current_listener=mixer.deferred_listener;
    if(host_dsound_trace_enabled()&&(listener_writes<8||listener_writes%500==0)){
        const DsMixerListener *l=&mixer.deferred_listener;
        host_log("[audio-listener] write=%llu deferred=%d position=%.3f,%.3f,%.3f front=%.3f,%.3f,%.3f up=%.3f,%.3f,%.3f distanceFactor=%.4f rolloff=%.4f doppler=%.4f",
            (unsigned long long)listener_writes,deferred,
            l->position[0],l->position[1],l->position[2],l->front[0],l->front[1],l->front[2],
            l->up[0],l->up[1],l->up[2],l->distance_factor,l->rolloff_factor,l->doppler_factor);
    }
    listener_writes++;
}
void host_dsound_listener_0(EngineCPU *cpu){DsObject*o=object_from_guest(ARG(0));uint32_t out=ARG(2);if(!out)RET_STDCALL(DSERR_INVALIDPARAM,3);S32(out,0);RET_STDCALL(buffer_qi(o,ARG(1),out)?DS_OK:E_NOINTERFACE,3);} void host_dsound_listener_1(EngineCPU *cpu){RET_STDCALL(object_addref(object_from_guest(ARG(0))),1);} void host_dsound_listener_2(EngineCPU *cpu){RET_STDCALL(object_release(object_from_guest(ARG(0))),1);}
void host_dsound_listener_3(EngineCPU *cpu){if(!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);pthread_mutex_lock(&mixer.lock);listener_get(&mixer.current_listener,ARG(1));pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2);}
#define LGET(N,FIELD) void host_dsound_listener_##N(EngineCPU *cpu){if(!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);pthread_mutex_lock(&mixer.lock);sf32(ARG(1),mixer.current_listener.FIELD);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2);}
LGET(4,distance_factor) LGET(5,doppler_factor)
void host_dsound_listener_6(EngineCPU *cpu){if(!ARG(1)||!ARG(2))RET_STDCALL(DSERR_INVALIDPARAM,3);pthread_mutex_lock(&mixer.lock);put_vector(ARG(1),mixer.current_listener.front);put_vector(ARG(2),mixer.current_listener.up);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,3);}
#define LVGET(N,FIELD) void host_dsound_listener_##N(EngineCPU *cpu){if(!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);pthread_mutex_lock(&mixer.lock);put_vector(ARG(1),mixer.current_listener.FIELD);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2);}
LVGET(7,position) LGET(8,rolloff_factor) LVGET(9,velocity)
void host_dsound_listener_10(EngineCPU *cpu){uint32_t p=ARG(1),d=ARG(2);pthread_mutex_lock(&mixer.lock);int ok=listener_set(&mixer.deferred_listener,p);if(ok)listener_changed(d!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(ok?DS_OK:DSERR_INVALIDPARAM,3);}
#define LSETF(N,FIELD,CHECK) void host_dsound_listener_##N(EngineCPU *cpu){float v=gf32(cpu->gpr[4]+8);uint32_t d=ARG(2);if(!(CHECK)||!isfinite(v))RET_STDCALL(DSERR_INVALIDPARAM,3);pthread_mutex_lock(&mixer.lock);mixer.deferred_listener.FIELD=v;listener_changed(d!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,3);}
LSETF(11,distance_factor,v>0) LSETF(12,doppler_factor,v>=0)
void host_dsound_listener_13(EngineCPU *cpu){uint32_t a=cpu->gpr[4]+8;if(!isfinite(gf32(a))||!isfinite(gf32(a+4))||!isfinite(gf32(a+8))||!isfinite(gf32(a+12))||!isfinite(gf32(a+16))||!isfinite(gf32(a+20)))RET_STDCALL(DSERR_INVALIDPARAM,8);pthread_mutex_lock(&mixer.lock);for(int i=0;i<3;i++){mixer.deferred_listener.front[i]=gf32(a+4*i);mixer.deferred_listener.up[i]=gf32(a+12+4*i);}listener_changed(ARG(7)!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,8);}
#define LSETV(N,FIELD) void host_dsound_listener_##N(EngineCPU *cpu){uint32_t a=cpu->gpr[4]+8;if(!finite_vector(a))RET_STDCALL(DSERR_INVALIDPARAM,5);pthread_mutex_lock(&mixer.lock);get_vector(a,mixer.deferred_listener.FIELD);listener_changed(ARG(4)!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,5);}
LSETV(14,position) LSETF(15,rolloff_factor,v>=0) LSETV(16,velocity)
void host_dsound_listener_17(EngineCPU *cpu){ds_mixer_commit(&mixer);RET_STDCALL(DS_OK,1);}

static void b3get(DsMixer3D*p,uint32_t a){S32(a,64);put_vector(a+4,p->position);put_vector(a+16,p->velocity);put_vector(a+28,p->cone);S32(a+40,p->inside_angle);S32(a+44,p->outside_angle);S32(a+48,(uint32_t)p->outside_volume);sf32(a+52,p->min_distance);sf32(a+56,p->max_distance);S32(a+60,p->mode);}
static int b3set(DsMixer3D*p,uint32_t a){if(!a||G32(a)<64||!finite_vector(a+4)||!finite_vector(a+16)||!finite_vector(a+28))return 0;uint32_t in=G32(a+40),out=G32(a+44),mode=G32(a+60);int32_t vol=(int32_t)G32(a+48);float min=gf32(a+52),max=gf32(a+56);if(in>360||out>360||in>out||vol < -10000||vol>0||!(min>0)||max<min||mode>2||!isfinite(min+max))return 0;get_vector(a+4,p->position);get_vector(a+16,p->velocity);get_vector(a+28,p->cone);p->inside_angle=in;p->outside_angle=out;p->outside_volume=vol;p->min_distance=min;p->max_distance=max;p->mode=mode;return 1;}
static DsObject*b3obj(EngineCPU *cpu){return object_from_guest(ARG(0));} static void b3changed(DsObject*o,int deferred){if(deferred)o->voice.pending_3d=1;else o->voice.current_3d=o->voice.deferred_3d;}
void host_dsound_buffer3d_0(EngineCPU *cpu){DsObject*o=b3obj(cpu);uint32_t out=ARG(2);if(!out)RET_STDCALL(DSERR_INVALIDPARAM,3);S32(out,0);RET_STDCALL(buffer_qi(o,ARG(1),out)?DS_OK:E_NOINTERFACE,3);}void host_dsound_buffer3d_1(EngineCPU *cpu){RET_STDCALL(object_addref(b3obj(cpu)),1);}void host_dsound_buffer3d_2(EngineCPU *cpu){RET_STDCALL(object_release(b3obj(cpu)),1);}
void host_dsound_buffer3d_3(EngineCPU *cpu){DsObject*o=b3obj(cpu);if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);pthread_mutex_lock(&mixer.lock);b3get(&o->voice.current_3d,ARG(1));pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2);}
void host_dsound_buffer3d_4(EngineCPU *cpu){DsObject*o=b3obj(cpu);if(!o||!ARG(1)||!ARG(2))RET_STDCALL(DSERR_INVALIDPARAM,3);pthread_mutex_lock(&mixer.lock);S32(ARG(1),o->voice.current_3d.inside_angle);S32(ARG(2),o->voice.current_3d.outside_angle);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,3);}
#define B3VGET(N,FIELD) void host_dsound_buffer3d_##N(EngineCPU *cpu){DsObject*o=b3obj(cpu);if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);pthread_mutex_lock(&mixer.lock);put_vector(ARG(1),o->voice.current_3d.FIELD);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2);}
#define B3IGET(N,FIELD) void host_dsound_buffer3d_##N(EngineCPU *cpu){DsObject*o=b3obj(cpu);if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);pthread_mutex_lock(&mixer.lock);S32(ARG(1),(uint32_t)o->voice.current_3d.FIELD);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2);}
#define B3FGET(N,FIELD) void host_dsound_buffer3d_##N(EngineCPU *cpu){DsObject*o=b3obj(cpu);if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2);pthread_mutex_lock(&mixer.lock);sf32(ARG(1),o->voice.current_3d.FIELD);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,2);}
B3VGET(5,cone) B3IGET(6,outside_volume) B3FGET(7,max_distance) B3FGET(8,min_distance) B3IGET(9,mode) B3VGET(10,position) B3VGET(11,velocity)
void host_dsound_buffer3d_12(EngineCPU *cpu){DsObject*o=b3obj(cpu);if(!o)RET_STDCALL(DSERR_INVALIDPARAM,3);pthread_mutex_lock(&mixer.lock);int ok=b3set(&o->voice.deferred_3d,ARG(1));if(ok)b3changed(o,ARG(2)!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(ok?DS_OK:DSERR_INVALIDPARAM,3);}
void host_dsound_buffer3d_13(EngineCPU *cpu){DsObject*o=b3obj(cpu);uint32_t in=ARG(1),out=ARG(2);if(!o||in>360||out>360||in>out)RET_STDCALL(DSERR_INVALIDPARAM,4);pthread_mutex_lock(&mixer.lock);o->voice.deferred_3d.inside_angle=in;o->voice.deferred_3d.outside_angle=out;b3changed(o,ARG(3)!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,4);}
#define B3VSET(N,FIELD) void host_dsound_buffer3d_##N(EngineCPU *cpu){DsObject*o=b3obj(cpu);uint32_t a=cpu->gpr[4]+8;if(!o||!finite_vector(a))RET_STDCALL(DSERR_INVALIDPARAM,5);pthread_mutex_lock(&mixer.lock);get_vector(a,o->voice.deferred_3d.FIELD);b3changed(o,ARG(4)!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,5);}
#define B3ISET(N,FIELD,CHECK) void host_dsound_buffer3d_##N(EngineCPU *cpu){DsObject*o=b3obj(cpu);int32_t v=(int32_t)ARG(1);if(!o||!(CHECK))RET_STDCALL(DSERR_INVALIDPARAM,3);pthread_mutex_lock(&mixer.lock);o->voice.deferred_3d.FIELD=v;b3changed(o,ARG(2)!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,3);}
#define B3FSET(N,FIELD,CHECK) void host_dsound_buffer3d_##N(EngineCPU *cpu){DsObject*o=b3obj(cpu);float v=gf32(cpu->gpr[4]+8);if(!o||!isfinite(v)||!(CHECK))RET_STDCALL(DSERR_INVALIDPARAM,3);pthread_mutex_lock(&mixer.lock);o->voice.deferred_3d.FIELD=v;b3changed(o,ARG(2)!=0);pthread_mutex_unlock(&mixer.lock);RET_STDCALL(DS_OK,3);}
B3VSET(14,cone) B3ISET(15,outside_volume,v>=-10000&&v<=0) B3FSET(16,max_distance,v>=o->voice.deferred_3d.min_distance) B3FSET(17,min_distance,v>0&&v<=o->voice.deferred_3d.max_distance) B3ISET(18,mode,v>=0&&v<=2) B3VSET(19,position) B3VSET(20,velocity)
