#include "directsound_mixer.h"
#include "haptics.h"
#include "halo_settings.h"
#include <math.h>
#include <string.h>

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static float dot3(const float a[3], const float b[3]) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static float len3(const float a[3]) { return sqrtf(dot3(a, a)); }
static void normalize3(float v[3], const float fallback[3]) {
    float n = len3(v);
    if (!(n > 1.0e-8f) || !isfinite(n)) { memcpy(v, fallback, 3*sizeof(float)); return; }
    v[0] /= n; v[1] /= n; v[2] /= n;
}
static float millibel_gain(int32_t value) {
    if (value <= -10000) return 0.f;
    if (value >= 0) return 1.f;
    return powf(10.f, (float)value / 2000.f);
}

void ds_mixer_init(DsMixer *m) {
    memset(m, 0, sizeof *m);
    pthread_mutex_init(&m->lock, NULL);
    m->current_listener.front[2] = 1.f;
    m->current_listener.up[1] = 1.f;
    m->current_listener.distance_factor = 1.f;
    m->current_listener.rolloff_factor = 1.f;
    m->current_listener.doppler_factor = 1.f;
    m->deferred_listener = m->current_listener;
}

int ds_mixer_add(DsMixer *m, DsMixerVoice *v) {
    int result = 0;
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < DS_MIXER_MAX_VOICES; ++i) if (!m->voices[i]) {
        m->voices[i] = v; result = 1; break;
    }
    pthread_mutex_unlock(&m->lock);
    return result;
}

void ds_mixer_remove(DsMixer *m, DsMixerVoice *v) {
    pthread_mutex_lock(&m->lock);
    for (int i = 0; i < DS_MIXER_MAX_VOICES; ++i)
        if (m->voices[i] == v) { m->voices[i] = NULL; break; }
    pthread_mutex_unlock(&m->lock);
}

void ds_mixer_commit(DsMixer *m) {
    pthread_mutex_lock(&m->lock);
    if (m->pending_listener) {
        m->current_listener = m->deferred_listener;
        m->pending_listener = 0;
    }
    for (int i = 0; i < DS_MIXER_MAX_VOICES; ++i) {
        DsMixerVoice *v = m->voices[i];
        if (v && v->pending_3d) {
            v->current_3d = v->deferred_3d;
            v->pending_3d = 0;
        }
    }
    pthread_mutex_unlock(&m->lock);
}

static float read_channel(const DsMixerVoice *v, uint32_t frame, uint32_t channel) {
    uint32_t frames = v->format.block_align ? v->bytes / v->format.block_align : 0;
    if (!frames || frame >= frames || channel >= v->format.channels) return 0.f;
    const uint8_t *p = v->data + (size_t)frame * v->format.block_align;
    if (v->format.tag == 1 && v->format.bits == 8)
        return ((float)p[channel] - 128.f) / 128.f;
    if (v->format.tag == 1 && v->format.bits == 16) {
        int16_t sample; memcpy(&sample, p + 2u*channel, sizeof sample);
        return (float)sample / 32768.f;
    }
    if (v->format.tag == 3 && v->format.bits == 32) {
        float sample; memcpy(&sample, p + 4u*channel, sizeof sample);
        return isfinite(sample) ? clampf(sample, -1.f, 1.f) : 0.f;
    }
    return 0.f;
}

