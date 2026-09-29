/* Signal-quality checks for the mixer, alongside the ABI checks in
 * test_directsound_mixer.c: resampling cleanliness, limiter behaviour,
 * head-relative panning, and the listener distance factor.
 *
 * clang -O2 -I native/EngineHost native/EngineHost/tests/test_mixer_quality.c \
 *   native/EngineHost/directsound_mixer.c native/EngineHost/halo_settings.c \
 *   -lm -o /tmp/halo-mixer-quality
 */

#include "directsound_mixer.h"
#include "halo_settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void halo_haptics_observe(const float *a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void halo_haptics_voice_onset(float a, float b, int c) { (void)a; (void)b; (void)c; }

static int failures;
static void check(int ok, const char *what) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/* Total power outside the fundamental bin, as a fraction of the fundamental.
 * A clean resampling of a pure tone leaves almost nothing there. */
static double distortion(const float *stereo, uint32_t frames, double rate, double tone) {
    double fr = 0, fi = 0, total = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        double s = stereo[2u*i], w = 2.0*M_PI*tone*i/rate;
        fr += s*cos(w); fi += s*sin(w); total += s*s;
    }
    double fundamental = 2.0*(fr*fr + fi*fi)/(double)frames/(double)frames*(double)frames;
    fundamental = 2.0*(fr*fr + fi*fi)/(double)frames;
    return total > 1e-12 ? (total - fundamental)/total : 0.0;
}

