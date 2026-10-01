/* openxr_input.c - OpenXR actions -> HostGCSnapshot bridge (Quest 3 input).
 *
 * Phase 5, input half of the issue #1 plan. On device builds
 * (HALO_OPENXR_INPUT) this replaces gamecontroller_stub.c: the DirectInput
 * shim polls hostgc_poll() for a HostGCSnapshot; here the snapshot is filled
 * from OpenXR 1.0 action states so the translated engine sees a standard
 * extended gamepad (docs/CONTROLS.md).
 *
 * OpenXR core 1.0 action API: XrActionCreateInfo carries countSubactionPaths/
 * subactionPaths (hands are per-action, not per-binding), bindings are
 * {action, XrPath}, and the action set must be attached to the session
 * (xrAttachSessionActionSets) before xrSyncActions.
 *
 * Device handles are injected by xr_input_attach (the xr shell owns instance
 * and session creation); this file never synthesizes one.
 */
#include "host.h"
#include "gamecontroller.h"
#include "openxr_input.h"
#include <string.h>
#include <stdatomic.h>

#ifdef HALO_OPENXR_INPUT
#include <openxr/openxr.h>

typedef struct {
    XrInstance inst;
    XrSession session;
    XrActionSet set;
    XrAction stickL, stickR, trigL, trigR;
    XrAction a, b, x, y;
    XrAction clickL, clickR, menu;
    XrAction gripL, gripR;
    XrAction hapticL, hapticR;
    XrPath handL, handR;
    atomic_ullong sequence;
    atomic_int attached;
} xr_input_ctx;

static xr_input_ctx g_xi;

static int make_action(XrActionSet set, XrPath *hands, const char *name,
                       const char *loc, XrActionType type, XrAction *out) {
    XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = type;
    snprintf(aci.actionName, sizeof aci.actionName, "%s", name);
    snprintf(aci.localizedActionName, sizeof aci.localizedActionName, "%s", loc);
    aci.countSubactionPaths = 2;
    aci.subactionPaths = hands;
    return xrCreateAction(set, &aci, out) == XR_SUCCESS ? 0 : -1;
}

int xr_input_attach(XrInstance inst, XrSession session) {
    xr_input_ctx *c = &g_xi;
    memset(c, 0, sizeof *c);
    c->inst = inst; c->session = session;

    XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(asci.actionSetName, sizeof asci.actionSetName, "gameplay");
    snprintf(asci.localizedActionSetName, sizeof asci.localizedActionSetName, "Halo gameplay");
    if (xrCreateActionSet(inst, &asci, &c->set) != XR_SUCCESS) return -1;

    if (xrStringToPath(inst, "/user/hand/left", &c->handL) != XR_SUCCESS) return -1;
    if (xrStringToPath(inst, "/user/hand/right", &c->handR) != XR_SUCCESS) return -1;
    XrPath hands[2] = {c->handL, c->handR};

    struct { const char *n, *l; XrActionType t; XrAction *o; } acts[] = {
        {"stick_l", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT, &c->stickL},
        {"stick_r", "Aim", XR_ACTION_TYPE_VECTOR2F_INPUT, &c->stickR},
        {"trig_l", "Grenade", XR_ACTION_TYPE_FLOAT_INPUT, &c->trigL},
        {"trig_r", "Fire", XR_ACTION_TYPE_FLOAT_INPUT, &c->trigR},
        {"a", "Jump", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->a},
        {"b", "Crouch", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->b},
        {"x", "Reload", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->x},
        {"y", "Melee", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->y},
        {"click_l", "Walk", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->clickL},
        {"click_r", "Zoom", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->clickR},
        {"menu", "Pause", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->menu},
        {"grip_l", "Grip", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->gripL},
        {"grip_r", "Grip", XR_ACTION_TYPE_BOOLEAN_INPUT, &c->gripR},
        {"haptic_l", "Impact left", XR_ACTION_TYPE_VIBRATION_OUTPUT, &c->hapticL},
        {"haptic_r", "Impact right", XR_ACTION_TYPE_VIBRATION_OUTPUT, &c->hapticR},
    };
    for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++)
        if (make_action(c->set, hands, acts[i].n, acts[i].l, acts[i].t, acts[i].o))
            return -1;

    XrPath profile;
    if (xrStringToPath(inst, "/interaction_profiles/oculus/touch_controls", &profile) != XR_SUCCESS)
        return -1;

    XrActionSuggestedBinding binds[] = {
        {c->stickL, XR_NULL_PATH}, {c->stickR, XR_NULL_PATH},
        {c->trigL, XR_NULL_PATH},  {c->trigR, XR_NULL_PATH},
        {c->a, XR_NULL_PATH},      {c->b, XR_NULL_PATH},
        {c->x, XR_NULL_PATH},      {c->y, XR_NULL_PATH},
        {c->clickL, XR_NULL_PATH}, {c->clickR, XR_NULL_PATH},
        {c->menu, XR_NULL_PATH},   {c->gripL, XR_NULL_PATH}, {c->gripR, XR_NULL_PATH},
        {c->hapticL, XR_NULL_PATH}, {c->hapticR, XR_NULL_PATH},
    };
    const char *leaf[] = {
        "/input/left/thumbstick", "/input/right/thumbstick",
        "/input/left/trigger", "/input/right/trigger",
        "/input/a_touch", "/input/b_touch", "/input/x_touch", "/input/y_touch",
        "/input/left_thumbstick_click", "/input/right_thumbstick_click",
        "/input/menu_touch", "/input/left_squeeze_click", "/input/right_squeeze_click",
        "/output/left_haptic", "/output/right_haptic",
    };
    for (size_t i = 0; i < sizeof binds / sizeof binds[0]; i++)
        if (xrStringToPath(inst, leaf[i], &binds[i].binding) != XR_SUCCESS)
            return -1;

    XrInteractionProfileSuggestedBinding isb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    isb.interactionProfile = profile;
    isb.suggestedBindings = binds;
    isb.countSuggestedBindings = (uint32_t)(sizeof binds / sizeof binds[0]);
    if (xrSuggestInteractionProfileBindings(inst, &isb) != XR_SUCCESS) return -1;

    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.actionSets = &c->set;
    attach.countActionSets = 1;
    if (xrAttachSessionActionSets(session, &attach) != XR_SUCCESS) return -1;

    atomic_store(&c->attached, 1);
    return 0;
}

