#ifndef HALO_HAPTICS_H
#define HALO_HAPTICS_H
#include <stdint.h>
/* Halo PC 1.10 contains no force-feedback code at all (no DirectInput effect
 * API is imported or referenced), so there is no rumble intent in the game to
 * forward to a controller. Haptics are instead synthesised from the mixed
 * audio the engine already produces: a sharp rise in low-frequency energy is
 * what gunfire, explosions, impacts and heavy footfalls have in common, and
 * that is what a player expects to feel. The mixer feeds this detector; the
 * platform layer drains one event per frame and plays it on the controller. */

/* Called by the mixer with the frames it just produced. Audio thread. */
void halo_haptics_observe(const float *interleaved_stereo, uint32_t frames, uint32_t rate);
/* A positional voice's rendered level just rose sharply: level is its fast
 * envelope after the mixer's gain, which carries Halo's own distance
 * attenuation, and floor the trough it rose from. at_listener says the
 * voice sits on the listener, which is where Halo puts the player's own
 * weapon, twenty decibels down; that one is judged on its own scale. The
 * mixed signal above cannot hear a rifle under music, and gunfire lives
 * above the band the low-pass keeps. HALO_HAPTICS_ONSET=0 disables this
 * path. Audio thread. */
void halo_haptics_voice_onset(float level, float floor, int at_listener);
/* Onsets that fired so far, for the device report. */
uint64_t halo_haptics_onset_count(void);
/* The player is firing (trigger held) right now. Engine thread, per frame.
 * For the next 150 ms any positional onset is the player's own shot, at
 * whatever level and distance the engine happens to give it: on the headset
 * the weapon's voice is never on the listener the way it is on the Mac. */
void halo_haptics_note_fire(void);
/* Take the pending event, if any. Returns 0 when there is nothing to play.
 * intensity and sharpness are 0..1 as Core Haptics defines them. */
int halo_haptics_take(float *intensity, float *sharpness);
/* 0 disables synthesis (HALO_HAPTICS=0). */
void halo_haptics_set_enabled(int enabled);
int halo_haptics_enabled(void);
#endif
