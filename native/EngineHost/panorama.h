#ifndef HALO_PANORAMA_H
#define HALO_PANORAMA_H
#include <stdint.h>
#include <stddef.h>
#define HALO_PANORAMA_PRESENT_BOUNDARY 1
enum { HALO_PANORAMA_FLAT=0, HALO_PANORAMA_COMPLETE=1, HALO_PANORAMA_WORLD_INCOMPLETE=2 };
enum { HALO_PANORAMA_OK=0, HALO_PANORAMA_MISSING_PASS=1, HALO_PANORAMA_NO_PROJECTION=2,
       HALO_PANORAMA_READBACK_FAILED=3, HALO_PANORAMA_ALLOCATION_FAILED=4,
       HALO_PANORAMA_INVALID_VIEWPORT=5, HALO_PANORAMA_SIZE_MISMATCH=6,
       HALO_PANORAMA_RENDER_ABORTED=7 };
/* Original-engine camera views plus a HUD. Rotating schedules retain some
 * bearings from earlier simulation frames, recorded by layer_epoch. All
 * access is on the engine thread; the visionOS bridge publishes a copy. */
/* Ten layers covering the whole sphere.
 *
 * Six yawed bands sixty degrees apart close the ring all the way round, and
 * two more views pitched straight up and straight down close the sky and the
 * floor, which no ring of rectilinear views can reach however wide its field.
 * Layer 3 is the flat HUD and layer 4 is the centre bearing seen by the right
 * eye, so the ten layers are eight pictures.
 *
 * Only the centre bearing is drawn twice, once per eye. Everything else is
 * drawn once and shown to both, because the engine, not the GPU, is what
 * limits this port and depth is worth paying for only where the viewer is
 * looking. That is nine passes for a complete sphere. */
enum { HALO_PANORAMA_LAYERS = 10, HALO_PANORAMA_HUD_LAYER = 3,
       /* Bearings, in degrees of yaw from straight ahead:
        *   0 is -60, 1 is 0 (left eye), 2 is +60, 4 is 0 (right eye),
        *   7 is +120, 8 is 180, 9 is -120.
        * 5 is straight up, 6 straight down, 3 the HUD. */
       HALO_PANORAMA_CENTRE_LEFT = 1, HALO_PANORAMA_CENTRE_RIGHT = 4,
       HALO_PANORAMA_UP = 5, HALO_PANORAMA_DOWN = 6,
       HALO_PANORAMA_REAR_RIGHT = 7, HALO_PANORAMA_REAR = 8, HALO_PANORAMA_REAR_LEFT = 9 };
/* Every layer that has to hold a world view for the frame to be a sphere, so
 * the validity mask and the presenter agree on what a complete frame is.
 * Layer 3 is deliberately absent: the HUD is redrawn every frame rather than
 * scheduled as a pass, and Halo hides it through cutscenes, so requiring it
 * would reject frames that are perfectly good. */
#define HALO_PANORAMA_MONO_MASK   0x3E7u   /* 0,1,2,5,6,7,8,9 */
#define HALO_PANORAMA_STEREO_MASK 0x3F7u   /* the same plus 4 */
/* The bearing a layer shows, ignoring which eye it was drawn for. */
#define HALO_PANORAMA_SECTOR_OF(layer) \
    ((layer) == HALO_PANORAMA_CENTRE_RIGHT ? 1 : (layer))
/* Position, forward and up: nine floats per layer camera. */
#define HALO_PANORAMA_POSE_FLOATS 9
typedef struct {
    uint32_t width, height, valid;
    uint32_t status, failure_reason;
    /* 1 when the right-eye layers were produced for this frame. */
    uint32_t stereo;
    uint64_t source_epoch, scene_epoch;
    /* Actual image epoch per layer, including images retained from older frames. */
    uint64_t layer_epoch[HALO_PANORAMA_LAYERS];
    /* Indexed by layer, not by bearing: the zenith and nadir views carry
     * their own projection and sit at layers 5 and 6. */
    float projection_x[HALO_PANORAMA_LAYERS], projection_y[HALO_PANORAMA_LAYERS];
    float viewport_u_min[HALO_PANORAMA_LAYERS], viewport_v_min[HALO_PANORAMA_LAYERS], viewport_u_max[HALO_PANORAMA_LAYERS], viewport_v_max[HALO_PANORAMA_LAYERS];
    /* The engine camera each layer was drawn with, carried with the layer
     * exactly like its epoch: position (world units), forward, up, as the
     * renderer record holds them (+0x00, +0x0C, +0x18) before the pass yaws
     * or pitches them to the layer's bearing. The bearing itself is fixed per
     * layer (panorama_hooks.inc), so this plus the layer index is the whole
     * camera. All zero when unknown. The presenter turns each held layer by
     * the rotation from this camera to layer 1's, so pictures drawn a few
     * frames apart meet at the joins while the player turns. */
    float layer_pose[HALO_PANORAMA_LAYERS][HALO_PANORAMA_POSE_FLOATS];
    /* The source epoch of the first frame after a camera cut (a jump the
     * stick cannot make in one frame) or a scene change. Layers older than it
     * show another shot and must not be turned to line up with this one. */
    uint64_t cut_epoch;
} HaloPanoramaInfo;
/* True once every layer holds a picture. Until then nothing may be skipped,
 * or the first frames would never assemble a complete sphere. */