void ds_mixer_voice_gains(const DsMixer *m, const DsMixerVoice *v, float *left, float *right) {
    float gain = millibel_gain(v->volume), pan_l = 1.f, pan_r = 1.f;
    if (v->pan > 0) pan_l = millibel_gain(-v->pan);
    else if (v->pan < 0) pan_r = millibel_gain(v->pan);
    if (v->has_3d && v->current_3d.mode != 2) {
        const DsMixer3D *p = &v->current_3d;
        const DsMixerListener *l = &m->current_listener;
        float relative[3] = { p->position[0], p->position[1], p->position[2] };
        if (p->mode == 0) {
            relative[0] -= l->position[0]; relative[1] -= l->position[1]; relative[2] -= l->position[2];
        }
        /* The distance factor is how many metres one vector unit represents.
         * It converts velocity for Doppler; it is NOT part of the rolloff.
         * Minimum distance, maximum distance and the position are all in the
         * same vector units, so the ratio below is already unit free.
         * Multiplying only the distance by it broke that ratio: Halo sets the
         * factor to 3.048, ten feet in metres, which quietly pushed every
         * positional sound 9.7 dB down while 2D music was untouched. A sound
         * sitting exactly at its minimum distance is meant to play at full
         * volume, and with the factor applied it never could. */
        float distance = len3(relative);
        float minimum = p->min_distance > 1.0e-4f ? p->min_distance : 1.f;
        float maximum = p->max_distance >= minimum ? p->max_distance : minimum;
        if (distance > minimum) {
            float limited = distance > maximum ? maximum : distance;
            float rolloff = l->rolloff_factor >= 0.f ? l->rolloff_factor : 1.f;
            gain *= powf(minimum / limited, rolloff);
        }
        /* The voice Halo places on the listener is the player's own weapon,
         * reload and melee, handed over twenty decibels down (the assault
         * rifle at -2000 mB): in a recording of a headset firefight, firing
         * raised the mix by under a decibel. Lift it back. */
        if (distance < 0.5f || v->self_voice) gain *= halo_settings_self_gain();
        float direction[3] = { relative[0], relative[1], relative[2] };
        static const float forward[3] = { 0.f, 0.f, 1.f };
        normalize3(direction, forward);
        float listener_front[3] = { l->front[0], l->front[1], l->front[2] };
        float listener_up[3] = { l->up[0], l->up[1], l->up[2] };
        normalize3(listener_front, forward);
        static const float upward[3] = { 0.f, 1.f, 0.f };
        normalize3(listener_up, upward);
        float listener_right[3] = {
            listener_up[1]*listener_front[2] - listener_up[2]*listener_front[1],
            listener_up[2]*listener_front[0] - listener_up[0]*listener_front[2],
            listener_up[0]*listener_front[1] - listener_up[1]*listener_front[0]
        };
        static const float rightward[3] = { 1.f, 0.f, 0.f };
        normalize3(listener_right, rightward);
        /* The panorama lets the viewer look away from where the game camera
         * points. Turning the head right by yaw swings the listener's right
         * vector the same way, so a sound keeps its place in the world
         * instead of following the eyes. */
        float yaw = halo_settings_head_yaw();
        if (yaw > 1.0e-4f || yaw < -1.0e-4f) {
            float c = cosf(yaw), s = sinf(yaw);
            for (int axis = 0; axis < 3; ++axis) {
                float r = listener_right[axis], f = listener_front[axis];
                listener_right[axis] = r*c - f*s;
                listener_front[axis] = f*c + r*s;
            }
        }
        float spatial_pan = clampf(dot3(direction, listener_right), -1.f, 1.f);
        if (spatial_pan > 0) pan_l *= sqrtf(1.f - spatial_pan);
        else pan_r *= sqrtf(1.f + spatial_pan);

        float cone[3] = { p->cone[0], p->cone[1], p->cone[2] };
        if (len3(cone) > 1.0e-5f && p->outside_angle < 360u) {
            normalize3(cone, forward);
            float toward_listener[3] = { -direction[0], -direction[1], -direction[2] };
            float degrees = acosf(clampf(dot3(cone, toward_listener), -1.f, 1.f)) * 57.2957795f * 2.f;
            float inside = (float)p->inside_angle, outside = (float)p->outside_angle;
            float cone_mb = 0.f;
            if (degrees >= outside) cone_mb = (float)p->outside_volume;
            else if (degrees > inside && outside > inside)
                cone_mb = (float)p->outside_volume * (degrees - inside) / (outside - inside);
            gain *= millibel_gain((int32_t)cone_mb);
        }
    }
    *left = gain * pan_l; *right = gain * pan_r;
}

void ds_mixer_voice_restart(DsMixerVoice *v) {
    /* A new sound's attack is its loudest instant. The pulse gap only keeps
     * one voice's own pulses apart, so a restart is eligible at once: with the
     * gap cleared here, the first 45 ms of every sound Halo started with Play
     * (all 133 in the Build73 session) could never pulse, and a shot's crack
     * has faded by then. */
    v->onset_fast = v->onset_slow = 0.f; v->onset_gap = UINT32_MAX;
    v->onset_wait = 0; v->onset_armed = 1;
    v->self_voice = 0;
}

/* How far a positional voice is from the listener, in the game's units. */
float ds_mixer_voice_distance(const DsMixer *m, const DsMixerVoice *v) {
    const DsMixer3D *p = &v->current_3d;
    float relative[3] = { p->position[0], p->position[1], p->position[2] };
    if (p->mode == 0) {
        const DsMixerListener *l = &m->current_listener;
        relative[0] -= l->position[0]; relative[1] -= l->position[1]; relative[2] -= l->position[2];
    }
    float d = len3(relative);
    return isfinite(d) ? d : 1.0e9f;
}

