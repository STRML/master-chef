#include "halo_settings.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>

/* Floats are carried as atomic 32-bit words so no lock is needed on the
 * render path; each field is independent and a torn pair is harmless because
 * every value is read through its own clamped accessor. */
static _Atomic uint32_t field_separation, field_vfov, field_haptics, field_backdrop, field_target_fps, field_self_gain_db;
static _Atomic uint32_t field_head_yaw, field_head_pitch, field_head_roll;
static _Atomic int field_spatial_shell, field_gaze_pointer, field_layer_align, field_frame_pacing;
static _Atomic int initialised;

/* This experimental render switch is immutable for the process. Keep its
 * publication independent of the live settings so simultaneous diagnostic
 * and renderer reads cannot see different values during initialization. */
static pthread_once_t radial_fog_once = PTHREAD_ONCE_INIT;
static int radial_fog_enabled;

static void initialize_radial_fog(void) {
    const char *value = getenv("HALO_RADIAL_FOG");
    radial_fog_enabled = value && strcmp(value, "1") == 0;
}

int halo_settings_radial_fog(void) {
    pthread_once(&radial_fog_once, initialize_radial_fog);
    return radial_fog_enabled;
}

static uint32_t pack(float v) { uint32_t w; memcpy(&w, &v, 4); return w; }
static float unpack(uint32_t w) { float v; memcpy(&v, &w, 4); return v; }

static double env_number(const char *name, double fallback, double low, double high) {
    const char *text = getenv(name);
    if (!text || !text[0]) return fallback;
    double value = atof(text);
    return (value >= low && value <= high) ? value : fallback;
}

static void ensure(void) {
    if (atomic_load_explicit(&initialised, memory_order_acquire)) return;
    /* Defaults match the documented environment behaviour. */
    const char *stereo = getenv("HALO_STEREO");
    double millimetres = env_number("HALO_STEREO_IPD", 63.0, 40.0, 90.0);
    double separation = millimetres / 1000.0 / 2.0 / 3.048;
    separation = env_number("HALO_STEREO_SEPARATION", separation, 0.0, 1.0);
    if (stereo && stereo[0] == '0') separation = 0.0;
    atomic_store_explicit(&field_separation, pack((float)separation), memory_order_relaxed);
    /* Rectilinear projection spreads a wide vertical field thinly: at 150
     * degrees the centre of view gets under 3 pixels per degree, which reads
     * as a soft, low-resolution image. 105 degrees nearly triples that for
     * the same pixels, and the cap beyond it is faded rather than shown. */
    double degrees = env_number("HALO_PANORAMA_VFOV", 105.0, 60.0, 175.0);
    atomic_store_explicit(&field_vfov, pack((float)(degrees * 3.14159265358979 / 180.0)), memory_order_relaxed);
    const char *haptics = getenv("HALO_HAPTICS");
    atomic_store_explicit(&field_haptics, pack(haptics && haptics[0] == '0' ? 0.f : 1.f), memory_order_relaxed);
    atomic_store_explicit(&field_backdrop, pack((float)env_number("HALO_BACKDROP", 1.0, 0.0, 2.0)), memory_order_relaxed);
    const char *shell = getenv("HALO_SPATIAL_MENU");
    atomic_store_explicit(&field_spatial_shell, shell && shell[0] == '0' ? 0 : 1, memory_order_relaxed);
    atomic_store_explicit(&field_target_fps, pack((float)env_number("HALO_PANORAMA_TARGET_FPS", 30.0, 10.0, 60.0)), memory_order_relaxed);
    atomic_store_explicit(&field_self_gain_db, pack((float)env_number("HALO_SELF_GAIN_DB", 18.0, 0.0, 24.0)), memory_order_relaxed);
    const char *gaze = getenv("HALO_GAZE_POINTER");
    /* Head-follow for the menu cursor, off unless HALO_GAZE_POINTER=1: the
     * eyes choose (the system's gaze ray at the pinch, its hover glow between). */
    atomic_store_explicit(&field_gaze_pointer, gaze && gaze[0] == '1' ? 1 : 0, memory_order_relaxed);
    /* Layer alignment, off unless HALO_LAYER_ALIGN=1 (or the settings
     * toggle). It has only been checked on synthetic pictures. While the
     * stick turns it also bends the side bands at eye level (the weapon
     * zone's blend) and sharpens the trailing join's cross-fade. It stays off
     * until a b30 stick-turn A/B on the headset shows the joins improve
     * without those side effects. */
    const char *align = getenv("HALO_LAYER_ALIGN");
    atomic_store_explicit(&field_layer_align, align && align[0] == '1' ? 1 : 0, memory_order_relaxed);
    /* Aggregate headset traces cannot establish a clear per-frame win.
     * Keep experimental pacing opt-in, including after malformed input. */
    const char *paced = getenv("HALO_FRAME_PACING");
    int pacing = paced && (!strcmp(paced, "1") || !strcmp(paced, "even") || !strcmp(paced, "on"));
    atomic_store_explicit(&field_frame_pacing, pacing, memory_order_relaxed);
    atomic_store_explicit(&initialised, 1, memory_order_release);
}

void halo_settings_get(HaloSettings *out) {
    if (!out) return;
    ensure();
    out->stereo_separation = unpack(atomic_load_explicit(&field_separation, memory_order_relaxed));
    out->panorama_vfov = unpack(atomic_load_explicit(&field_vfov, memory_order_relaxed));
    out->haptics_strength = unpack(atomic_load_explicit(&field_haptics, memory_order_relaxed));
    out->backdrop_brightness = unpack(atomic_load_explicit(&field_backdrop, memory_order_relaxed));
    out->spatial_shell = atomic_load_explicit(&field_spatial_shell, memory_order_relaxed);
    out->panorama_target_fps = unpack(atomic_load_explicit(&field_target_fps, memory_order_relaxed));
    out->self_gain_db = unpack(atomic_load_explicit(&field_self_gain_db, memory_order_relaxed));
    out->gaze_pointer = atomic_load_explicit(&field_gaze_pointer, memory_order_relaxed);
    out->layer_align = atomic_load_explicit(&field_layer_align, memory_order_relaxed);
    out->frame_pacing = atomic_load_explicit(&field_frame_pacing, memory_order_relaxed);
}