int host_panorama_all_layers_ready(void);
/* The bearing budget (panorama_budget.h) as it stands: extra passes per two
 * frames, and the smoothed busy time per frame it steers on. For the reports. */
unsigned host_panorama_budget_extra(void);
/* The budget's heavy tier, 0 to 3 (panorama_budget.h). */
unsigned host_panorama_budget_tier(void);
float host_panorama_busy_seconds(void);
/* The engine camera this frame's passes start from (the renderer record's
 * position, forward and up before any bearing is applied). Called once per
 * frame by the panorama hook, before the first pass; every layer the frame
 * draws records it. Informational: nothing here changes what is drawn. */
int host_panorama_camera_moving(void);
void host_panorama_set_camera(const float pose[HALO_PANORAMA_POSE_FLOATS]);
void host_panorama_begin(int pass);
void host_panorama_end(int pass);
void host_panorama_reset(void);
/* A scene/target discontinuity also invalidates bearings held from prior frames. */
void host_panorama_invalidate(void);
/* Whether this frame produces right-eye layers. Held across the per-frame
 * reset, so the validity mask knows how many layers to expect. */
void host_panorama_set_stereo(int on);
/* A world render escaped; do not misclassify its center buffer as a menu. */
void host_panorama_abort(void);
/* Called once after the platform bridge has consumed this Present. */
void host_panorama_present_complete(void);
void host_panorama_ui(int active);
/* Diagnostic marker for actual004924B0 duration; independent of target routing. */
void host_panorama_viewmodel(int active);
void host_panorama_projection(float m11,float m22,int viewport_x,int viewport_y,int viewport_w,int viewport_h,uint32_t caller);
int host_panorama_frame(HaloPanoramaInfo *info, const void *layers[HALO_PANORAMA_LAYERS]);
/* Zero-copy sink (visionOS): the platform bridge lends HALO_PANORAMA_LAYERS BGRA8 Metal
 * textures per frame and the engine blits each view into them instead of
 * reading the frame back through the CPU. With a sink set, the CPU layer
 * copies are never made. */
typedef struct {
    /* Reserve a slot with HALO_PANORAMA_LAYERS width x height targets; returns 0 when the pool is busy. */
    int (*acquire)(uint32_t width, uint32_t height, void *textures[HALO_PANORAMA_LAYERS], int *slot);
    /* The frame did not complete; the slot goes back to the pool untouched. */
    void (*release)(int slot);
    /* Copy the named layers into this slot from the last published one.
     *
     * Layers persist between frames in the host's own buffers, so a frame is
     * complete when every layer holds a picture rather than when every layer
     * was drawn this tick. A zero-copy frame has no such buffers: it publishes
     * a different pool slot each time, so a layer the rotating schedule chose
     * not to redraw has to be carried into the new slot or it comes back as
     * whatever that slot held several frames ago. Optional; a sink without it
     * simply cannot rotate. */
    /* Return the subset successfully copied; any missing requested bit drops the frame. */
    /* Metadata must come from the exact published images copied, never a
     * newer producer frame. source is zeroed by the caller. */
    uint32_t (*carry)(int slot, uint32_t layer_mask, HaloPanoramaInfo *source);
} HaloPanoramaGPUSink;
void host_panorama_set_gpu_sink(const HaloPanoramaGPUSink *sink);
/* Slot the current frame's views were blitted into, or -1. */
int host_panorama_gpu_slot(void);
/* Carry every layer this frame did not redraw into the current slot.
 * Called once after the passes and before the frame is presented. */
void host_panorama_gpu_carry_missing(void);
/* The platform bridge now owns the slot (it publishes when the GPU finishes). */
void host_panorama_gpu_handoff(void);
/* A zero-copy sink is registered (no CPU layer copies are ever made). */
int host_panorama_gpu_sink_active(void);
#endif
