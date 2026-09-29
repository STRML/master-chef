#ifndef ENGINE_DIAGNOSTICS_BRIDGE_H
#define ENGINE_DIAGNOSTICS_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint64_t resident_bytes, virtual_bytes, controller_sequence, gamepad_reads, keyboard_events;
    uint32_t controller_connected, button_mask;
    float axes[6];
    /* Appended for ABI compatibility. A value may legitimately be zero, so
       consult memory_metric_flags to distinguish zero from unavailable. */
    uint64_t physical_footprint_bytes, metal_current_allocated_bytes;
    uint32_t memory_metric_flags;
    uint64_t audio_frames, audio_nonzero_samples;
    float audio_peak, audio_output_volume;
    uint32_t audio_queue_running;
    int32_t audio_queue_status;
    uint32_t audio_queue_generation, audio_queue_suspended, audio_queue_restart, audio_queue_deferred;
    int32_t audio_last_prepare, audio_last_start, audio_last_enqueue, audio_last_pause, audio_last_dispose, audio_last_recovery;
    /* host_heap_stats: live/peak/total requested bytes, live allocations,
     * metadata bytes, address high-water bytes. */
    uint64_t guest_heap[6];
    /* Host work outside the render passes that only the headset pays.
     * Controller captures taken and polls answered from a recent capture
     * (window in microseconds, 0 = every poll captures); DirectInput Acquire
     * calls refused while the pad was away; the engine's time in Halo's
     * blocking cache-read waits (and whether those block rather than spin);
     * the cache reader's reads; the guest threads' qos_class_t. */
    uint64_t controller_captures, controller_reuses, dinput_acquire_lost;
    uint64_t cache_wait_calls, cache_wait_ns, cache_reads, cache_read_ns;
    uint32_t controller_reuse_us, cache_wait_blocks, guest_thread_qos, guest_threads;
    /* The engine's voices (host_dsound_get_voice_stats, directsound.h):
     * cumulative DirectSound Play and Stop calls, GetStatus probes and busy
     * answers, refused calls; buffers and voices playing now and the peak.
     * sound_* are the original engine's own tables read from guest memory,
     * -1 until readable; by-type arrays follow the engine's voice-type
     * table, the first being the positional voices. */
    uint64_t audio_voice_plays, audio_voice_stops, audio_status_probes, audio_status_busy, audio_calls_rejected;
    uint32_t audio_buffers, audio_voices_playing, audio_voices_peak;
    int32_t sound_engine_flags, sound_sources, sound_looping_sounds;
    int32_t sound_channels, sound_channels_busy, sound_channels_starved;
    int32_t sound_voices, sound_voices_assigned, sound_voices_free, sound_voices_held;
    int32_t sound_voices_by_type[4], sound_free_by_type[4], sound_held_by_type[4];
    int32_t sound_cache_sounds, sound_cache_loaded, sound_cache_locked;
    /* Cache-file reads the engine has queued: level map, bitmaps, sounds. */
    int32_t sound_reads_queued[3];
    /* ReadFileEx reads and bytes, cumulative, by file (host.h HOST_READ_*):
     * the level map, bitmaps.map, sounds.map, anything else. */
    uint64_t cache_file_reads[4], cache_file_read_bytes[4];
    /* Host draw shortcuts: opt-in switch and cumulative traffic. Resident
     * bytes count repeated bindings, not allocated memory. */
    uint32_t draw_fastpath_enabled;
    uint64_t draw_resident_vertex_bytes, draw_arena_vertex_bytes, draw_folded_clears;
    /* HALO_NATIVE_GATHER's effective state and cumulative dispatch counts. */
    uint64_t native_gather_calls, native_gather_native_calls, native_gather_fallback_calls;
    uint32_t native_gather_enabled;
    uint64_t audio_callbacks, audio_late_callbacks, audio_enqueue_failures;
    uint64_t audio_callback_age_ns, audio_max_callback_gap_ns, audio_max_callback_work_ns;
    uint32_t audio_sample_rate, audio_buffer_frames;
} EngineDiagnosticSample;
enum {
    ENGINE_DIAGNOSTIC_HAS_PHYSICAL_FOOTPRINT = 1u << 0,
    ENGINE_DIAGNOSTIC_HAS_METAL_ALLOCATED_SIZE = 1u << 1
};
void enginevision_diagnostic_sample(EngineDiagnosticSample *sample);
int enginevision_capture_host_log(const char *path);
#endif
