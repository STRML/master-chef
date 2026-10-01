/*
 * xr_tests.c - host-side unit tests for the OpenXR shell's pure logic:
 * pose math (section 1 of xr_shell.c), the lifecycle FSM, and the quad
 * layer stack. No OpenXR runtime is required; the tests compile against
 * the vendored core headers and link nothing from the loader.
 */
#include "xr.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>

#define EPS 1e-5f
static int near(float a, float b) { return fabsf(a - b) < EPS; }
static int pose_eq(XrPosef a, XrPosef b) {
    return near(a.position.x, b.position.x) && near(a.position.y, b.position.y) &&
           near(a.position.z, b.position.z) && near(a.orientation.x, b.orientation.x) &&
           near(a.orientation.y, b.orientation.y) && near(a.orientation.z, b.orientation.z) &&
           near(a.orientation.w, b.orientation.w);
}

static XrVector3f V(float x, float y, float z) { XrVector3f v = {x, y, z}; return v; }
static XrQuaternionf Q(float x, float y, float z, float w) { XrQuaternionf q = {x, y, z, w}; return q; }
static XrPosef P(XrQuaternionf q, XrVector3f p) { XrPosef o = {q, p}; return o; }

/* ---- pose math ---- */

static void test_quat(void) {
    XrQuaternionf id = xr_quat_identity();
    /* 90 deg yaw about +Y: forward (0,0,-1) -> (-1,0,0). */
    XrQuaternionf y90 = xr_quat_from_axis_angle(V(0, 1, 0), (float)M_PI / 2);
    XrVector3f f = xr_quat_rotate(y90, V(0, 0, -1));
    assert(near(f.x, -1) && near(f.y, 0) && near(f.z, 0));
    /* +X -> -Z. */
    XrVector3f r = xr_quat_rotate(y90, V(1, 0, 0));
    assert(near(r.x, 0) && near(r.y, 0) && near(r.z, -1));
    /* q * conj(q) = identity. */
    XrQuaternionf p = xr_quat_mul(y90, xr_quat_conjugate(y90));
    assert(pose_eq(P(p, V(0, 0, 0)), P(id, V(0, 0, 0))));
    /* Normalization: (0,0,0,0) -> identity; (2,0,0,0) -> (1,0,0,0). */
    XrQuaternionf n = xr_quat_normalize(Q(0, 0, 0, 0));
    assert(near(n.w, 1));
    n = xr_quat_normalize(Q(2, 0, 0, 0));
    assert(near(n.x, 1) && near(n.w, 0));
    /* Composition: yaw 90 then yaw 90 = yaw 180. */
    XrQuaternionf two = xr_quat_mul(y90, y90);
    XrVector3f f2 = xr_quat_rotate(two, V(0, 0, -1));
    assert(near(f2.x, 0) && near(f2.z, 1));
    puts("quat ok");
}

static void test_pose(void) {
    XrPosef head = P(xr_quat_from_axis_angle(V(0, 1, 0), 0.5f), V(1, 2, 3));
    XrPosef inv = xr_pose_inverse(head);
    XrPosef id = P(xr_quat_identity(), V(0, 0, 0));
    assert(pose_eq(xr_pose_compose(head, inv), id));
    /* Eye poses: head with no rotation -> ±ipd/2 along X. */
    XrPosef eyes[2];
    XrPosef axis = P(xr_quat_identity(), V(0, 0, -2));
    xr_eye_poses(axis, 0.0635f, eyes);
    assert(near(eyes[0].position.x, -0.03175f) && near(eyes[1].position.x, 0.03175f));
    assert(near(eyes[0].position.z, -2) && near(eyes[1].position.z, -2));
    /* Yawed head: the eye offset rotates with the head. */
    XrPosef yaw = P(xr_quat_from_axis_angle(V(0, 1, 0), (float)M_PI / 2), V(0, 0, 0));
    xr_eye_poses(yaw, 0.0635f, eyes);
    assert(near(eyes[1].position.z, -0.03175f) && near(eyes[1].position.x, 0));
    /* Forward (0,0,-1) at yaw 90 maps to (-1,0,0). */
    XrVector3f fwd = xr_quat_rotate(yaw.orientation, V(0, 0, -1));
    assert(near(fwd.x, -1) && near(fwd.z, 0));
    puts("pose ok");
}

/* Apply a column-major 4x4 to a point (x,y,z,w=1). */
static void xf(const float m[16], float x, float y, float z, float out[4]) {
    out[0] = m[0]*x + m[4]*y + m[8]*z + m[12];
    out[1] = m[1]*x + m[5]*y + m[9]*z + m[13];
    out[2] = m[2]*x + m[6]*y + m[10]*z + m[14];
    out[3] = m[3]*x + m[7]*y + m[11]*z + m[15];
}

