#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include "EngineDiagnosticsBridge.h"
#include "gamecontroller.h"
#include "directsound.h"
#include "metalrenderer.h"
#include "gather_diagnostics.h"
#include <TargetConditionals.h>
#if TARGET_OS_VISION
#import <AVFAudio/AVFAudio.h>
#endif
uint64_t host_dinput_gamepad_reads(void);
uint64_t host_dinput_keyboard_events(void);
uint64_t host_dinput_acquire_lost(void);
void host_heap_stats(uint64_t out[6]);
extern uint64_t host_cache_wait_calls, host_cache_wait_ns, host_cache_reads, host_cache_read_ns;
int host_cache_wait_blocks(void);
uint32_t host_guest_thread_qos(void);
uint32_t host_guest_threads(void);

static id<MTLDevice> diagnostic_metal_device(void) {
    static id<MTLDevice> device;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ device = MTLCreateSystemDefaultDevice(); });
    return device;
}

void enginevision_diagnostic_sample(EngineDiagnosticSample *sample) {
    if(!sample)return;memset(sample,0,sizeof *sample);
    host_heap_stats(sample->guest_heap);
    sample->draw_fastpath_enabled=(uint32_t)mr_fast_paths_enabled();
    mr_draw_traffic_stats(&sample->draw_resident_vertex_bytes,&sample->draw_arena_vertex_bytes,&sample->draw_folded_clears);
    HostGatherDiagnostics gather;host_gather_get_diagnostics(&gather);
    sample->native_gather_enabled=gather.enabled;sample->native_gather_calls=gather.calls;
    sample->native_gather_native_calls=gather.native_calls;
    sample->native_gather_fallback_calls=gather.fallback_calls;
    mach_task_basic_info_data_t info;mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,(task_info_t)&info,&count)==KERN_SUCCESS){sample->resident_bytes=info.resident_size;sample->virtual_bytes=info.virtual_size;}
    task_vm_info_data_t vm_info;count=TASK_VM_INFO_COUNT;
    if(task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&vm_info,&count)==KERN_SUCCESS){
        sample->physical_footprint_bytes=vm_info.phys_footprint;
        sample->memory_metric_flags|=ENGINE_DIAGNOSTIC_HAS_PHYSICAL_FOOTPRINT;
    }
    id<MTLDevice> device=diagnostic_metal_device();
    if(device){
        sample->metal_current_allocated_bytes=(uint64_t)device.currentAllocatedSize;
        sample->memory_metric_flags|=ENGINE_DIAGNOSTIC_HAS_METAL_ALLOCATED_SIZE;
    }
    HostGCSnapshot snapshot;hostgc_poll(&snapshot);
    sample->controller_connected=snapshot.connected;sample->controller_sequence=snapshot.sequence;
    for(int i=0;i<HOSTGC_BUTTON_COUNT;i++)if(snapshot.buttons[i])sample->button_mask|=1u<<i;
    sample->axes[0]=snapshot.lx;sample->axes[1]=snapshot.ly;sample->axes[2]=snapshot.rx;sample->axes[3]=snapshot.ry;sample->axes[4]=snapshot.lt;sample->axes[5]=snapshot.rt;
    sample->gamepad_reads=host_dinput_gamepad_reads();
    sample->keyboard_events=host_dinput_keyboard_events();
    hostgc_poll_stats(&sample->controller_captures,&sample->controller_reuses,&sample->controller_reuse_us);
    sample->dinput_acquire_lost=host_dinput_acquire_lost();
    /* Plain reads of counters one thread advances, as for the sleep counters. */
    sample->cache_wait_calls=host_cache_wait_calls;sample->cache_wait_ns=host_cache_wait_ns;
    sample->cache_reads=host_cache_reads;sample->cache_read_ns=host_cache_read_ns;
    sample->cache_wait_blocks=(uint32_t)host_cache_wait_blocks();
    sample->guest_thread_qos=host_guest_thread_qos();sample->guest_threads=host_guest_threads();
    host_dsound_get_stats(&sample->audio_frames,&sample->audio_nonzero_samples,&sample->audio_peak);
    HostDsOutputDiagnostics output;host_dsound_get_output_diagnostics(&output);
    sample->audio_queue_generation=output.generation;sample->audio_queue_suspended=output.suspended;
    sample->audio_queue_restart=output.restart_required;sample->audio_queue_deferred=output.deferred;
    sample->audio_last_prepare=output.last_prepare;sample->audio_last_start=output.last_start;
    sample->audio_last_enqueue=output.last_enqueue;sample->audio_last_pause=output.last_pause;
    sample->audio_last_dispose=output.last_dispose;sample->audio_last_recovery=output.last_recovery;
    sample->audio_callbacks=output.callbacks;sample->audio_late_callbacks=output.late_callbacks;
    sample->audio_enqueue_failures=output.enqueue_failures;
    uint64_t now=clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    sample->audio_callback_age_ns=output.last_callback_ns && now>=output.last_callback_ns ? now-output.last_callback_ns : 0;
    sample->audio_max_callback_gap_ns=output.max_callback_gap_ns;sample->audio_max_callback_work_ns=output.max_callback_work_ns;
    sample->audio_sample_rate=output.sample_rate;sample->audio_buffer_frames=output.buffer_frames;
    host_dsound_get_output_state(&sample->audio_queue_running,&sample->audio_queue_status);
    HostDsVoiceStats voices;host_dsound_get_voice_stats(&voices);
    sample->audio_voice_plays=voices.plays;sample->audio_voice_stops=voices.stops;
    sample->audio_status_probes=voices.status_probes;sample->audio_status_busy=voices.status_busy;
    sample->audio_calls_rejected=voices.rejected;sample->audio_buffers=voices.buffers;
    sample->audio_voices_playing=voices.playing;sample->audio_voices_peak=voices.playing_peak;
    sample->sound_engine_flags=voices.engine_flags;sample->sound_sources=voices.sources;
    sample->sound_looping_sounds=voices.looping_sounds;sample->sound_channels=voices.channels;
    sample->sound_channels_busy=voices.channels_busy;sample->sound_channels_starved=voices.channels_starved;
    sample->sound_voices=voices.voices;sample->sound_voices_assigned=voices.voices_assigned;
    sample->sound_voices_free=voices.voices_free;sample->sound_voices_held=voices.voices_held;
    _Static_assert(HOST_DS_VOICE_TYPES==4,"voice types in the diagnostic sample");
    for(int t=0;t<HOST_DS_VOICE_TYPES;t++){
        sample->sound_voices_by_type[t]=voices.voices_by_type[t];
        sample->sound_free_by_type[t]=voices.free_by_type[t];sample->sound_held_by_type[t]=voices.held_by_type[t];
    }
    sample->sound_cache_sounds=voices.cache_sounds;sample->sound_cache_loaded=voices.cache_loaded;
    sample->sound_cache_locked=voices.cache_locked;
    for(int f=0;f<3;f++)sample->sound_reads_queued[f]=voices.reads_queued[f];
    _Static_assert(HOST_READ_KINDS==4,"read kinds in the diagnostic sample");
    for(int k=0;k<HOST_READ_KINDS;k++){sample->cache_file_reads[k]=host_async_reads[k];sample->cache_file_read_bytes[k]=host_async_read_bytes[k];}
#if TARGET_OS_VISION
    sample->audio_output_volume=[AVAudioSession sharedInstance].outputVolume;
#else
    sample->audio_output_volume=-1;
#endif
}
int enginevision_capture_host_log(const char *path) {
    if(!path)return EINVAL;
    int fd=open(path,O_WRONLY|O_CREAT|O_APPEND,0600);if(fd<0)return errno;
    fflush(stdout);fflush(stderr);
    int saved=0;
    if(dup2(fd,STDERR_FILENO)<0||dup2(fd,STDOUT_FILENO)<0)saved=errno;
    close(fd);setvbuf(stderr,NULL,_IONBF,0);setvbuf(stdout,NULL,_IOLBF,0);return saved;
}
