#ifndef HALO_SETTINGS_H
#define HALO_SETTINGS_H
/* Live settings for the port's own presentation features, shared between the
 * engine thread that renders and the UI that adjusts them. Every field starts
 * from its environment variable so existing scripted runs behave as before,
 * and every field may then be changed while the game runs. Values are read
 * once per frame at well-defined points, never mid-pass, so a change can not
 * tear a frame between the two eyes. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Half the eye separation in Halo world units (its unit is ten feet).
     * Zero renders mono, which halves the world passes. */
    float stereo_separation;
    /* Vertical field of view of each panorama view, in radians. The
     * horizontal field comes from the projection rectangle, so this only
     * trades vertical resolution for vertical coverage. */
    float panorama_vfov;
    /* Scales synthesised controller haptics. Zero disables them. */
    float haptics_strength;
    /* Brightness of the surround that continues the picture past its edges. */
    float backdrop_brightness;
    /* Render the front-end menu through the panorama path instead of as a
     * flat panel, so its 3D backdrop surrounds the viewer. */
    int spatial_shell;
    /* The frame rate the bearing budget keeps: it draws more of the sphere
     * each frame while the engine's busy time stays under this, and less as
     * soon as it runs over. Halo's own cap is 30. */
    float panorama_target_fps;
    /* Decibels added to the sounds Halo places on the listener, which are
     * the player's own weapon, reload and melee. Halo hands those to
     * DirectSound twenty decibels down (the assault rifle at -2000 mB), and
     * in a headset firing that cannot be heard over the music is a fault. */
    float self_gain_db;
    /* With a menu up, the engine's cursor follows the gaze and a pinch is a
     * click (pointer.c). Off, menus take the pad and the mouse only. */
    int gaze_pointer;
    /* Turn each panorama layer drawn in an earlier engine frame by the
     * rotation from the camera it was drawn with to the newest centre
     * camera, so the joins meet while the player turns (EngineLayerAlignment
     * in the presenter). Presentation only; the engine draws the same.
     * Off unless HALO_LAYER_ALIGN=1 until a headset A/B supports it. */
    int layer_align;
    /* Optional frame pacing (0 off, 1 on). HALO_FRAME_PACING seeds this
     * setting; the controller uses 45/30/22.5/18/15 fps on a 90 Hz grid. */
    int frame_pacing;
} HaloSettings;

/* Where the viewer is looking inside the panorama, in radians, measured from
 * the recentred forward direction. The display thread owns this; the audio
 * mixer reads it so a sound stays with its object when only the head turns
 * and the game camera does not. Zero means looking straight ahead. */
void halo_settings_set_head(float yaw, float pitch);
float halo_settings_head_yaw(void);
float halo_settings_head_pitch(void);
/* How far the head is tilted about the line of sight, in radians, positive
 * when the right eye is the higher one. The two centre passes place their
 * cameras along this tilted baseline, so a near object such as the weapon
 * keeps corresponding between the eyes when the head rolls. */
void halo_settings_set_head_roll(float roll);
float halo_settings_head_roll(void);

/* Both are safe from any thread. get() never blocks. */
void halo_settings_get(HaloSettings *out);
void halo_settings_set(const HaloSettings *in);
/* Clamped accessors the renderer uses directly. */
float halo_settings_stereo_separation(void);
float halo_settings_panorama_vfov(void);
float halo_settings_haptics_strength(void);
float halo_settings_backdrop_brightness(void);
int halo_settings_spatial_shell(void);
float halo_settings_panorama_target_fps(void);
/* As a linear gain, for the mixer. */
float halo_settings_self_gain(void);
int halo_settings_gaze_pointer(void);
int halo_settings_layer_align(void);
/* Experimental bearing-invariant fog. Defaults off; only HALO_RADIAL_FOG=1
 * enables it. Cached once for the process, so all bearings and diagnostics
 * use the same effective setting even if the environment later changes. */
int halo_settings_radial_fog(void);
int halo_settings_frame_pacing(void);

#ifdef __cplusplus
}
#endif
#endif