static void test_matrices(void) {
    /* View matrix: the eye's own position maps to the origin. */
    XrPosef eye = P(xr_quat_from_axis_angle(V(0, 1, 0), 0.3f), V(4, 5, 6));
    float v[16];
    xr_view_matrix(eye, v);
    float o[4];
    xf(v, 4, 5, 6, o);
    assert(near(o[0], 0) && near(o[1], 0) && near(o[2], 0));
    /* A point 1 unit in front of the eye (local -Z) is at z=-1. */
    XrVector3f fwd = xr_quat_rotate(eye.orientation, V(0, 0, -1));
    xf(v, 4 + fwd.x, 5 + fwd.y, 6 + fwd.z, o);
    assert(near(o[2], -1));

    /* Projection: symmetric 90 deg fov, near 0.1 far 100. */
    XrFovf fov = {-(float)M_PI / 4, (float)M_PI / 4, (float)M_PI / 4, -(float)M_PI / 4};
    float p[16];
    xr_projection_matrix(fov, 0.1f, 100.0f, p);
    float c[4];
    /* Center of the view frustum -> NDC (0,0). */
    xf(p, 0, 0, -0.1f, c);
    assert(near(c[0], 0) && near(c[1], 0));
    /* Near plane -> ndc z 0; far plane -> ndc z 1. */
    xf(p, 0, 0, -0.1f, c);
    assert(near(c[2] / c[3], 0));
    xf(p, 0, 0, -100.0f, c);
    assert(near(c[2] / c[3], 1));
    /* Top of the frustum (y up) -> ndc y = -1 (Vulkan is y-down). */
    xf(p, 0, 0.1f, -0.1f, c);
    assert(near(c[1] / c[3], -1));
    /* Bottom -> +1. */
    xf(p, 0, -0.1f, -0.1f, c);
    assert(near(c[1] / c[3], 1));
    /* Right edge -> +1, left -> -1. */
    xf(p, 0.1f, 0, -0.1f, c);
    assert(near(c[0] / c[3], 1));
    xf(p, -0.1f, 0, -0.1f, c);
    assert(near(c[0] / c[3], -1));
    puts("matrix ok");
}

/* ---- lifecycle FSM ---- */
static void test_fsm(void) {
    xr_lc lc;
    xr_lc_init(&lc);
    assert(lc.state == XR_APP_NEW);
    /* Invalid events in NEW are rejected. */
    assert(xr_lc_send(&lc, XR_LC_FOCUS_LOST) == -1);
    assert(xr_lc_send(&lc, XR_LC_QUIT) == -1);
    assert(xr_lc_send(&lc, XR_LC_SESSION_RUNNING) == -1);
    /* Session ready -> running, begin+enqueue. */
    assert(xr_lc_send(&lc, XR_LC_SESSION_READY) == 0);
    assert(lc.state == XR_APP_RUNNING);
    assert(lc.actions & XR_LC_ACT_BEGIN);
    assert(lc.actions & XR_LC_ACT_ENQUEUE);
    assert(xr_lc_should_render(&lc));
    /* The runtime's session-running event is a no-op refresh. */
    assert(xr_lc_send(&lc, XR_LC_SESSION_RUNNING) == 0);
    assert(xr_lc_should_render(&lc));
    /* onPause: focus lost -> PAUSED, engine suspended. */
    assert(xr_lc_send(&lc, XR_LC_FOCUS_LOST) == 0);
    assert(lc.state == XR_APP_PAUSED);
    assert(lc.actions & XR_LC_ACT_SUSPEND);
    assert(!xr_lc_should_render(&lc));
    /* Back in PAUSED is a quit, not a resume. */
    assert(xr_lc_send(&lc, XR_LC_QUIT) == 0);
    assert(lc.state == XR_APP_EXITING);
    assert(lc.actions & XR_LC_ACT_QUIT);
    assert(xr_lc_should_exit(&lc));
    /* Runtime exiting -> EXITED. */
    assert(xr_lc_send(&lc, XR_LC_SESSION_EXITING) == 0);
    assert(lc.state == XR_APP_EXITED);
    /* Terminal: everything rejected. */
    assert(xr_lc_send(&lc, XR_LC_FOCUS_GAINED) == -1);
    assert(xr_lc_send(&lc, XR_LC_SESSION_READY) == -1);

    /* Resume path: RUNNING -> PAUSED -> RUNNING -> STOPPING -> EXITING. */
    xr_lc lc2;
    xr_lc_init(&lc2);
    xr_lc_send(&lc2, XR_LC_SESSION_READY);
    assert(xr_lc_send(&lc2, XR_LC_FOCUS_LOST) == 0);
    assert(xr_lc_send(&lc2, XR_LC_FOCUS_GAINED) == 0);
    assert(lc2.state == XR_APP_RUNNING);
    assert(xr_lc_should_render(&lc2));
    /* STOPPING from RUNNING -> PAUSED. */
    assert(xr_lc_send(&lc2, XR_LC_SESSION_STOPPING) == 0);
    assert(lc2.state == XR_APP_PAUSED);
    /* Instance loss from PAUSED -> EXITING. */
    assert(xr_lc_send(&lc2, XR_LC_INSTANCE_LOSS) == 0);
    assert(lc2.state == XR_APP_EXITING);

    /* Session state -> event mapping (legacy header names). */
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_READY) == XR_LC_SESSION_READY);
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_FOCUSED) == XR_LC_SESSION_RUNNING);
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_VISIBLE) == XR_LC_SESSION_RUNNING);
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_SYNCHRONIZED) == XR_LC_SESSION_RUNNING);
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_STOPPING) == XR_LC_SESSION_STOPPING);
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_LOSS_PENDING) == XR_LC_INSTANCE_LOSS);
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_EXITING) == XR_LC_SESSION_EXITING);
    assert(xr_lc_event_from_session_state(XR_SESSION_STATE_IDLE) == 0);
    puts("fsm ok");
}