/* DirectSound positions are byte offsets at whole PCM frames. Computing the
 * lead in bytes first made 22,050 Hz streams return half-frame write cursors. */
uint32_t ds_mixer_write_cursor(const DsMixerVoice *v) {
    uint32_t align = v->format.block_align;
    uint32_t frames = align ? v->bytes / align : 0;
    if (!frames) return 0;
    uint32_t play = (uint32_t)v->cursor_frames % frames;
    uint32_t lead = v->format.sample_rate / 20u;
    return ((play + lead) % frames) * align;
}

/* Catmull-Rom. Halo's effects are 22,050 Hz and the device runs at 48,000, so
 * every voice is resampled. Linear interpolation is a lowpass at that ratio
 * and costs the top of the band, which is what makes the mix sound muffled.
 * Four taps restore it for two more reads per frame. */
static float cubic(float p0, float p1, float p2, float p3, float t) {
    float a = 2.f*p1;
    float b = p2 - p0;
    float c = 2.f*p0 - 5.f*p1 + 4.f*p2 - p3;
    float d = -p0 + 3.f*p1 - 3.f*p2 + p3;
    return 0.5f * (a + t*(b + t*(c + t*d)));
}

/* Halo mixes many voices into one bus and a busy firefight sums past full
 * scale. Squaring the peaks off at +/-1 turns that into buzz, so the curve
 * bends smoothly to the ceiling instead: unity below the knee, never past
 * one above it. The knee sits high on purpose. A single loud sound already
 * reaches full scale in this game, and a low knee would quietly compress
 * every one of them; at 0.90 a full-scale peak loses 0.45 dB, which is not
 * audible, while a mix at twice full scale still lands under the ceiling. */
static float soften(float v) {
    const float knee = 0.90f;
    float magnitude = v < 0.f ? -v : v;
    if (!(magnitude > knee)) return v;
    if (!isfinite(magnitude)) return v < 0.f ? -1.f : 1.f;
    float over = (magnitude - knee) / (1.f - knee);
    float shaped = knee + (1.f - knee) * (over / (1.f + over));
    return v < 0.f ? -shaped : shaped;
}

