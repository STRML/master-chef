/*
 * xr.h - OpenXR shell module for the Halo Quest 3 port (Phase 4).
 *
 * Three concerns, split by compilability:
 *
 *  - Pose math (xr_quat_*, xr_pose_*, xr_eye_poses, xr_view_matrix,
 *    xr_projection_matrix): pure float math over OpenXR core types.
 *    Compiles and runs on the macOS host (unit tests) and on arm64
 *    Android; no runtime dependency.
 *  - Lifecycle FSM (xr_lc_*): the app state machine driven by
 *    XrSessionState events and Android activity commands (onResume /
 *    onPause / back). Pure transitions over the OpenXR state diagram;
 *    host-testable.
 *  - Quad layer UI (xr_quads_*) and the shell driver
 *    (xr_shell_*): composition-layer quad stack, session + per-eye
 *    Vulkan swapchains + frame loop calling the mr_* renderer
 *    contract (metalrenderer.h). The shell's Vulkan code is compiled
 *    only under XR_USE_GRAPHICS_API_VULKAN (the NDK build); the quad
 *    manager is pure state + struct building and host-testable.
 *
 * Matrix convention: all 4x4 matrices are column-major floats
 * (m[col*4+row]) matching Vulkan's layout, right-handed world space,
 * OpenXR view space (-Z forward, +Y up), Vulkan NDC (x right, y down,
 * z in [0,1]).
 *
 * Threading: the shell runs the XR frame loop on the render thread
 * (the engine worker thread of main.c); event polling and
 * xrPollEvent-derived FSM transitions happen there.
 */
#ifndef HALO_XR_H
#define HALO_XR_H

#include <openxr/openxr.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Pose math (pure; host + device)                                   */
/* ------------------------------------------------------------------ */

static inline XrQuaternionf xr_quat_identity(void) {
    XrQuaternionf q = {0.0f, 0.0f, 0.0f, 1.0f};
    return q;
}

/* Hamilton product, matching XrQuaternionf (x,y,z,w). */
XrQuaternionf xr_quat_mul(XrQuaternionf a, XrQuaternionf b);

/* Inverse of a unit quaternion (conjugate). */
XrQuaternionf xr_quat_conjugate(XrQuaternionf q);

/* Scales to unit length; returns identity for a degenerate (0,0,0,0)
 * input. */
XrQuaternionf xr_quat_normalize(XrQuaternionf q);

/* Rotate a vector by a unit quaternion. */
XrVector3f xr_quat_rotate(XrQuaternionf q, XrVector3f v);

/* Right-handed rotation about a unit axis by angle radians. */
XrQuaternionf xr_quat_from_axis_angle(XrVector3f axis, float angle);

/*
 * Compose two rigid poses: result = a ∘ b, i.e. apply b first, then a.
 * Position is transformed by a's rotation.
 */
XrPosef xr_pose_compose(XrPosef a, XrPosef b);

/* Inverse of a rigid pose (rotation + translation). */
XrPosef xr_pose_inverse(XrPosef p);

/*
 * Derive left/right eye poses from a head pose in view space.
 * Each eye is the head pose translated by ±ipd/2 along the head's
 * local +X axis (view space is right-handed, +X to the right).
 * out[0] = left eye, out[1] = right eye (matches the OpenXR
 * stereo view order).
 */
void xr_eye_poses(XrPosef head, float ipd, XrPosef out[2]);

/*
 * View matrix for an eye pose: the matrix that maps world-space
 * (the pose's reference space) points into the eye's view space,
 * i.e. the inverse of the rigid pose transform. Column-major.
 */
void xr_view_matrix(XrPosef p, float m[16]);

/*
 * Off-axis projection matrix for one eye's XrFovf, near/far, with
 * Vulkan conventions: y is flipped into NDC (top = -1) and z maps
 * [near, far] -> [0, 1]. Column-major.
 */
void xr_projection_matrix(XrFovf fov, float nearZ, float farZ, float m[16]);

/* ------------------------------------------------------------------ */
/* Lifecycle state machine (pure; host + device)                     */
/* ------------------------------------------------------------------ */

/*
 * App-level state, coarser than XrSessionState: the state the D3D
 * host and the engine care about.
 */
