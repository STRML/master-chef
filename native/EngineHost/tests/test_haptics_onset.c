/* Haptics from voice onsets: a shot rising inside a positional voice at the
 * listener buzzes; the same shot far away, in a flat (2D) voice, a steady
 * sustained sound, or a slow swell does not.
 *
 * The mixed-signal detector cannot hear gunfire: on the headset it fired
 * five times on a cutscene explosion and never once through four minutes
 * of Silent Cartographer firefights, because a rifle's crack lives above
 * the 160 Hz band it keeps and under the music's low end. And Halo never
 * starts a sound with Play: it streams every sound through a fixed pool of
 * looping ring buffers (37 Play calls in a whole Maw run), so a new sound
 * is a rise inside a looping voice. This path watches each 3D voice's own
 * rendered level for that rise. The bursts here sit at 6 kHz on purpose,
 * where the one-pole low-pass is thirty-seven times down and the old
 * detector is deaf, so every event counted comes from the onset path.
 *
 * clang -O2 -I native/EngineHost native/EngineHost/tests/test_haptics_onset.c \
 *   native/EngineHost/directsound_mixer.c native/EngineHost/haptics.c \
 *   native/EngineHost/halo_settings.c -lm -o /tmp/halo-haptics-onset && /tmp/halo-haptics-onset
 */
#include "../directsound_mixer.h"
#include "../haptics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { SOURCE_RATE = 22050, OUTPUT_RATE = 48000, CHUNK = 512 };
static int failures;
static void check(int ok, const char *what) {
    printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/* Silence, then shots: each starts at full scale and is a tail within 25 ms. */
static void fill_shots(int16_t *pcm, uint32_t frames, const double *at, int count) {
    for (uint32_t i = 0; i < frames; ++i) {
        double t = (double)i / SOURCE_RATE, s = 0;
        for (int k = 0; k < count; ++k) {
            double dt = t - at[k];
            if (dt >= 0 && dt < 0.2) s += 0.95 * exp(-dt * 120.0) * sin(2.0 * M_PI * 6000.0 * dt);
        }
        if (s > 1) s = 1; if (s < -1) s = -1;
        pcm[i] = (int16_t)lrint(s * 32767.0);
    }
}
/* Shots every 70 ms over a lull that never drops below a third of the peak,
 * which is what a weapon's looping fire sound is. */
static void fill_loop(int16_t *pcm, uint32_t frames, const double *at, int count) {
    fill_shots(pcm, frames, at, count);
    for (uint32_t i = 0; i < frames; ++i) {
        double t = (double)i / SOURCE_RATE;
        double s = pcm[i] / 32767.0 + (t >= 0.3 ? 0.3 * sin(2.0 * M_PI * 6000.0 * t) : 0);
        if (s > 1) s = 1; if (s < -1) s = -1;
        pcm[i] = (int16_t)lrint(s * 32767.0);
    }
}
/* A steady tone at full scale: an engine, a beam, a hum. */
static void fill_steady(int16_t *pcm, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i] = (int16_t)lrint(0.9 * sin(2.0 * M_PI * 6000.0 * (double)i / SOURCE_RATE) * 32767.0);
}
/* Silence, then the tone swelling in over a full second. */
static void fill_swell(int16_t *pcm, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) {
        double t = (double)i / SOURCE_RATE - 0.3, ramp = t < 0 ? 0 : t < 1.0 ? t : 1.0;
        pcm[i] = (int16_t)lrint(0.9 * ramp * sin(2.0 * M_PI * 6000.0 * t) * 32767.0);
    }
}

static void voice_setup(DsMixerVoice *v, int16_t *pcm, uint32_t frames, int32_t volume, int has_3d) {
    memset(v, 0, sizeof *v);
    v->data = (uint8_t *)pcm; v->bytes = frames * 2u;
    v->format = (DsMixerFormat){ 1, 1, 16, 2, SOURCE_RATE, SOURCE_RATE * 2 };
    v->frequency = SOURCE_RATE; v->volume = volume; v->looping = 1; v->has_3d = (uint8_t)has_3d;
    /* At the listener, as Halo places the player's own weapon. */
    v->current_3d.min_distance = v->deferred_3d.min_distance = 1.f;
    v->current_3d.max_distance = v->deferred_3d.max_distance = 1000.f;
    v->current_3d.inside_angle = v->deferred_3d.inside_angle = 360;
    v->current_3d.outside_angle = v->deferred_3d.outside_angle = 360;
}

