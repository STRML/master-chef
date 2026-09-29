#ifndef HALO_DIRECTSOUND_H
#define HALO_DIRECTSOUND_H

#include "host.h"

void host_dsound_create8(EngineCPU *cpu);
void host_dsound_create(EngineCPU *cpu);
/* Synchronously stops and disposes the output queue. Call after translated
 * engine threads have stopped making DirectSound calls and before teardown of
 * guest memory or other EngineHost process state. */
void host_dsound_shutdown(void);
void host_dsound_watchdog(void);
/* The app's audio queue takes the watchdog over from Present, so it keeps
 * running while the engine stalls in a load. One caller at a time. */
void host_dsound_watchdog_set_external(void);
int host_dsound_watchdog_is_external(void);
uint32_t host_dsound_watchdog_rebuilds(void);
/* Session recovery preserves mixer voices, cursors, diagnostics and capture.
 * Call from a serialized control context, never the AudioQueue callback.
 * Reactivate AVAudioSession before resume. Rebuild replaces only queue/buffers
 * and does not start playback; follow it with resume when playback is allowed. */
void host_dsound_pause_output(void);
int host_dsound_resume_output(void);
int host_dsound_rebuild_output(void);
void host_dsound_get_stats(uint64_t *frames, uint64_t *nonzero_samples, float *peak);
typedef struct {
    uint32_t generation, requested, suspended, restart_required, callback_suspended, deferred, terminated;
    int32_t last_prepare, last_start, last_enqueue, last_pause, last_dispose, last_recovery;
    /* Cumulative callback evidence. Gaps exclude known pauses/rebuilds;
     * late means >2 buffer periods, not a hardware underrun verdict. */
    uint64_t callbacks, late_callbacks, enqueue_failures, last_callback_ns;
    uint64_t max_callback_gap_ns, max_callback_work_ns;
    uint32_t sample_rate, buffer_frames;
} HostDsOutputDiagnostics;
void host_dsound_get_output_diagnostics(HostDsOutputDiagnostics *state);
/* How the engine's voices are being used, for the device report. Never call
 * from the AudioQueue callback.
 * Host side, cumulative: DirectSound Play and Stop calls accepted, GetStatus
 * calls and those answering PLAYING, calls refused. Now: secondary buffers,
 * those playing, and the most seen playing.
 * Engine side, read from guest memory (directsound_engine_state.inc), -1
 * where not readable yet: engine_flags (1 initialised, 2 enabled, 4 paused by
 * the game, 8 disabled); live sound sources (of 512) and looping sounds (of
 * 128); channels, those with a sound, and those with a sound but no voice;
 * voices, those on a channel, those the engine could take for a new sound,
 * and those it could not though unused ("held"), overall and by voice type
 * (the order of the engine's type table at 0x0069F528; the first is the
 * positional voices); the sound cache's sounds (of 512), loaded and locked;
 * and cache-file reads queued for the level map, bitmaps.map and sounds.map. */
enum { HOST_DS_VOICE_TYPES = 4 };
typedef struct {
    uint64_t plays, stops, status_probes, status_busy, rejected;
    uint32_t buffers, playing, playing_peak;
    int32_t engine_flags, sources, looping_sounds, channels, channels_busy, channels_starved;
    int32_t voices, voices_assigned, voices_free, voices_held;
    int32_t voices_by_type[HOST_DS_VOICE_TYPES], free_by_type[HOST_DS_VOICE_TYPES], held_by_type[HOST_DS_VOICE_TYPES];
    int32_t cache_sounds, cache_loaded, cache_locked, reads_queued[3];
} HostDsVoiceStats;
void host_dsound_get_voice_stats(HostDsVoiceStats *stats);
void host_dsound_get_output_state(uint32_t *running,int32_t *status);
/* Shared lazy-on trace switch: an early UI stats poll must not cache it off
 * before the engine worker configures its environment. Never logs in callback. */
int host_dsound_trace_enabled(void);
uint64_t host_dsound_output_frames(void);