/* ---- quad layer stack ---- */
static void test_quads(void) {
    xr_quads qs;
    xr_quads_init(&qs, (XrSpace)(uintptr_t)0x1000);
    assert(qs.count == 0);
    XrPosef p = {xr_quat_identity(), {0, 0, -1}};
    XrExtent2Df s = {0.4f, 0.25f};
    XrColor4f w = {1, 1, 1, 1};
    /* Insertion order is arbitrary; layers must come out ascending z. */
    uint32_t a = xr_quads_add(&qs, p, s, w, 5);
    uint32_t b = xr_quads_add(&qs, p, s, w, 1);
    uint32_t c = xr_quads_add(&qs, p, s, w, 9);
    assert(a == 1 && b == 2 && c == 3);
    assert(qs.count == 3);
    assert(qs.quad[0].id == b && qs.quad[1].id == a && qs.quad[2].id == c);
    /* Equal z keeps insertion order (stable). */
    uint32_t d = xr_quads_add(&qs, p, s, w, 5);
    assert(d == 4);
    assert(qs.quad[1].id == a && qs.quad[2].id == d);

    /* Build: only quads with a bound swapchain are layered. */
    assert(xr_quads_build_layers(&qs) == 0);
    for (uint32_t i = 0; i < 3; i++)
        assert(xr_quads_bind_swapchain(&qs, qs.quad[i].id,
            (XrSwapchain)(uintptr_t)(0x2000 + i), 64, 64) == 0);
    uint32_t n = xr_quads_build_layers(&qs);
    assert(n == 3);
    assert(qs.layers[0].subImage.swapchain == (XrSwapchain)(uintptr_t)0x2000);
    assert(qs.layers[0].size.width == 0.4f && qs.layers[0].size.height == 0.25f);
    assert(qs.layers[0].space == (XrSpace)(uintptr_t)0x1000);
    assert(qs.layers[0].type == XR_TYPE_COMPOSITION_LAYER_QUAD);

    /* Visibility toggles the layer count; pose updates land in the
     * layer. */
    assert(xr_quads_set_visible(&qs, qs.quad[1].id, false) == 0);
    assert(xr_quads_build_layers(&qs) == 2);
    assert(xr_quads_set_visible(&qs, qs.quad[1].id, true) == 0);
    assert(xr_quads_build_layers(&qs) == 3);
    XrPosef p2 = {xr_quat_identity(), {0.1f, 0.2f, -1.5f}};
    assert(xr_quads_set_pose(&qs, qs.quad[0].id, p2) == 0);
    assert(xr_quads_build_layers(&qs) == 3);
    assert(near(qs.layers[0].pose.position.x, 0.1f));
    assert(near(qs.layers[0].pose.position.z, -1.5f));

    /* Unknown ids: no crash, -1. */
    assert(xr_quads_set_visible(&qs, 99, true) == -1);
    assert(xr_quads_set_pose(&qs, 99, p) == -1);
    assert(xr_quads_remove(&qs, 99) == -1);
    /* Remove compacts the stack. */
    assert(xr_quads_remove(&qs, a) == 0);
    assert(qs.count == 3);
    for (uint32_t i = 0; i < qs.count; i++)
        assert(qs.quad[i].id != a);

    /* Capacity: fill to the max, then overflow is refused. */
    xr_quads qs2;
    xr_quads_init(&qs2, (XrSpace)(uintptr_t)1);
    for (uint32_t i = 0; i < XR_QUADS_MAX; i++)
        assert(xr_quads_add(&qs2, p, s, w, (int)i) != 0);
    assert(qs2.count == XR_QUADS_MAX);
    assert(xr_quads_add(&qs2, p, s, w, 99) == 0);

    /* Default layout: two head-anchored quads, z ascending. */
    xr_quads qs3;
    xr_quads_init(&qs3, (XrSpace)(uintptr_t)2);
    xr_quads_default_layout(&qs3);
    assert(qs3.count == 2);
    assert(qs3.quad[0].z < qs3.quad[1].z);
    assert(near(qs3.quad[0].pose.position.z, -1.5f));
    puts("quads ok");
}

int main(void) {
    test_quat();
    test_pose();
    test_matrices();
    test_fsm();
    test_quads();
    puts("all xr tests passed");
    return 0;
}
