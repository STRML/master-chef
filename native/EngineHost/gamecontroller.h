/* Apple GameController -> DirectInput bridge for the native Halo host.
 *
 * Provides a controller-agnostic snapshot of the "extended gamepad" profile
 * (works for the PS5 DualSense, Xbox, MFi, etc. on macOS and visionOS) that the
 * DirectInput 8 joystick shim (dinput8.c) turns into DIJOYSTATE-style reports.
 *
 * The snapshot is deliberately API-neutral: dinput8.c never touches any Apple
 * type, so it stays plain C and unit-testable, while all the ObjC/GameController
 * code lives in gamecontroller.m.  Tests may bypass the hardware entirely by
 * calling hostgc_inject_test_snapshot().
 */
#ifndef HALO_GAMECONTROLLER_H
#define HALO_GAMECONTROLLER_H
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Play one impact on whatever pad the host is already reading.
 *
 * Haptics used to be driven from the Swift side, which looked the controller
 * up for itself. That is a second, independent piece of controller plumbing
 * that can be empty at a moment when the host's own is not, and on the device
 * it reported no controller at all while impacts were being detected. Driving
 * the pad from here means that whenever the game can be played, it can be
 * felt: the same object answers both questions.
 *
 * Safe from any thread. Does nothing, quietly, when no pad is attached or the
 * pad has no haptics. Intensity and sharpness are clamped to 0...1. */
void hostgc_play_haptic(float intensity, float sharpness);
/* What the haptics path would say for itself, for the device report:
 * "ready (DualSense)", "no controller", "no haptics on this pad", and so on.
 * The snapshot remains valid until the next call on the same thread. */
const char *hostgc_haptic_state(void);
/* How many pulses Core Haptics accepted for playback since launch. */
uint64_t hostgc_haptics_played(void);

/* The controller link and the haptics guard (gamecontroller_guard.h), for the
 * device report. */
typedef struct {
    uint64_t connects, disconnects, linked_disconnects, suspensions;
    uint64_t pulses_admitted, pulses_dropped_rate, pulses_dropped_grace,
             pulses_dropped_suspended, pulses_dropped_disconnected;
    uint32_t suspended_ms_left;
    int32_t guard_enabled;
    uint64_t haptic_failures, haptic_resets, haptic_stops;
    uint64_t last_connect_ns, last_disconnect_ns, last_played_ns;
} HostGCLinkStats;
void hostgc_link_stats(HostGCLinkStats *out);

/* Logical buttons, in a stable order.  The index is what dinput8.c reports as
 * DirectInput button N (rgbButtons[N]).  This ordering matches the classic
 * "XInput-style / Halo PC" pad expectation closely enough for movement, aim,
 * fire, grenade, jump, reload, melee and menu to all be reachable. */
enum {
    HOSTGC_BTN_A = 0,        /* cross      */
    HOSTGC_BTN_B,            /* circle     */
    HOSTGC_BTN_X,            /* square     */
    HOSTGC_BTN_Y,            /* triangle   */
    HOSTGC_BTN_LSHOULDER,    /* L1         */
    HOSTGC_BTN_RSHOULDER,    /* R1         */
    HOSTGC_BTN_LTRIGGER,     /* L2 (digital view of the analog trigger) */
    HOSTGC_BTN_RTRIGGER,     /* R2 (digital view of the analog trigger) */
    HOSTGC_BTN_LTHUMB,       /* L3         */
    HOSTGC_BTN_RTHUMB,       /* R3         */
    HOSTGC_BTN_MENU,         /* Options / Start */
    HOSTGC_BTN_OPTIONS,      /* Create/Share / Back */
    HOSTGC_BTN_HOME,         /* PS / Guide */
    HOSTGC_BTN_DPAD_UP,
    HOSTGC_BTN_DPAD_DOWN,
    HOSTGC_BTN_DPAD_LEFT,
    HOSTGC_BTN_DPAD_RIGHT,
    HOSTGC_BUTTON_COUNT
};

/* One controller reading.  Axes are normalised floats:
 *   thumbsticks: [-1, +1], +x = right, +y = UP (GameController convention)
 *   triggers:    [0, 1]
 * The dpad is exposed both as four booleans and as a DirectInput POV angle.
 */
typedef struct {
    bool  connected;             /* true when a live extended gamepad is present  */
    float lx, ly, rx, ry;        /* left/right thumbstick, [-1,+1], y up          */
    float lt, rt;                /* left/right analog trigger, [0,1]              */
    bool  dpad_up, dpad_down, dpad_left, dpad_right;
    bool  buttons[HOSTGC_BUTTON_COUNT];
    uint64_t sequence;           /* increments on every hardware poll; a reused
                                    reading keeps the sequence it was taken with */
} HostGCSnapshot;

/* Bring the GameController subsystem up.  Idempotent; safe to call from the game
 * thread.  Registers connect/disconnect observers on the current run loop. */
void hostgc_init(void);

/* Read the currently-attached extended gamepad into *out.  Returns true if a
 * controller is connected.  A hardware reading taken less than the reuse
 * window ago (HALO_PAD_REUSE_US, default 4000 us, 0 = never reuse) is handed
 * out again rather than captured anew, so the several reads one engine frame
 * makes share one capture.  When a test snapshot has been injected, that
 * snapshot is returned instead of hardware (see below).  Thread-safe. */
bool hostgc_poll(HostGCSnapshot *out);

/* For the device report: hardware captures taken, polls answered from a
 * recent capture instead, and the reuse window in microseconds. */
void hostgc_poll_stats(uint64_t *captures, uint64_t *reuses, uint32_t *window_us);

/* True if at least one extended gamepad is currently connected (or a test
 * snapshot is active and marked connected). */
bool hostgc_connected(void);

/* --- Test hooks (no hardware required) --------------------------------------
 * Inject a snapshot that hostgc_poll() will return verbatim until cleared.
 * This lets unit tests drive the DirectInput mapping deterministically. */
void hostgc_inject_test_snapshot(const HostGCSnapshot *snap);
void hostgc_clear_test_snapshot(void);

#ifdef __cplusplus
}
#endif
#endif