#define DSOUND_DECLARE(prefix, n) void host_dsound_##prefix##_##n(EngineCPU *cpu)
DSOUND_DECLARE(device,0); DSOUND_DECLARE(device,1); DSOUND_DECLARE(device,2); DSOUND_DECLARE(device,3);
DSOUND_DECLARE(device,4); DSOUND_DECLARE(device,5); DSOUND_DECLARE(device,6); DSOUND_DECLARE(device,7);
DSOUND_DECLARE(device,8); DSOUND_DECLARE(device,9); DSOUND_DECLARE(device,10); DSOUND_DECLARE(device,11);
DSOUND_DECLARE(buffer,0); DSOUND_DECLARE(buffer,1); DSOUND_DECLARE(buffer,2); DSOUND_DECLARE(buffer,3);
DSOUND_DECLARE(buffer,4); DSOUND_DECLARE(buffer,5); DSOUND_DECLARE(buffer,6); DSOUND_DECLARE(buffer,7);
DSOUND_DECLARE(buffer,8); DSOUND_DECLARE(buffer,9); DSOUND_DECLARE(buffer,10); DSOUND_DECLARE(buffer,11);
DSOUND_DECLARE(buffer,12); DSOUND_DECLARE(buffer,13); DSOUND_DECLARE(buffer,14); DSOUND_DECLARE(buffer,15);
DSOUND_DECLARE(buffer,16); DSOUND_DECLARE(buffer,17); DSOUND_DECLARE(buffer,18); DSOUND_DECLARE(buffer,19);
DSOUND_DECLARE(buffer,20); DSOUND_DECLARE(buffer,21); DSOUND_DECLARE(buffer,22); DSOUND_DECLARE(buffer,23);
DSOUND_DECLARE(listener,0); DSOUND_DECLARE(listener,1); DSOUND_DECLARE(listener,2); DSOUND_DECLARE(listener,3);
DSOUND_DECLARE(listener,4); DSOUND_DECLARE(listener,5); DSOUND_DECLARE(listener,6); DSOUND_DECLARE(listener,7);
DSOUND_DECLARE(listener,8); DSOUND_DECLARE(listener,9); DSOUND_DECLARE(listener,10); DSOUND_DECLARE(listener,11);
DSOUND_DECLARE(listener,12); DSOUND_DECLARE(listener,13); DSOUND_DECLARE(listener,14); DSOUND_DECLARE(listener,15);
DSOUND_DECLARE(listener,16); DSOUND_DECLARE(listener,17);
DSOUND_DECLARE(buffer3d,0); DSOUND_DECLARE(buffer3d,1); DSOUND_DECLARE(buffer3d,2); DSOUND_DECLARE(buffer3d,3);
DSOUND_DECLARE(buffer3d,4); DSOUND_DECLARE(buffer3d,5); DSOUND_DECLARE(buffer3d,6); DSOUND_DECLARE(buffer3d,7);
DSOUND_DECLARE(buffer3d,8); DSOUND_DECLARE(buffer3d,9); DSOUND_DECLARE(buffer3d,10); DSOUND_DECLARE(buffer3d,11);
DSOUND_DECLARE(buffer3d,12); DSOUND_DECLARE(buffer3d,13); DSOUND_DECLARE(buffer3d,14); DSOUND_DECLARE(buffer3d,15);
DSOUND_DECLARE(buffer3d,16); DSOUND_DECLARE(buffer3d,17); DSOUND_DECLARE(buffer3d,18); DSOUND_DECLARE(buffer3d,19);
DSOUND_DECLARE(buffer3d,20);
#undef DSOUND_DECLARE

/* These names are also used to build the guest COM vtables. Keeping the
 * methods in the normal import table lets the translated indirect calls use
 * the same checked host boundary as ordinary DLL imports. */