/* Render the voice(s) once through from the start and count events. */
static int render_and_count(DsMixer *m, DsMixerVoice **voices, int voice_count, double seconds,
                            float *first_intensity, float *first_sharpness) {
    int events = 0;
    for (int k = 0; k < voice_count; ++k) {
        voices[k]->cursor_frames = 0; voices[k]->playing = 1;
        ds_mixer_voice_restart(voices[k]);   /* what Play does */
    }
    float out[CHUNK * 2];
    for (uint32_t done = 0; done < (uint32_t)(OUTPUT_RATE * seconds); done += CHUNK) {
        ds_mixer_render(m, out, CHUNK, OUTPUT_RATE);
        float intensity, sharpness;
        if (halo_haptics_take(&intensity, &sharpness)) {
            if (!events && first_intensity) *first_intensity = intensity;
            if (!events && first_sharpness) *first_sharpness = sharpness;
            events++;
        }
    }
    return events;
}

int main(void) {
    uint32_t frames = SOURCE_RATE * 2;
    int16_t *shot = malloc(frames * 2u), *burst = malloc(frames * 2u);
    int16_t *steady = malloc(frames * 2u), *swell = malloc(frames * 2u), *loop = malloc(frames * 2u);
    double one[1] = { 0.3 };
    double rifle[12] = { 0.3, 0.37, 0.44, 0.51, 0.58, 0.65, 0.72, 0.79, 0.86, 0.93, 1.0, 1.07 };
    fill_shots(shot, frames, one, 1); fill_shots(burst, frames, rifle, 12);
    fill_steady(steady, frames); fill_swell(swell, frames); fill_loop(loop, frames, rifle, 12);
    DsMixer m; ds_mixer_init(&m);
    DsMixerVoice v, w; DsMixerVoice *a[1] = { &v }, *both[2] = { &v, &w };
    float intensity = 0, sharpness = 0;

    voice_setup(&v, shot, frames, 0, 1);
    if (!ds_mixer_add(&m, &v)) return 2;
    int fired = render_and_count(&m, a, 1, 2.0, &intensity, &sharpness);
    printf("  one shot in a 3D voice at the listener: %d event(s), intensity %.2f sharpness %.2f\n", fired, intensity, sharpness);
    check(fired == 1, "a shot rising inside a looping 3D voice at the listener fires once");
    check(intensity >= 0.9f, "and it is a hard pulse");
    check(sharpness >= 0.6f, "and a sharp one");
    ds_mixer_remove(&m, &v);

    /* Halo does start sounds with Play: 133 of them in the Build73 headset
     * session, each after writing the whole buffer and rewinding it. The
     * attack is then the voice's very first frames, and it must pulse. */
    { double start[1] = { 0.0 }; int16_t *at_start = malloc(frames * 2u);
      fill_shots(at_start, frames, start, 1);
      voice_setup(&v, at_start, frames, 0, 1);
      ds_mixer_add(&m, &v);
      fired = render_and_count(&m, a, 1, 1.0, &intensity, NULL);
      printf("  a shot in the first frames after Play: %d event(s), intensity %.2f\n", fired, intensity);
      check(fired == 1, "a sound's own attack right after Play pulses");
      ds_mixer_remove(&m, &v); free(at_start); }

    voice_setup(&v, shot, frames, -2500, 1);
    v.current_3d.position[0] = v.deferred_3d.position[0] = 20.f;
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  the same shot at -25 dB, twenty units off: %d event(s)\n", fired);
    check(fired == 0, "a distant rifle does not buzz");
    ds_mixer_remove(&m, &v);

    voice_setup(&v, shot, frames, 0, 0);
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  the shot in a flat (2D) voice: %d event(s)\n", fired);
    check(fired == 0, "dialogue and music are flat voices and never impacts");
    ds_mixer_remove(&m, &v);

    voice_setup(&v, steady, frames, 0, 1);
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  a steady full-scale 3D tone: %d event(s)\n", fired);
    check(fired <= 1, "a sustained sound pulses at most once, when it starts");
    ds_mixer_remove(&m, &v);

    voice_setup(&v, swell, frames, 0, 1);
    v.current_3d.position[0] = v.deferred_3d.position[0] = 2.f;
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  a one-second swell two units off: %d event(s)\n", fired);
    check(fired == 0, "a slow swell in the world does not buzz however loud it gets");
    ds_mixer_remove(&m, &v);

    /* On the listener every sound is the player's own doing, so a swell
     * there (a vehicle's engine as the player boards it) may pulse as it
     * starts, but only then. */
    voice_setup(&v, swell, frames, 0, 1);
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  the same swell on the listener: %d event(s)\n", fired);
    check(fired <= 1, "a swell on the listener pulses at most once, as it starts");
    ds_mixer_remove(&m, &v);

    voice_setup(&v, burst, frames, 0, 1);
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  twelve rifle shots 70 ms apart: %d event(s)\n", fired);
    check(fired >= 10, "automatic fire pulses on nearly every shot");
    ds_mixer_remove(&m, &v);

    voice_setup(&v, loop, frames, 0, 1);
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  twelve shots over a firing loop's lull: %d event(s)\n", fired);
    check(fired >= 10, "shots inside a looping fire sound each pulse");
    ds_mixer_remove(&m, &v);

    /* The player's own weapon: Halo places it on the listener and plays it
     * at -2000 mB, which the Silent Cartographer trace showed for the
     * assault rifle. */
    voice_setup(&v, burst, frames, -2000, 1);
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, &intensity, NULL);
    printf("  the rifle on the listener at Halo's -20 dB: %d event(s), intensity %.2f\n", fired, intensity);
    check(fired >= 10, "the player's own rifle pulses on every shot");
    check(intensity >= 0.5f, "and firmly");
    ds_mixer_remove(&m, &v);

    /* The rifle as the desktop trace actually measured it: at -2000 mB its
     * fast envelope peaked at 0.0148, half what the full-scale burst above
     * reads at that volume. The listener scale must catch that. */
    { int16_t *quiet = malloc(frames * 2u);
      for (uint32_t i = 0; i < frames; ++i) quiet[i] = (int16_t)(burst[i] / 2);
      voice_setup(&v, quiet, frames, -2000, 1);
      ds_mixer_add(&m, &v);
      fired = render_and_count(&m, a, 1, 2.0, &intensity, NULL);
      printf("  the rifle at the level the trace measured (0.015): %d event(s), intensity %.2f\n", fired, intensity);
      check(fired >= 10, "the traced rifle level pulses on every shot");
      check(intensity >= 0.5f, "and firmly");
      ds_mixer_remove(&m, &v); free(quiet); }

    /* On the headset the player's own weapon is not on the listener and not
     * loud: the same distant, quiet rifle pulses while the trigger is held. */
    voice_setup(&v, burst, frames, -2000, 1);
    v.current_3d.position[0] = v.deferred_3d.position[0] = 2.f;
    ds_mixer_add(&m, &v);
    halo_haptics_note_fire();
    fired = render_and_count(&m, a, 1, 0.5, &intensity, NULL);
    printf("  a quiet rifle two units off while the trigger is held: %d event(s), intensity %.2f\n", fired, intensity);
    check(fired >= 1, "the player's own shots pulse while the trigger is held");
    ds_mixer_remove(&m, &v);

    /* The same -20 dB rifle a few metres off is another soldier's, once the
     * trigger has been up for longer than the firing window. */
    usleep(200000);
    voice_setup(&v, burst, frames, -2000, 1);
    v.current_3d.position[0] = v.deferred_3d.position[0] = 3.f;
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 2.0, NULL, NULL);
    printf("  the same rifle three metres away: %d event(s)\n", fired);
    check(fired == 0, "a marine's rifle beside the player does not buzz");
    ds_mixer_remove(&m, &v);

    voice_setup(&v, shot, frames, 0, 1); voice_setup(&w, shot, frames, 0, 1);
    ds_mixer_add(&m, &v); ds_mixer_add(&m, &w);
    fired = render_and_count(&m, both, 2, 2.0, NULL, NULL);
    printf("  the same shot in two voices at once: %d event(s)\n", fired);
    check(fired == 1, "two shots in one instant are one pulse");
    ds_mixer_remove(&m, &v); ds_mixer_remove(&m, &w);

    /* The trigger is sampled once per engine frame. At five frames a second
     * the samples are 200 ms apart, and a shot 180 ms after the last sample is
     * still the player's own. */
    halo_haptics_note_fire(); usleep(200000); halo_haptics_note_fire(); usleep(180000);
    voice_setup(&v, burst, frames, -2000, 1);
    v.current_3d.position[0] = v.deferred_3d.position[0] = 2.f;
    ds_mixer_add(&m, &v);
    fired = render_and_count(&m, a, 1, 0.5, NULL, NULL);
    printf("  a quiet rifle 180 ms after a 5 fps trigger sample: %d event(s)\n", fired);
    check(fired >= 1, "the trigger window spans the engine's frame spacing");
    ds_mixer_remove(&m, &v);

    check(halo_haptics_onset_count() >= 44, "the onset counter saw every pulse");
    free(shot); free(burst); free(steady); free(swell); free(loop);
    printf("%s (%d failing)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
