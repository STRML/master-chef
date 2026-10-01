#ifdef __ANDROID__
#include "engine_compat_android.h"
#endif
#include "haptics.h"
#include "halo_settings.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>

/* One-pole low-pass at roughly 160 Hz isolates the band that carries impact
 * weight; an envelope follower with a fast attack and slow release turns it
 * into a level. An event fires when the level jumps well above its own recent
 * average, which is what distinguishes a hit from sustained music. */
enum { HAPTICS_MIN_GAP_FRAMES = 2000 };   /* ~45 ms at 44.1 kHz */

static _Atomic int haptics_enabled = 1;
/* Both fields belong to one event. A single exchange prevents the consumer
 * from pairing one event's intensity with another event's sharpness. Low 16
 * bits hold intensity*1000+1 (zero means idle), high 16 hold sharpness*1000. */
static _Atomic uint32_t pending_event;
static void publish_event(float intensity, float sharpness) {
    uint32_t level = (uint32_t)(intensity * 1000.f) + 1u;
    uint32_t edge = (uint32_t)(sharpness * 1000.f);
    atomic_store_explicit(&pending_event, level | (edge << 16), memory_order_release);
}

static float low_state, envelope, baseline;
/* Starts past the gap so the first impact after launch is not swallowed. */
static uint32_t frames_since_event = HAPTICS_MIN_GAP_FRAMES;

void halo_haptics_set_enabled(int enabled) {
    atomic_store_explicit(&haptics_enabled, enabled ? 1 : 0, memory_order_relaxed);
}
int halo_haptics_enabled(void) {
    return atomic_load_explicit(&haptics_enabled, memory_order_relaxed);
}

void halo_haptics_observe(const float *out, uint32_t frames, uint32_t rate) {
    float strength = halo_settings_haptics_strength();
    if (!out || !frames || !rate || !halo_haptics_enabled() || strength <= 0.f) return;
    const float cutoff = 160.f;
    float alpha = 1.f - expf(-2.f * (float)M_PI * cutoff / (float)rate);
    if (!(alpha > 0.f) || alpha > 1.f) alpha = 0.2f;
    /* Attack ~5 ms, release ~250 ms, baseline ~1.5 s. */
    float attack = 1.f - expf(-1.f / (0.005f * (float)rate));
    float release = 1.f - expf(-1.f / (0.250f * (float)rate));
    float settle = 1.f - expf(-1.f / (1.500f * (float)rate));
    for (uint32_t i = 0; i < frames; ++i) {
        float mono = 0.5f * (out[2u*i] + out[2u*i + 1u]);
        low_state += alpha * (mono - low_state);
        float magnitude = fabsf(low_state);
        envelope += (magnitude > envelope ? attack : release) * (magnitude - envelope);
        baseline += settle * (envelope - baseline);
        if (frames_since_event < UINT32_MAX) frames_since_event++;
    }
    if (frames_since_event < HAPTICS_MIN_GAP_FRAMES) return;
    /* A transient is a level well clear of both the running average and the
     * noise floor, so quiet scenes and steady music do not buzz. */
    float floor_level = 0.02f, excess = envelope - (baseline * 1.9f + floor_level);
    if (excess <= 0.f) return;
    float intensity = excess * 3.2f * strength;
    if (intensity > 1.f) intensity = 1.f;
    if (intensity < 0.12f) return;
    /* Sharper for brief cracks, softer for sustained rumble. */
    float sharpness = 0.35f + 0.45f * (envelope > 0.f ? excess / envelope : 0.f);
    if (sharpness > 1.f) sharpness = 1.f;
    frames_since_event = 0;
    publish_event(intensity, sharpness);
}

static _Atomic uint64_t onset_count;
static _Atomic uint64_t fire_intent_ns, fire_window_ns;
void halo_haptics_note_fire(void) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t previous = atomic_exchange_explicit(&fire_intent_ns, now, memory_order_relaxed);
    /* The trigger is sampled once per engine frame. Below about seven frames a
     * second a fixed 150 ms window lapsed between samples while the trigger
     * was still held, and the player's own shots fell back to the world
     * threshold (11 pulses in the Build73 session's 4-20 fps stretch). Hold the
     * window for one and a half of the observed sample spacings, capped. */
    uint64_t window = 150000000ull;
    if (previous && now > previous && now - previous < 400000000ull) {
        uint64_t spaced = (now - previous) * 3u / 2u;
        if (spaced > window) window = spaced;
    }
    if (window > 500000000ull) window = 500000000ull;
    atomic_store_explicit(&fire_window_ns, window, memory_order_relaxed);
}
static int firing_now(void) {
    uint64_t at = atomic_load_explicit(&fire_intent_ns, memory_order_relaxed);
    uint64_t window = atomic_load_explicit(&fire_window_ns, memory_order_relaxed);
    return at && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - at < (window ? window : 150000000ull);
}
static int onset_enabled(void) {
    static int cached = -1;
    if (cached < 0) { const char *v = getenv("HALO_HAPTICS_ONSET"); cached = !(v && v[0] == '0'); }
    return cached;
}
uint64_t halo_haptics_onset_count(void) {
    return atomic_load_explicit(&onset_count, memory_order_relaxed);
}

void halo_haptics_voice_onset(float level, float floor, int at_listener) {
    float strength = halo_settings_haptics_strength();
    if (!halo_haptics_enabled() || strength <= 0.f || !onset_enabled()) return;
    if (!(level > 0.f) || !isfinite(level) || !isfinite(floor)) return;
    if (frames_since_event < HAPTICS_MIN_GAP_FRAMES) return;
    /* What reaches the ear, as a three-millisecond envelope of the rendered
     * level: a full-scale crack reads about 0.3 there (the envelope lags a
     * fast decay). A sound out in the world must be loud to count, so a
     * rifle across the beach, a tenth of that, does not buzz. The player's
     * own weapon is the voice on the listener, and Halo plays it twenty
     * decibels down: the assault rifle's own material reads 0.015 there on
     * the desktop trace, so that one is judged on its own scale. */
    float threshold = at_listener ? 0.006f : 0.18f;
    float full = at_listener ? 0.02f : 0.30f;
    if (firing_now()) {
        /* The player's own shot: the trigger is down, so a rise in any
         * positional voice is it. The headset gives the weapon's voice
         * neither the listener's position nor the Mac's level. */
        threshold = 0.008f; full = 0.05f;
    }
    if (level < threshold) return;
    float intensity = 0.35f + 0.65f * (level - threshold) / (full - threshold);
    intensity *= strength;
    if (intensity > 1.f) intensity = 1.f;
    if (intensity < 0.12f) return;
    /* How far the rise stands over the voice's own recent level tells a
     * crack from a thud: a shot out of silence is many times its floor. */
    float rise = floor > 1.0e-3f ? level / floor : 12.f;
    float edge = (rise - 2.f) / 8.f;
    if (edge < 0.f) edge = 0.f;
    if (edge > 1.f) edge = 1.f;
    float sharpness = 0.3f + 0.6f * edge;
    frames_since_event = 0;
    publish_event(intensity, sharpness);
    atomic_fetch_add_explicit(&onset_count, 1u, memory_order_relaxed);
}

int halo_haptics_take(float *intensity, float *sharpness) {
    uint32_t packed = atomic_exchange_explicit(&pending_event, 0u, memory_order_acquire);
    if (!packed) return 0;
    if (intensity) *intensity = (float)((packed & 0xffffu) - 1u) / 1000.f;
    if (sharpness) *sharpness = (float)(packed >> 16) / 1000.f;
    return 1;
}
