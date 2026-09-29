/* Mock AudioQueue and clock; no playback. Build with directsound_mixer.c,
 * -I native/EngineReuse -pthread -framework AudioToolbox; supports ASan/UBSan. */
#include <AudioToolbox/AudioToolbox.h>
#include <time.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
static uint64_t now_ns=1000000000ull;
static unsigned creates,starts,allocated;
static int fail_create, fail_enqueue, fail_start;
static int inside_callback;
static void (*on_transaction_lock)(void);
static pthread_mutex_t *transaction_lock;
static AudioQueueBufferRef buffers[3];
static int queued[3];
static unsigned lifecycle_unlocks;
static int fake_unlock(pthread_mutex_t *m){
    if(m==transaction_lock)lifecycle_unlocks++;
    return pthread_mutex_unlock(m);
}
static int fake_lock(pthread_mutex_t *m){
    if(m==transaction_lock && on_transaction_lock){void (*hook)(void)=on_transaction_lock;on_transaction_lock=NULL;hook();}
    return pthread_mutex_lock(m);
}
static void locked(void){assert(pthread_mutex_trylock(transaction_lock)!=0);}
static uint64_t fake_clock(clockid_t c){(void)c;return now_ns;}
static OSStatus fake_new(const AudioStreamBasicDescription *f,AudioQueueOutputCallback cb,void *ctx,CFRunLoopRef loop,CFStringRef mode,UInt32 flags,AudioQueueRef *out){
(void)f;(void)cb;(void)ctx;(void)loop;(void)mode;(void)flags;locked();creates++;if(fail_create){*out=NULL;return -50;}*out=(AudioQueueRef)(uintptr_t)1;return 0;}
static OSStatus fake_allocate(AudioQueueRef q,UInt32 size,AudioQueueBufferRef *out){(void)q;assert(allocated<3);AudioQueueBufferRef b=calloc(1,sizeof *b);assert(b);AudioQueueBuffer init={.mAudioDataBytesCapacity=size,.mAudioData=calloc(1,size)};memcpy(b,&init,sizeof init);buffers[allocated++]=b;*out=b;return 0;}
static OSStatus fake_enqueue(AudioQueueRef q,AudioQueueBufferRef b,UInt32 n,const AudioStreamPacketDescription *p){(void)q;(void)n;(void)p;unsigned i=0;while(i<allocated&&buffers[i]!=b)i++;assert(i<allocated&&!queued[i]);if(fail_enqueue){fail_enqueue=0;return -50;}queued[i]=1;return 0;}
static OSStatus fake_dispose(AudioQueueRef q,Boolean immediate){(void)q;(void)immediate;locked();for(unsigned i=0;i<allocated;i++){free(buffers[i]->mAudioData);free(buffers[i]);buffers[i]=NULL;queued[i]=0;}allocated=0;return 0;}
static OSStatus fake_get(AudioQueueRef q,AudioQueuePropertyID prop,void *out,UInt32 *size){(void)q;(void)prop;assert(*size==sizeof(UInt32));*(UInt32 *)out=1;return 0;}
static OSStatus fake_start(AudioQueueRef q,const AudioTimeStamp *t){(void)q;(void)t;locked();starts++;if(fail_start){fail_start=0;return -50;}return 0;}
static OSStatus fake_pause(AudioQueueRef q){(void)q;return 0;}
#define AudioQueueNewOutput fake_new
#define AudioQueueAllocateBuffer fake_allocate
#define AudioQueueEnqueueBuffer fake_enqueue
#define AudioQueueDispose fake_dispose
#define AudioQueueGetProperty fake_get
#define AudioQueueStart fake_start
#define AudioQueuePause fake_pause
#define clock_gettime_nsec_np fake_clock
#define pthread_mutex_unlock fake_unlock
#define pthread_mutex_lock fake_lock
#include "../directsound.c"
uint8_t *engine_flat_base;
void host_log(const char *f,...){(void)f;assert(!inside_callback);}
uint32_t guest_alloc(uint32_t n){(void)n;abort();}
void guest_free(uint32_t p){(void)p;abort();}
uint32_t host_proc_address(const char *d,const char *n){(void)d;(void)n;return 0;}
void halo_haptics_observe(const float *p,uint32_t n,uint32_t r){(void)p;(void)n;(void)r;}
void halo_haptics_voice_onset(float l,float f,int a){(void)l;(void)f;(void)a;}
static void progress_while_locking(void){atomic_fetch_add(&output_frames,512);}
static unsigned queued_count(void){return (unsigned)(queued[0]+queued[1]+queued[2]);}
static void complete_buffer(unsigned i){assert(i<allocated&&queued[i]);queued[i]=0;inside_callback=1;audio_callback(NULL,output_queue,buffers[i]);inside_callback=0;}
static void fault_while_locking(void){fail_enqueue=1;complete_buffer(0);}
static void replace_while_locking(void){assert(host_dsound_rebuild_output());assert(host_dsound_resume_output());}
int main(void){
pthread_once(&mixer_once,mixer_initialize_once);
/* Two remaining buffers must cover the observed 50 ms callback gap. */
assert((OUTPUT_BUFFERS-1)*output_frames_per_buffer*1000u/OUTPUT_RATE>50u);
transaction_lock=&output_lock;output_requested=1;output_suspended=0;
pthread_mutex_lock(&output_lock);assert(audio_resume_locked());pthread_mutex_unlock(&output_lock);assert(starts==1&&creates==1);
host_dsound_watchdog();now_ns+=2000000000ull;fail_create=1;lifecycle_unlocks=0;host_dsound_watchdog();assert(lifecycle_unlocks==1);assert(!output_queue&&creates==2&&starts==1&&output_last_recovery==-1);
fail_create=0;now_ns+=1000000000ull;host_dsound_watchdog();assert(creates==2);
now_ns+=1000000000ull;lifecycle_unlocks=0;host_dsound_watchdog();assert(lifecycle_unlocks==1);assert(output_queue&&creates==3&&starts==2&&output_last_recovery==0);
host_dsound_pause_output();unsigned before=creates;now_ns+=3000000000ull;host_dsound_watchdog();
pthread_mutex_lock(&output_lock);assert(audio_resume_locked());pthread_mutex_unlock(&output_lock);assert(creates==before&&starts==2&&output_suspended&&output_callbacks_suspended);
assert(host_dsound_resume_output());assert(starts==3);
/* The paused interval is not evidence that the newly resumed queue stalled. */
host_dsound_watchdog();assert(creates==before);
now_ns+=499000000ull;host_dsound_watchdog();assert(creates==before);
atomic_fetch_add(&output_frames,512);now_ns+=3000000000ull;host_dsound_watchdog();assert(creates==before);
/* A callback may finish while the watchdog waits for the lifecycle lock. */
now_ns+=3000000000ull;on_transaction_lock=progress_while_locking;host_dsound_watchdog();assert(creates==before&&!on_transaction_lock);
/* A late paused callback does not advance voices; retain the buffer exactly
 * once and do not lose it if its first resume enqueue fails. */
host_dsound_pause_output();uint64_t frames=host_dsound_output_frames();
memset(buffers[0]->mAudioData,0xff,buffers[0]->mAudioDataByteSize);
complete_buffer(0);audio_callback(NULL,output_queue,buffers[0]);
assert(output_deferred_count==1&&host_dsound_output_frames()==frames);
fail_enqueue=1;assert(!host_dsound_resume_output());
assert(output_deferred_count==1&&output_callbacks_suspended&&output_restart_required);
assert(host_dsound_resume_output());assert(!output_deferred_count&&!output_callbacks_suspended);
for(unsigned i=0;i<buffers[0]->mAudioDataByteSize;i++)assert(!((unsigned char *)buffers[0]->mAudioData)[i]);
/* A failed Start remains suspended, so late callbacks cannot consume audio. */
host_dsound_pause_output();fail_start=1;assert(!host_dsound_resume_output());
complete_buffer(1);assert(output_deferred_count==1&&host_dsound_output_frames()==frames);
assert(host_dsound_resume_output());assert(!output_deferred_count&&!output_callbacks_suspended);
/* A failed callback enqueue loses one queue slot. Later callbacks can still
 * advance frames and overwrite last_enqueue with success, but cannot restore
 * that slot. Recovery must observe the loss despite that apparent progress. */
now_ns+=3000000000ull;before=creates;frames=host_dsound_output_frames();
fail_enqueue=1;complete_buffer(0);assert(queued_count()==2&&host_dsound_output_frames()>frames);
complete_buffer(1);assert(queued_count()==2&&atomic_load(&output_last_enqueue)==0);
host_dsound_watchdog();assert(creates==before+1&&queued_count()==3);
/* Repeated failures remain bounded even while the other buffers advance. */
before=creates;fail_enqueue=1;complete_buffer(0);
for(unsigned i=0;i<10;i++){
    complete_buffer(1);complete_buffer(2);host_dsound_watchdog();
    assert(creates==before&&queued_count()==2);
    now_ns+=200000000ull;
}
host_dsound_watchdog();assert(creates==before+1&&queued_count()==3);
/* A known loss survives pause; resume grace must not hide that fault. */
before=creates;fail_enqueue=1;complete_buffer(0);host_dsound_pause_output();
frames=host_dsound_output_frames();now_ns+=3000000000ull;host_dsound_watchdog();
assert(creates==before&&host_dsound_output_frames()==frames);
assert(host_dsound_resume_output());host_dsound_watchdog();
assert(creates==before+1&&queued_count()==3);
/* Another lifecycle transaction may replace a faulty queue before this
 * watchdog obtains output_lock. The new healthy queue keeps its grace. */
before=creates;fail_enqueue=1;complete_buffer(0);now_ns+=3000000000ull;
on_transaction_lock=replace_while_locking;host_dsound_watchdog();
assert(creates==before+1&&queued_count()==3&&!on_transaction_lock);
/* Conversely, a callback can lose a buffer while the watchdog is waiting
 * for output_lock; that callback's frame progress must not hide the fault. */
before=creates;now_ns+=3000000000ull;on_transaction_lock=fault_while_locking;
host_dsound_watchdog();assert(creates==before+1&&queued_count()==3&&!on_transaction_lock);
/* Telemetry counts short queue faults even after a later enqueue succeeds,
 * and excludes a deliberate suspension from the callback-gap maximum. */
HostDsOutputDiagnostics diag0,diag1;
complete_buffer(0);host_dsound_get_output_diagnostics(&diag0);
now_ns+=100000000ull;complete_buffer(1);host_dsound_get_output_diagnostics(&diag1);
assert(diag1.callbacks==diag0.callbacks+1 && diag1.late_callbacks==diag0.late_callbacks+1);
assert(diag1.max_callback_gap_ns>=100000000ull && diag1.sample_rate==48000 && diag1.buffer_frames==1536);
host_dsound_pause_output();now_ns+=9000000000ull;assert(host_dsound_resume_output());
complete_buffer(2);host_dsound_get_output_diagnostics(&diag0);
assert(diag0.max_callback_gap_ns==diag1.max_callback_gap_ns);
before=creates;fail_enqueue=1;complete_buffer(0);
host_dsound_get_output_diagnostics(&diag1);assert(diag1.enqueue_failures==diag0.enqueue_failures+1);
atomic_store(&output_terminated,1);now_ns+=3000000000ull;host_dsound_watchdog();assert(creates==before);
pthread_mutex_lock(&output_lock);audio_dispose_locked();pthread_mutex_unlock(&output_lock);
puts("PASS watchdog: failed rebuild retries, retry bound, locked recovery, resume grace, concurrent progress, deferred callbacks, callback enqueue loss, fault/reset ordering, failed enqueue/start retry and termination");}
float halo_settings_head_yaw(void){return 0.f;}
float halo_settings_self_gain(void){return 1.f;}
