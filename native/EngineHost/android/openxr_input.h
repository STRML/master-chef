/* openxr_input.h - OpenXR action -> HostGCSnapshot mapping, pure core.
 *
 * The Quest 3 controllers (Touch/Meta HorizonOS) deliver input through OpenXR
 * actions: thumbstick poses (vector2f), analog triggers (float), digital
 * buttons (bool) and haptic outputs. The engine consumes platform-neutral
 * HostGCSnapshot readings (native/EngineHost/gamecontroller.h). This header
 * exposes the translation as pure functions so the mapping is unit-testable
 * without a headset, the loader, or any OpenXR device.
 *
 * Action semantics follow docs/CONTROLS.md (left stick move, right stick aim,
 * R2 fire, L2 grenade, X use/reload, A jump/confirm, B crouch/back, menu
 * pause). Face-button layout follows the PlayStation labels the doc uses:
 * square=left, cross=bottom, circle=right, triangle=top, so on Touch
 * X->square, A->cross, B->circle, Y->triangle.
 */
#ifndef HALO_OPENXR_INPUT_H
#define HALO_OPENXR_INPUT_H

#include <stdint.h>
#include <stdbool.h>

/* One OpenXR action reading, in OpenXR conventions (2F vectors: +x right,
 * +y UP; triggers 0..1; bools from XR_ACTION_STATE_BOOL / VECTOR2F). */
typedef struct {
    bool tracked;          /* a live tracked device feeding this reading   */
    float lx, ly;          /* left thumbstick, [-1,1], y up                */
    float rx, ry;          /* right thumbstick, [-1,1], y up               */
    float lt, rt;          /* triggers, [0,1]                              */
    bool a, b, x, y;       /* Touch face buttons                           */
    bool lthumb, rthumb;   /* thumbstick clicks                          */
    bool ltrigger, rtrigger; /* digital trigger views (past threshold)    */
    bool menu;             /* left-controller menu button                  */
    bool squeezeL, squeezeR; /* grip buttons                             */
} xr_input_reading;

/* Digital trigger threshold: a trigger counts as pressed at >= this value,
 * matching the Meta runtime's default boolean action threshold (0.75). */
#define XR_INPUT_TRIGGER_THRESHOLD 0.75f

/* Translate one OpenXR reading into the engine snapshot contract.
 * Pure, total: no device, no globals. */
void xr_input_to_snapshot(const xr_input_reading *in, uint64_t sequence,
                          void *out_snapshot);

/* Convert an analog trigger value to the digital button state using the
 * threshold above. Pure. */
bool xr_input_trigger_pressed(float v);

/* Haptic pulse request: maps a (intensity, sharpness) impact from the game
 * onto an OpenXR haptic Vibration action's amplitude/frequency. The OpenXR
 * vibration action takes amplitude [0,1] and frequency Hz [0,~400 on Quest];
 * the game's sharpness is a 0..1 timbre. Pure; clamped, never fails.
 * Returns 0 on success; -1 when inputs are out of domain (caller skips the
 * action). */
int xr_input_haptic_pulse(float intensity, float sharpness,
                          float *out_amplitude, float *out_frequency_hz);

#endif /* HALO_OPENXR_INPUT_H */
