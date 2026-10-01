/* openxr_input_tests.c - host unit tests for the OpenXR->snapshot mapping
 * (native/EngineHost/android/openxr_input.c, pure half only). Build:
 *   cc -std=c11 -I native/EngineHost -I native/EngineHost/android \
 *      native/EngineHost/android/openxr_input.c \
 *      native/EngineHost/android/openxr_input_tests.c -o .scratch/openxr_input_tests
 * No OpenXR loader, no device: the __ANDROID__ block is excluded; these
 * exercise xr_input_to_snapshot, xr_input_trigger_pressed,
 * xr_input_haptic_pulse, and the CONTROLS.md mapping semantics.
 */
#include "gamecontroller.h"
#include "openxr_input.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void) {
    HostGCSnapshot s;
    xr_input_reading r;

    /* 1: neutral reading -> connected, all axes centered, no buttons */
    memset(&r, 0, sizeof r);
    r.tracked = true;
    xr_input_to_snapshot(&r, 7, &s);
    CHECK(s.connected);
    CHECK(s.sequence == 7);
    CHECK(s.lx == 0.f && s.ly == 0.f);
    CHECK(!s.buttons[HOSTGC_BTN_RTRIGGER]);
    CHECK(!s.buttons[HOSTGC_BTN_LTRIGGER]);

    /* 2: right trigger full -> FIRE (R2 per CONTROLS.md) */
    memset(&r, 0, sizeof r);
    r.tracked = true; r.rt = 1.0f;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(s.rt == 1.0f);
    CHECK(s.buttons[HOSTGC_BTN_RTRIGGER]);
    CHECK(!s.buttons[HOSTGC_BTN_LTRIGGER]);

    /* 3: left trigger full -> GRENADE (L2 per CONTROLS.md) */
    memset(&r, 0, sizeof r);
    r.tracked = true; r.lt = 1.0f;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(s.buttons[HOSTGC_BTN_LTRIGGER]);

    /* 4: trigger threshold = 0.75 (Meta runtime default) */
    CHECK(xr_input_trigger_pressed(0.74f) == false);
    CHECK(xr_input_trigger_pressed(0.75f) == true);
    CHECK(xr_input_trigger_pressed(1.0f) == true);

    /* 5: face buttons map: A jump, B crouch, X reload, Y melee (PS layout:
     * cross=A=bottom, circle=B=right, square=X=left, triangle=Y=top) */
    memset(&r, 0, sizeof r);
    r.tracked = true; r.a = true; r.b = true; r.x = true; r.y = true;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(s.buttons[HOSTGC_BTN_A]);
    CHECK(s.buttons[HOSTGC_BTN_B]);
    CHECK(s.buttons[HOSTGC_BTN_X]);
    CHECK(s.buttons[HOSTGC_BTN_Y]);

    /* 6: stick passthrough: OpenXR +y up == HostGC +y up (no inversion) */
    memset(&r, 0, sizeof r);
    r.tracked = true; r.lx = 0.5f; r.ly = -0.25f; r.rx = -1.f; r.ry = 1.f;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(s.lx == 0.5f);
    CHECK(s.ly == -0.25f);
    CHECK(s.rx == -1.f);
    CHECK(s.ry == 1.f);

    /* 7: menu -> pause (HOSTGC_BTN_MENU is the Options button in the contract) */
    memset(&r, 0, sizeof r);
    r.tracked = true; r.menu = true;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(s.buttons[HOSTGC_BTN_MENU]);

    /* 8: squeeze -> shoulder (grip -> LSHOULDER/LSHOULDER for melee/grenade) */
    memset(&r, 0, sizeof r);
    r.tracked = true; r.squeezeL = true; r.squeezeR = true;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(s.buttons[HOSTGC_BTN_LSHOULDER]);
    CHECK(s.buttons[HOSTGC_BTN_RSHOULDER]);

    /* 9: untracked -> not connected (game falls back to kbd/mouse) */
    memset(&r, 0, sizeof r);
    r.tracked = false;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(!s.connected);

    /* 10: haptic pulse: intensity/sharpness in domain maps, out of domain -1 */
    float amp, freq;
    CHECK(xr_input_haptic_pulse(0.5f, 0.5f, &amp, &freq) == 0);
    CHECK(amp == 0.5f);
    CHECK(freq == 220.f); /* 40 + 0.5*360 */
    CHECK(xr_input_haptic_pulse(-0.1f, 0.f, &amp, &freq) == -1);
    CHECK(xr_input_haptic_pulse(1.f, 1.1f, &amp, &freq) == -1);
    CHECK(xr_input_haptic_pulse(1.f, 0.f, &amp, &freq) == 0);
    CHECK(amp == 1.f && freq == 40.f);
    CHECK(xr_input_haptic_pulse(0.f, 1.f, &amp, &freq) == 0);
    CHECK(amp == 0.f && freq == 400.f);
    CHECK(xr_input_haptic_pulse(0.8f, 0.f, NULL, &freq) == -1);

    /* 11: full forward aim (right stick +x max) -> rx 1 */
    memset(&r, 0, sizeof r);
    r.tracked = true; r.rx = 1.0f;
    xr_input_to_snapshot(&r, 1, &s);
    CHECK(s.rx == 1.0f);
    CHECK(fabsf(s.ry) < 1e-6f);

    if (fails) { printf("openxr_input: %d FAIL\n", fails); return 1; }
    printf("openxr_input: PASS (11 groups, mapping semantics verified)\n");
    return 0;
}