int xr_input_poll_snapshot(HostGCSnapshot *out) {
    xr_input_ctx *c = &g_xi;
    if (!atomic_load(&c->attached) || !out) return -1;
    memset(out, 0, sizeof *out);

    XrActiveActionSet sets[] = {{c->set, XR_NULL_PATH}};
    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.activeActionSets = sets;
    sync.countActiveActionSets = 1;
    if (xrSyncActions(c->session, &sync) != XR_SUCCESS) return -1;

    XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
    uint64_t seq = atomic_fetch_add(&c->sequence, 1) + 1;

    #define GET2F(act, ox, oy) do { \
        gi.action = (act); \
        XrActionStateVector2f sv = {XR_TYPE_ACTION_STATE_VECTOR2F}; \
        if (xrGetActionStateVector2f(c->session, &gi, &sv) == XR_SUCCESS && sv.isActive) { \
            (ox) = sv.currentState.x; (oy) = sv.currentState.y; out->connected = true; } \
    } while (0)
    #define GETF(act, dst) do { \
        gi.action = (act); \
        XrActionStateFloat f = {XR_TYPE_ACTION_STATE_FLOAT}; \
        if (xrGetActionStateFloat(c->session, &gi, &f) == XR_SUCCESS && f.isActive) \
            (dst) = f.currentState; \
    } while (0)
    #define GETB(act, dst) do { \
        gi.action = (act); \
        XrActionStateBoolean b = {XR_TYPE_ACTION_STATE_BOOLEAN}; \
        if (xrGetActionStateBoolean(c->session, &gi, &b) == XR_SUCCESS && b.isActive) \
            (dst) = b.currentState; \
    } while (0)

    GET2F(c->stickL, out->lx, out->ly);
    GET2F(c->stickR, out->rx, out->ry);
    GETF(c->trigL, out->lt);
    GETF(c->trigR, out->rt);
    GETB(c->a, out->buttons[HOSTGC_BTN_A]);
    GETB(c->b, out->buttons[HOSTGC_BTN_B]);
    GETB(c->x, out->buttons[HOSTGC_BTN_X]);
    GETB(c->y, out->buttons[HOSTGC_BTN_Y]);
    GETB(c->clickL, out->buttons[HOSTGC_BTN_LTHUMB]);
    GETB(c->clickR, out->buttons[HOSTGC_BTN_RTHUMB]);
    GETB(c->menu, out->buttons[HOSTGC_BTN_MENU]);
    GETB(c->gripL, out->buttons[HOSTGC_BTN_LSHOULDER]);
    GETB(c->gripR, out->buttons[HOSTGC_BTN_RSHOULDER]);
    #undef GET2F
    #undef GETF
    #undef GETB

    out->buttons[HOSTGC_BTN_RTRIGGER] = xr_input_trigger_pressed(out->rt);
    out->buttons[HOSTGC_BTN_LTRIGGER] = xr_input_trigger_pressed(out->lt);
    /* Dpad: Touch has no hardware dpad; synthesize from the left stick. */
    if (out->lx * out->lx + out->ly * out->ly > 0.49f) {
        if (out->ly > 0.5f) out->dpad_up = true;
        if (out->ly < -0.5f) out->dpad_down = true;
        if (out->lx > 0.5f) out->dpad_right = true;
        if (out->lx < -0.5f) out->dpad_left = true;
    }
    out->sequence = seq;
    return 0;
}