void ds_mixer_render(DsMixer *m, float *out, uint32_t count, uint32_t output_rate) {
    if (!out || !count || !output_rate) return;
    memset(out, 0, (size_t)count * 2u * sizeof(float));
    /* Onset envelopes: a three-millisecond attack against a fifteen-
     * millisecond reference, so only a rise that happens within a few
     * milliseconds counts (a crack, not a swell); five milliseconds to take
     * the rise's peak; and pulses no closer than forty-five milliseconds,
     * which lets an assault rifle's shots each land. */
    float onset_fast_alpha = 1.f - expf(-1.f / (0.003f * (float)output_rate));
    float onset_ref_alpha = 1.f - expf(-1.f / (0.015f * (float)output_rate));
    uint16_t onset_peak_frames = (uint16_t)(output_rate / 200u);
    uint32_t onset_gap_frames = output_rate / 22u;
    pthread_mutex_lock(&m->lock);
    for (int index = 0; index < DS_MIXER_MAX_VOICES; ++index) {
        DsMixerVoice *v = m->voices[index];
        if (!v || !v->playing || !v->data || !v->format.block_align) continue;
        uint32_t source_frames = v->bytes / v->format.block_align;
        if (!source_frames) { v->playing = 0; v->end_count++; m->ended_voices++; continue; }
        uint32_t frequency = v->frequency ? v->frequency : v->format.sample_rate;
        double step = (double)frequency / (double)output_rate;
        int track_onset = v->has_3d && v->current_3d.mode != 2;
        if (track_onset && !v->self_voice && ds_mixer_voice_distance(m, v) < 0.5f) v->self_voice = 1;
        float gain_l, gain_r; ds_mixer_voice_gains(m, v, &gain_l, &gain_r);
        if (!(gain_l > 0.f) && !(gain_r > 0.f)) {
            /* Halo keeps streaming into voices it has muted (-10,000 mB);
             * the trace shows several fed continuously. Advance them without
             * resampling silence, ending or wrapping as the loop would. */
            v->cursor_frames += step * (double)count;
            if (v->cursor_frames >= source_frames) {
                if (v->looping) v->cursor_frames = fmod(v->cursor_frames, source_frames);
                else { v->playing = 0; v->end_count++; m->ended_voices++; }
            }
            continue;
        }
        int pair = v->format.channels == 2;
        /* Impacts for the haptics come from the positional voices: weapons,
         * hits and explosions are 3D in Halo, dialogue and music are not. A
         * rise of the rendered level well above the voice's own recent
         * trough is a sound starting inside its stream, whatever else is
         * playing. The one voice Halo places at the listener is the player's
         * own weapon, which it plays twenty decibels down; that one is
         * judged on its own scale, for as long as that sound plays. */
        int at_listener = track_onset && v->self_voice;
        /* The listener's voice is judged at the level Halo gave it, not
         * after the lift the player's weapon gets on the way out. */
        float onset_scale = at_listener ? 1.f / halo_settings_self_gain() : 1.f;
        for (uint32_t i = 0; i < count; ++i) {
            if (v->cursor_frames >= source_frames) {
                if (v->looping) v->cursor_frames = fmod(v->cursor_frames, source_frames);
                else { v->playing = 0; v->end_count++; m->ended_voices++; break; }
            }
            uint32_t a = (uint32_t)v->cursor_frames;
            float fraction = (float)(v->cursor_frames - a);
            /* A looping voice wraps its neighbours so the seam interpolates
             * across the loop point; a one-shot holds its end frames. */
            uint32_t before = a ? a - 1u : (v->looping ? source_frames - 1u : 0u);
            uint32_t b = a + 1u, after = a + 2u;
            if (b >= source_frames) b = v->looping ? b - source_frames : source_frames - 1u;
            if (after >= source_frames) after = v->looping ? after % source_frames : source_frames - 1u;
            float l0 = read_channel(v, before, 0), l1 = read_channel(v, a, 0);
            float l2 = read_channel(v, b, 0), l3 = read_channel(v, after, 0);
            float left = cubic(l0, l1, l2, l3, fraction), right;
            if (pair) {
                float r0 = read_channel(v, before, 1), r1 = read_channel(v, a, 1);
                float r2 = read_channel(v, b, 1), r3 = read_channel(v, after, 1);
                right = cubic(r0, r1, r2, r3, fraction);
            } else right = left;
            out[2u*i] += left * gain_l;
            out[2u*i+1u] += right * gain_r;
            v->cursor_frames += step;
            if (track_onset) {
                float level = fabsf(left * gain_l), other = fabsf(right * gain_r);
                if (other > level) level = other;
                level *= onset_scale;
                v->onset_fast += onset_fast_alpha * (level - v->onset_fast);
                if (v->onset_fast > v->onset_level_max) v->onset_level_max = v->onset_fast;
                /* The reference lags the attack by a dozen milliseconds:
                 * a crack stands twice over it for its first instants, a
                 * swell or a hum never does, and between the shots of a
                 * burst the attack falls back under it and re-arms. */
                v->onset_slow += onset_ref_alpha * (v->onset_fast - v->onset_slow);
                if (v->onset_gap < onset_gap_frames) v->onset_gap++;
                if (v->onset_wait) {
                    /* A rise was seen; take its peak before reporting it. */
                    if (v->onset_fast > v->onset_peak) v->onset_peak = v->onset_fast;
                    if (--v->onset_wait == 0) {
                        v->onset_gap = 0; v->onset_reported++;
                        halo_haptics_voice_onset(v->onset_peak, v->onset_floor, at_listener);
                    }
                } else if (v->onset_fast < 1.2f * v->onset_slow + 0.005f) {
                    v->onset_armed = 1;
                } else if (v->onset_armed && v->onset_gap >= onset_gap_frames &&
                           v->onset_fast > 0.004f &&
                           v->onset_fast > 2.f * v->onset_slow) {
                    v->onset_armed = 0;
                    v->onset_peak = v->onset_fast; v->onset_floor = v->onset_slow;
                    v->onset_wait = onset_peak_frames;
                }
            }
        }
    }
    /* How often the sum would have hit the old hard ceiling. Reported once
     * so a capture can say whether clipping was real or theoretical. */
    for (uint32_t i = 0; i < count*2u; ++i) {
        if (out[i] > 1.f || out[i] < -1.f) m->limited_samples++;
        out[i] = soften(out[i]);
    }
    m->rendered_samples += count*2u;
    m->rendered_frames += count;
    pthread_mutex_unlock(&m->lock);
    halo_haptics_observe(out, count, output_rate);
}