#define DSE(iface,method,fn) { "DSOUND.dll", iface "::" method, fn }
#define HOST_DSOUND_SHIM_ENTRIES \
 DSE("IDirectSound8","QueryInterface",host_dsound_device_0), DSE("IDirectSound8","AddRef",host_dsound_device_1), DSE("IDirectSound8","Release",host_dsound_device_2), \
 DSE("IDirectSound8","CreateSoundBuffer",host_dsound_device_3), DSE("IDirectSound8","GetCaps",host_dsound_device_4), DSE("IDirectSound8","DuplicateSoundBuffer",host_dsound_device_5), \
 DSE("IDirectSound8","SetCooperativeLevel",host_dsound_device_6), DSE("IDirectSound8","Compact",host_dsound_device_7), DSE("IDirectSound8","GetSpeakerConfig",host_dsound_device_8), \
 DSE("IDirectSound8","SetSpeakerConfig",host_dsound_device_9), DSE("IDirectSound8","Initialize",host_dsound_device_10), DSE("IDirectSound8","VerifyCertification",host_dsound_device_11), \
 DSE("IDirectSoundBuffer8","QueryInterface",host_dsound_buffer_0), DSE("IDirectSoundBuffer8","AddRef",host_dsound_buffer_1), DSE("IDirectSoundBuffer8","Release",host_dsound_buffer_2), \
 DSE("IDirectSoundBuffer8","GetCaps",host_dsound_buffer_3), DSE("IDirectSoundBuffer8","GetCurrentPosition",host_dsound_buffer_4), DSE("IDirectSoundBuffer8","GetFormat",host_dsound_buffer_5), \
 DSE("IDirectSoundBuffer8","GetVolume",host_dsound_buffer_6), DSE("IDirectSoundBuffer8","GetPan",host_dsound_buffer_7), DSE("IDirectSoundBuffer8","GetFrequency",host_dsound_buffer_8), \
 DSE("IDirectSoundBuffer8","GetStatus",host_dsound_buffer_9), DSE("IDirectSoundBuffer8","Initialize",host_dsound_buffer_10), DSE("IDirectSoundBuffer8","Lock",host_dsound_buffer_11), \
 DSE("IDirectSoundBuffer8","Play",host_dsound_buffer_12), DSE("IDirectSoundBuffer8","SetCurrentPosition",host_dsound_buffer_13), DSE("IDirectSoundBuffer8","SetFormat",host_dsound_buffer_14), \
 DSE("IDirectSoundBuffer8","SetVolume",host_dsound_buffer_15), DSE("IDirectSoundBuffer8","SetPan",host_dsound_buffer_16), DSE("IDirectSoundBuffer8","SetFrequency",host_dsound_buffer_17), \
 DSE("IDirectSoundBuffer8","Stop",host_dsound_buffer_18), DSE("IDirectSoundBuffer8","Unlock",host_dsound_buffer_19), DSE("IDirectSoundBuffer8","Restore",host_dsound_buffer_20), \
 DSE("IDirectSoundBuffer8","SetFX",host_dsound_buffer_21), DSE("IDirectSoundBuffer8","AcquireResources",host_dsound_buffer_22), DSE("IDirectSoundBuffer8","GetObjectInPath",host_dsound_buffer_23), \
 DSE("IDirectSound3DListener8","QueryInterface",host_dsound_listener_0), DSE("IDirectSound3DListener8","AddRef",host_dsound_listener_1), DSE("IDirectSound3DListener8","Release",host_dsound_listener_2), \
 DSE("IDirectSound3DListener8","GetAllParameters",host_dsound_listener_3), DSE("IDirectSound3DListener8","GetDistanceFactor",host_dsound_listener_4), DSE("IDirectSound3DListener8","GetDopplerFactor",host_dsound_listener_5), \
 DSE("IDirectSound3DListener8","GetOrientation",host_dsound_listener_6), DSE("IDirectSound3DListener8","GetPosition",host_dsound_listener_7), DSE("IDirectSound3DListener8","GetRolloffFactor",host_dsound_listener_8), \
 DSE("IDirectSound3DListener8","GetVelocity",host_dsound_listener_9), DSE("IDirectSound3DListener8","SetAllParameters",host_dsound_listener_10), DSE("IDirectSound3DListener8","SetDistanceFactor",host_dsound_listener_11), \
 DSE("IDirectSound3DListener8","SetDopplerFactor",host_dsound_listener_12), DSE("IDirectSound3DListener8","SetOrientation",host_dsound_listener_13), DSE("IDirectSound3DListener8","SetPosition",host_dsound_listener_14), \
 DSE("IDirectSound3DListener8","SetRolloffFactor",host_dsound_listener_15), DSE("IDirectSound3DListener8","SetVelocity",host_dsound_listener_16), DSE("IDirectSound3DListener8","CommitDeferredSettings",host_dsound_listener_17), \
 DSE("IDirectSound3DBuffer8","QueryInterface",host_dsound_buffer3d_0), DSE("IDirectSound3DBuffer8","AddRef",host_dsound_buffer3d_1), DSE("IDirectSound3DBuffer8","Release",host_dsound_buffer3d_2), \
 DSE("IDirectSound3DBuffer8","GetAllParameters",host_dsound_buffer3d_3), DSE("IDirectSound3DBuffer8","GetConeAngles",host_dsound_buffer3d_4), DSE("IDirectSound3DBuffer8","GetConeOrientation",host_dsound_buffer3d_5), \
 DSE("IDirectSound3DBuffer8","GetConeOutsideVolume",host_dsound_buffer3d_6), DSE("IDirectSound3DBuffer8","GetMaxDistance",host_dsound_buffer3d_7), DSE("IDirectSound3DBuffer8","GetMinDistance",host_dsound_buffer3d_8), \
 DSE("IDirectSound3DBuffer8","GetMode",host_dsound_buffer3d_9), DSE("IDirectSound3DBuffer8","GetPosition",host_dsound_buffer3d_10), DSE("IDirectSound3DBuffer8","GetVelocity",host_dsound_buffer3d_11), \
 DSE("IDirectSound3DBuffer8","SetAllParameters",host_dsound_buffer3d_12), DSE("IDirectSound3DBuffer8","SetConeAngles",host_dsound_buffer3d_13), DSE("IDirectSound3DBuffer8","SetConeOrientation",host_dsound_buffer3d_14), \
 DSE("IDirectSound3DBuffer8","SetConeOutsideVolume",host_dsound_buffer3d_15), DSE("IDirectSound3DBuffer8","SetMaxDistance",host_dsound_buffer3d_16), DSE("IDirectSound3DBuffer8","SetMinDistance",host_dsound_buffer3d_17), \
 DSE("IDirectSound3DBuffer8","SetMode",host_dsound_buffer3d_18), DSE("IDirectSound3DBuffer8","SetPosition",host_dsound_buffer3d_19), DSE("IDirectSound3DBuffer8","SetVelocity",host_dsound_buffer3d_20)

#endif