int main(void) {
    /* 1. A 1 kHz tone stored at 22,050 Hz, played out at 48,000. */
    enum { SRC = 22050, OUT = 48000, SRC_FRAMES = SRC, OUT_FRAMES = 8192 };
    static int16_t pcm[SRC_FRAMES];
    for (int i = 0; i < SRC_FRAMES; ++i)
        pcm[i] = (int16_t)lrint(24000.0*sin(2.0*M_PI*1000.0*i/SRC));
    DsMixer m; ds_mixer_init(&m);
    DsMixerVoice v; memset(&v, 0, sizeof v);
    v.data = (uint8_t*)pcm; v.bytes = sizeof pcm;
    v.format.tag = 1; v.format.channels = 1; v.format.bits = 16;
    v.format.block_align = 2; v.format.sample_rate = SRC;
    v.playing = 1; v.looping = 1; v.volume = 0; v.pan = 0;
    ds_mixer_add(&m, &v);
    static float out[OUT_FRAMES*2];
    ds_mixer_render(&m, out, OUT_FRAMES, OUT);
    double thd = distortion(out, OUT_FRAMES, OUT, 1000.0);
    printf("  resampled 1 kHz non-fundamental power: %.5f%%\n", thd*100.0);
    check(thd < 0.002, "22 kHz -> 48 kHz tone stays clean");

    /* 2. The limiter is transparent below the knee and bounded above it. */
    ds_mixer_remove(&m, &v);
    static int16_t quiet[256], loud[256];
    for (int i = 0; i < 256; ++i) {
        quiet[i] = (int16_t)lrint(9000.0*sin(2.0*M_PI*i/64.0));   /* ~0.27 peak */
        loud[i]  = (int16_t)lrint(32000.0*sin(2.0*M_PI*i/64.0));
    }
    DsMixerVoice q = v; q.data = (uint8_t*)quiet; q.bytes = sizeof quiet; q.cursor_frames = 0;
    ds_mixer_add(&m, &q);
    static float soft[256*2]; ds_mixer_render(&m, soft, 200, OUT);
    double worst = 0;
    for (int i = 0; i < 200; ++i) { double a = fabs(soft[2*i]); if (a > worst) worst = a; }
    check(worst > 0.2 && worst < 0.30, "quiet material passes the limiter untouched");
    ds_mixer_remove(&m, &q);

    /* Eight loud voices at once would sum to about eight times full scale. */
    static DsMixerVoice many[8];
    for (int i = 0; i < 8; ++i) {
        many[i] = v; many[i].data = (uint8_t*)loud; many[i].bytes = sizeof loud;
        many[i].cursor_frames = 0; ds_mixer_add(&m, &many[i]);
    }
    static float hot[256*2]; ds_mixer_render(&m, hot, 200, OUT);
    double peak = 0; int flat = 0;
    for (int i = 0; i < 400; ++i) {
        double a = fabs(hot[i]); if (a > peak) peak = a;
        if (a > 0.9999) flat++;
    }
    printf("  eight full-scale voices peak at %.4f, %d samples pinned\n", peak, flat);
    check(peak <= 1.0f, "a hot mix never leaves the output range");
    check(flat == 0, "a hot mix is compressed, not squared off");
    for (int i = 0; i < 8; ++i) ds_mixer_remove(&m, &many[i]);

    /* 3. Head rotation moves a world sound across the ears. */
    DsMixerVoice s = v; s.data = (uint8_t*)loud; s.bytes = sizeof loud; s.cursor_frames = 0;
    s.has_3d = 1; s.current_3d.mode = 0; s.current_3d.min_distance = 1.f; s.current_3d.max_distance = 100.f;
    s.current_3d.position[2] = 5.f;   /* five units straight ahead */
    m.current_listener.front[2] = 1.f; m.current_listener.up[1] = 1.f;
    float l, r;
    halo_settings_set_head(0.f, 0.f);
    ds_mixer_voice_gains(&m, &s, &l, &r);
    check(fabsf(l-r) < 1e-4f, "looking ahead keeps a sound ahead centred");
    halo_settings_set_head((float)(M_PI/2.0), 0.f);   /* turn right */
    ds_mixer_voice_gains(&m, &s, &l, &r);
    printf("  head turned 90 degrees right: left=%.3f right=%.3f\n", l, r);
    check(l > r*1.5f, "turning right moves that sound to the left ear");
    halo_settings_set_head((float)(-M_PI/2.0), 0.f);  /* turn left */
    ds_mixer_voice_gains(&m, &s, &l, &r);
    check(r > l*1.5f, "turning left moves that sound to the right ear");
    halo_settings_set_head(0.f, 0.f);

    /* 4. The listener's distance factor must not touch the rolloff. Halo sets
     * it to 3.048, ten feet in metres, and a sound at its minimum distance
     * has to stay at full volume whatever unit the world is measured in. */
    DsMixerVoice d = v; d.data = (uint8_t*)loud; d.bytes = sizeof loud; d.cursor_frames = 0;
    d.has_3d = 1; d.current_3d.mode = 0; d.current_3d.min_distance = 2.f;
    d.current_3d.max_distance = 3.4e38f; d.current_3d.position[2] = 2.f;  /* exactly at minimum */
    m.current_listener.distance_factor = 1.f;
    float unit_l, unit_r; ds_mixer_voice_gains(&m, &d, &unit_l, &unit_r);
    m.current_listener.distance_factor = 3.048f;
    float scaled_l, scaled_r; ds_mixer_voice_gains(&m, &d, &scaled_l, &scaled_r);
    printf("  at the minimum distance: factor 1 gives %.3f, factor 3.048 gives %.3f\n",
           unit_l + unit_r, scaled_l + scaled_r);
    check(fabsf(unit_l - scaled_l) < 1e-5f && fabsf(unit_r - scaled_r) < 1e-5f,
          "the distance factor does not change the rolloff");
    check(unit_l + unit_r > 1.9f, "a sound at its minimum distance plays at full volume");
    /* Further away it must still attenuate by the plain ratio. */
    d.current_3d.position[2] = 8.f;
    float far_l, far_r; ds_mixer_voice_gains(&m, &d, &far_l, &far_r);
    printf("  four times the minimum distance: %.4f of full\n", (far_l + far_r) / 2.f);
    check(fabsf((far_l + far_r)/2.f - 0.25f) < 0.01f, "rolloff is still the plain distance ratio");
    m.current_listener.distance_factor = 1.f;

    printf("%s (%d failing)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