void halo_settings_set(const HaloSettings *in) {
    if (!in) return;
    ensure();
    float separation = in->stereo_separation;
    if (!(separation >= 0.f) || separation > 1.f) separation = 0.f;
    float vfov = in->panorama_vfov;
    if (!(vfov >= 1.0472f) || vfov > 3.0543f) vfov = 2.6179939f;   /* 60..175 degrees */
    float haptics = in->haptics_strength;
    if (!(haptics >= 0.f) || haptics > 4.f) haptics = 1.f;
    float backdrop = in->backdrop_brightness;
    if (!(backdrop >= 0.f) || backdrop > 2.f) backdrop = 1.f;
    float target = in->panorama_target_fps;
    if (!(target >= 10.f) || target > 60.f) target = 27.f;
    atomic_store_explicit(&field_target_fps, pack(target), memory_order_relaxed);
    float self_db = in->self_gain_db;
    if (!(self_db >= 0.f) || self_db > 24.f) self_db = 18.f;
    atomic_store_explicit(&field_self_gain_db, pack(self_db), memory_order_relaxed);
    atomic_store_explicit(&field_separation, pack(separation), memory_order_relaxed);
    atomic_store_explicit(&field_vfov, pack(vfov), memory_order_relaxed);
    atomic_store_explicit(&field_haptics, pack(haptics), memory_order_relaxed);
    atomic_store_explicit(&field_backdrop, pack(backdrop), memory_order_relaxed);
    atomic_store_explicit(&field_spatial_shell, in->spatial_shell ? 1 : 0, memory_order_relaxed);
    atomic_store_explicit(&field_gaze_pointer, in->gaze_pointer ? 1 : 0, memory_order_relaxed);
    atomic_store_explicit(&field_layer_align, in->layer_align ? 1 : 0, memory_order_relaxed);
    atomic_store_explicit(&field_frame_pacing, in->frame_pacing == 1, memory_order_relaxed);
}

float halo_settings_stereo_separation(void) { ensure(); float v = unpack(atomic_load_explicit(&field_separation, memory_order_relaxed)); return (v >= 0.f && v <= 1.f) ? v : 0.f; }
float halo_settings_panorama_vfov(void) { ensure(); float v = unpack(atomic_load_explicit(&field_vfov, memory_order_relaxed)); return (v >= 1.0472f && v <= 3.0543f) ? v : 2.6179939f; }
float halo_settings_haptics_strength(void) { ensure(); float v = unpack(atomic_load_explicit(&field_haptics, memory_order_relaxed)); return (v >= 0.f && v <= 4.f) ? v : 1.f; }
float halo_settings_backdrop_brightness(void) { ensure(); float v = unpack(atomic_load_explicit(&field_backdrop, memory_order_relaxed)); return (v >= 0.f && v <= 2.f) ? v : 1.f; }
int halo_settings_spatial_shell(void) { ensure(); return atomic_load_explicit(&field_spatial_shell, memory_order_relaxed); }
int halo_settings_gaze_pointer(void) { ensure(); return atomic_load_explicit(&field_gaze_pointer, memory_order_relaxed); }
int halo_settings_layer_align(void) { ensure(); return atomic_load_explicit(&field_layer_align, memory_order_relaxed); }
int halo_settings_frame_pacing(void) { ensure(); return atomic_load_explicit(&field_frame_pacing, memory_order_relaxed); }
float halo_settings_panorama_target_fps(void) { ensure(); float v = unpack(atomic_load_explicit(&field_target_fps, memory_order_relaxed)); return (v >= 10.f && v <= 60.f) ? v : 27.f; }
float halo_settings_self_gain(void) { ensure(); float db = unpack(atomic_load_explicit(&field_self_gain_db, memory_order_relaxed)); if (!(db >= 0.f && db <= 24.f)) db = 18.f; return powf(10.f, db / 20.f); }

/* The pose is not a setting, so it is neither seeded from the environment nor
 * carried in HaloSettings; it starts at zero and is replaced every frame. */
void halo_settings_set_head(float yaw, float pitch) {
    if (!isfinite(yaw)) yaw = 0.f;
    if (!isfinite(pitch)) pitch = 0.f;
    atomic_store_explicit(&field_head_yaw, pack(yaw), memory_order_relaxed);
    atomic_store_explicit(&field_head_pitch, pack(pitch), memory_order_relaxed);
}
float halo_settings_head_yaw(void) { float v = unpack(atomic_load_explicit(&field_head_yaw, memory_order_relaxed)); return isfinite(v) ? v : 0.f; }
float halo_settings_head_pitch(void) { float v = unpack(atomic_load_explicit(&field_head_pitch, memory_order_relaxed)); return isfinite(v) ? v : 0.f; }
void halo_settings_set_head_roll(float roll) {
    if (!isfinite(roll)) return;
    if (roll > 1.5f) roll = 1.5f;
    if (roll < -1.5f) roll = -1.5f;
    atomic_store_explicit(&field_head_roll, pack(roll), memory_order_relaxed);
}
float halo_settings_head_roll(void) { float v = unpack(atomic_load_explicit(&field_head_roll, memory_order_relaxed)); return isfinite(v) && v >= -1.5f && v <= 1.5f ? v : 0.f; }