typedef enum xr_app_state {
    XR_APP_NEW = 0,      /* no instance/session yet */
    XR_APP_READY,        /* session ready, not rendering (focus lost) */
    XR_APP_RUNNING,      /* rendering frames */
    XR_APP_PAUSED,       /* focus lost mid-session; engine suspended */
    XR_APP_EXITING,      /* exit requested; drain the frame loop */
    XR_APP_EXITED,       /* loop terminated; teardown complete */
} xr_app_state;

/* External events the FSM consumes. */
typedef enum xr_lc_event {
    XR_LC_SESSION_READY = 1,    /* XR_SESSION_STATE_READY */
    XR_LC_SESSION_RUNNING,      /* XR_SESSION_STATE_SYNCHRONIZED/VISIBLE/RUNNING */
    XR_LC_SESSION_STOPPING,     /* XR_SESSION_STATE_STOPPING */
    XR_LC_SESSION_EXITING,      /* XR_SESSION_STATE_EXITING */
    XR_LC_SESSION_CLOSING,      /* XR_SESSION_STATE_CLOSING / DESTROYED */
    XR_LC_FOCUS_LOST,           /* Android onPause / focus state change */
    XR_LC_FOCUS_GAINED,         /* Android onResume with a live session */
    XR_LC_QUIT,                 /* Android back button / XR_APP_QUIT_REQUEST */
    XR_LC_INSTANCE_LOSS,        /* XR_SESSION_STATE instance loss pending */
} xr_lc_event;

/* Actions the FSM requests of the caller after each transition. */
enum {
    XR_LC_ACT_NONE      = 0,
    XR_LC_ACT_BEGIN     = 1 << 0,  /* xrBeginSession */
    XR_LC_ACT_END       = 1 << 1,  /* xrEndSession */
    XR_LC_ACT_ENQUEUE   = 1 << 2,  /* engine may render frames */
    XR_LC_ACT_SUSPEND   = 1 << 3,  /* engine must stop rendering */
    XR_LC_ACT_QUIT      = 1 << 4,  /* tear down the loop and exit */
};

typedef struct xr_lc {
    xr_app_state state;
    int          actions;          /* last emitted action mask */
    bool         focused;          /* app focus flag */
} xr_lc;

/* Initialise to XR_APP_NEW. Idempotent. */
void xr_lc_init(xr_lc *lc);

/*
 * Feed one event. Returns 0 if the event was accepted (state may have
 * changed), -1 if the event is invalid for the current state (state
 * unchanged). lc->actions holds the action mask for the transition.
 *
 * Rules (the OpenXR session state diagram, Android variant):
 *   NEW       + SESSION_READY   -> RUNNING   (BEGIN|ENQUEUE)
 *   READY     + SESSION_RUNNING -> RUNNING   (ENQUEUE)
 *   READY     + QUIT            -> EXITING   (QUIT)
 *   RUNNING   + FOCUS_LOST      -> PAUSED    (SUSPEND)
 *   RUNNING   + SESSION_STOPPING-> PAUSED    (SUSPEND)
 *   RUNNING   + QUIT            -> EXITING   (SUSPEND|QUIT)
 *   PAUSED    + FOCUS_GAINED    -> RUNNING   (ENQUEUE)
 *   PAUSED    + SESSION_RUNNING -> RUNNING   (ENQUEUE)
 *   PAUSED    + SESSION_STOPPING-> EXITING   (END|QUIT)
 *   PAUSED    + QUIT            -> EXITING   (END|QUIT)
 *   EXITING   + SESSION_CLOSING -> EXITED    (QUIT)
 *   EXITING   + QUIT            -> EXITED    (QUIT)
 *   any       + INSTANCE_LOSS   -> EXITING   (END|QUIT)
 */
int xr_lc_send(xr_lc *lc, xr_lc_event ev);

/* Map a raw XrSessionState to the FSM event it triggers (0 if none). */
xr_lc_event xr_lc_event_from_session_state(XrSessionState s);

static inline bool xr_lc_should_render(const xr_lc *lc) {
    return lc->state == XR_APP_RUNNING && (lc->actions & XR_LC_ACT_ENQUEUE);
}
static inline bool xr_lc_should_exit(const xr_lc *lc) {
    return lc->state == XR_APP_EXITING || lc->state == XR_APP_EXITED;
}

/* ------------------------------------------------------------------ */
/* Quad layer UI (pure state; host + device)                         */
/* ------------------------------------------------------------------ */