void xr_input_play_haptic(float intensity, float sharpness) {
    xr_input_ctx *c = &g_xi;
    if (!atomic_load(&c->attached)) return;
    float amp, freq;
    if (xr_input_haptic_pulse(intensity, sharpness, &amp, &freq) != 0) return;
    XrHapticVibration vib = {XR_TYPE_HAPTIC_VIBRATION};
    vib.amplitude = amp;
    vib.frequency = freq;
    vib.duration = 80 * 1000000; /* 80 ms short impact */
    XrHapticActionInfo hai = {XR_TYPE_HAPTIC_ACTION_INFO};
    hai.action = c->hapticR;
    xrApplyHapticFeedback(c->session, &hai, (const XrHapticBaseHeader *)&vib);
}

bool hostgc_poll(HostGCSnapshot *out) { return xr_input_poll_snapshot(out) == 0; }
bool hostgc_connected(void) { return atomic_load(&g_xi.attached) != 0; }
void hostgc_play_haptic(float intensity, float sharpness) { xr_input_play_haptic(intensity, sharpness); }
void hostgc_init(void) { }

#endif /* HALO_OPENXR_INPUT */

/* ---- pure mapping (host-tested) ---- */
void xr_input_to_snapshot(const xr_input_reading *in, uint64_t sequence,
                          void *out_snapshot) {
    HostGCSnapshot *s = (HostGCSnapshot *)out_snapshot;
    memset(s, 0, sizeof *s);
    s->connected = in->tracked;
    s->lx = in->lx; s->ly = in->ly;
    s->rx = in->rx; s->ry = in->ry;
    s->lt = in->lt; s->rt = in->rt;
    s->buttons[HOSTGC_BTN_A] = in->a;
    s->buttons[HOSTGC_BTN_B] = in->b;
    s->buttons[HOSTGC_BTN_X] = in->x;
    s->buttons[HOSTGC_BTN_Y] = in->y;
    s->buttons[HOSTGC_BTN_LTHUMB] = in->lthumb;
    s->buttons[HOSTGC_BTN_RTHUMB] = in->rthumb;
    s->buttons[HOSTGC_BTN_MENU] = in->menu;
    s->buttons[HOSTGC_BTN_LSHOULDER] = in->squeezeL;
    s->buttons[HOSTGC_BTN_RSHOULDER] = in->squeezeR;
    s->buttons[HOSTGC_BTN_LTRIGGER] = xr_input_trigger_pressed(in->lt);
    s->buttons[HOSTGC_BTN_RTRIGGER] = xr_input_trigger_pressed(in->rt);
    s->sequence = sequence;
}

bool xr_input_trigger_pressed(float v) { return v >= XR_INPUT_TRIGGER_THRESHOLD; }

int xr_input_haptic_pulse(float intensity, float sharpness,
                          float *out_amplitude, float *out_frequency_hz) {
    if (!out_amplitude || !out_frequency_hz) return -1;
    if (!(intensity >= 0.f && intensity <= 1.f) || !(sharpness >= 0.f && sharpness <= 1.f))
        return -1;
    *out_amplitude = intensity;
    *out_frequency_hz = 40.f + sharpness * 360.f;
    return 0;
}
