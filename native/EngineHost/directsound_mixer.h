#ifndef HALO_DIRECTSOUND_MIXER_H
#define HALO_DIRECTSOUND_MIXER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum { DS_MIXER_MAX_VOICES = 768 };

typedef struct {
    uint16_t tag, channels, bits, block_align;
    uint32_t sample_rate, avg_bytes_per_sec;
} DsMixerFormat;

typedef struct {
    float position[3], velocity[3], cone[3];
    float min_distance, max_distance;
    uint32_t inside_angle, outside_angle, mode;
    int32_t outside_volume;
} DsMixer3D;

typedef struct {
    float position[3], velocity[3], front[3], up[3];
    float distance_factor, rolloff_factor, doppler_factor;
} DsMixerListener;

typedef struct DsMixerVoice {
    uint8_t *data;
    uint32_t bytes, caps;
    DsMixerFormat format;
    double cursor_frames;
    uint64_t end_count; /* Natural non-looping completions; preserved across Play. */
    int32_t volume, pan;
    uint32_t frequency;
    uint8_t playing, looping, has_3d, pending_3d;
    /* Halo streams every sound through a pool of looping ring buffers, so a
     * new sound is a rise inside a voice, never a Play. A fast envelope of
     * the voice's rendered level against a lagging reference finds that
     * rise; the detector arms once the attack has settled back to the
     * reference, so a sustained sound pulses once, and measures the rise's
     * peak for a few milliseconds before reporting it. onset_gap holds the
     * pulses apart. Reset by Play. */
    float onset_fast, onset_slow, onset_peak, onset_floor;
    uint32_t onset_gap;
    uint16_t onset_wait;
    uint8_t onset_armed;
    /* For the voice trace: the loudest the fast envelope has been, and how
     * many rises were reported. */
    float onset_level_max;
    uint32_t onset_reported;
    /* Set once the voice has sounded within half a unit of the listener since
     * its last Play: the player's own weapon, reload or melee. It keeps the
     * lift and the listener-scale haptics for the rest of that sound. Judged
     * per block, the lift fell away mid-shot as soon as a moving player left
     * the shot more than half a unit behind: 18 dB, heard as the sound
     * cutting out. Reset by Play. */
    uint8_t self_voice;
    DsMixer3D current_3d, deferred_3d;
} DsMixerVoice;

typedef struct {
    pthread_mutex_t lock;
    DsMixerVoice *voices[DS_MIXER_MAX_VOICES];
    DsMixerListener current_listener, deferred_listener;
    uint8_t pending_listener;
    uint64_t rendered_frames, ended_voices;
    /* Samples that exceeded full scale before the limiter, and the total. */
    uint64_t limited_samples, rendered_samples;
} DsMixer;

/* Caller holds mixer.lock when inspecting a live voice. */
void ds_mixer_voice_gains(const DsMixer *mixer, const DsMixerVoice *voice,
                          float *left, float *right);
uint32_t ds_mixer_write_cursor(const DsMixerVoice *voice);
/* Caller holds mixer.lock. */
float ds_mixer_voice_distance(const DsMixer *mixer, const DsMixerVoice *voice);
/* Caller holds mixer.lock. The per-sound state Play resets. */
void ds_mixer_voice_restart(DsMixerVoice *voice);
void ds_mixer_init(DsMixer *mixer);
int ds_mixer_add(DsMixer *mixer, DsMixerVoice *voice);
void ds_mixer_remove(DsMixer *mixer, DsMixerVoice *voice);
void ds_mixer_commit(DsMixer *mixer);
void ds_mixer_render(DsMixer *mixer, float *interleaved_stereo,
                     uint32_t frames, uint32_t output_rate);

#endif