#define XR_QUADS_MAX 16

/*
 * A composition-layer quad: a billboard anchored in a reference space
 * owns the quad list and builds the XrCompositionLayerQuad array for
 * xrEndFrame; swapchain creation is the shell's job (the
 * xr_quads::swapchain field is filled by the host).
 */
typedef struct xr_quad {
    bool            in_use;
    uint32_t        id;
    XrPosef         pose;          /* center + orientation in head space */
    XrExtent2Df     size;        /* meters */
    XrSwapchain     swapchain;   /* host-provided; 0 = not rendered */
    uint32_t        width, height;
    XrColor4f       tint;        /* modulate color (XR_KHR_composition_layer_color_scale_bias style) */
    int             z;           /* draw order; higher = closer to viewer */
    bool            visible;
    void          *renderer;     /* mr_context owned by the quad (device) */
} xr_quad;

typedef struct xr_quads {
    XrSpace    space;                       /* head-space the poses live in */
    uint32_t   count;
    xr_quad    quad[XR_QUADS_MAX];
    /* Layer array handed to xrEndFrame (sorted by z, stable). */
    XrCompositionLayerQuad layers[XR_QUADS_MAX];
    const XrCompositionLayerBaseHeader *layer_ptrs[XR_QUADS_MAX];
} xr_quads;

void xr_quads_init(xr_quads *qs, XrSpace space);
/* Add a quad; returns its id (>=1), 0 if full. */
uint32_t xr_quads_add(xr_quads *qs, XrPosef pose, XrExtent2Df size,
                      XrColor4f tint, int z);
/* Set the swapchain + size for a quad (host creates it). */
int xr_quads_bind_swapchain(xr_quads *qs, uint32_t id, XrSwapchain sc,
                            uint32_t width, uint32_t height);
int xr_quads_set_visible(xr_quads *qs, uint32_t id, bool visible);
int xr_quads_set_pose(xr_quads *qs, uint32_t id, XrPosef pose);
int xr_quads_remove(xr_quads *qs, uint32_t id);
/*
 * Rebuild the visible layer array, sorted by z ascending (painter's
 * order: last = frontmost). Returns the number of visible layers.
 */
uint32_t xr_quads_build_layers(xr_quads *qs);

/* Built-in layout presets: HUD anchor, loading panel, exit button. */
void xr_quads_default_layout(xr_quads *qs);

/* ------------------------------------------------------------------ */
/* Shell (runtime; device only)                                      */
/* ------------------------------------------------------------------ */

typedef struct xr_shell xr_shell;

typedef struct xr_shell_config {
    const char *app_name;
    const char *engine_root;
    /* Activity/VM handles for XR_KHR_android_create_instance (void* so
     * this header compiles without JNI). */
    void *application_vm;
    void *application_activity;
    /* Frame cap; <=0 = run until exit. */
    int max_frames;
    /* Render target: the engine's shared offscreen context (the D3D
     * shim draws into it). The shell blits it into each eye. */
    struct mr_context *engine_target;
    /* Quad UI. */
    bool show_quads;
} xr_shell_config;

/*
 * Create the XR instance + Android session + per-eye Vulkan swapchains
 * + view space. Returns NULL on failure (reason in xr_shell_last_error).
 * No-op stub (returns NULL) when built without XR_USE_GRAPHICS_API_VULKAN.
 */
xr_shell *xr_shell_create(const xr_shell_config *cfg);
/*
 * Pump events + run the frame loop: xrWaitFrame -> acquire -> render
 * (engine target blitted into the eye swapchain images via mr_*) ->
 * xrEndFrame with the projection layer and the quad layer stack.
 * Returns 0 on a clean exit, negative on failure.
 */
int xr_shell_run(xr_shell *shell);
void xr_shell_destroy(xr_shell *shell);
/* One frame; returns 0 on success, 1 = skip (no render), <0 = fatal. */
int xr_shell_frame(xr_shell *shell);
/* Request exit from the render loop (thread-safe). */
void xr_shell_request_exit(xr_shell *shell);
const char *xr_shell_last_error(void);
/* The head-space pose of the most recent frame (for quad anchoring). */
XrPosef xr_shell_head_pose(const xr_shell *shell);

#ifdef __cplusplus
}
#endif
#endif /* HALO_XR_H */
