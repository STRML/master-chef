/* gamecontroller_stub.c - no-controller stand-in for the Apple GameController
 * bridge. dinput8.c and the d3d9 pad-to-keys path call these; headless
 * reports no pad (poll returns false, connected returns false) so the game
 * falls back to keyboard/mouse input, which the DirectInput shim still
 * services from host_keyboard_state. Phase 5 replaces this with the OpenXR
 * action backend. */
#include "gamecontroller.h"
#include <string.h>

void hostgc_init(void) { }

bool hostgc_poll(HostGCSnapshot *out) {
    if (out) memset(out, 0, sizeof *out);
    return false;
}

bool hostgc_connected(void) { return false; }

void hostgc_poll_stats(uint64_t *captures, uint64_t *reuses, uint32_t *window_us) {
    if (captures) *captures = 0;
    if (reuses) *reuses = 0;
    if (window_us) *window_us = 0;
}

void hostgc_inject_test_snapshot(const HostGCSnapshot *snap) { (void)snap; }
void hostgc_clear_test_snapshot(void) { }

void hostgc_play_haptic(float intensity, float sharpness) {
    (void)intensity; (void)sharpness;
}
const char *hostgc_haptic_state(void) { return "no controller (headless)"; }
uint64_t hostgc_haptics_played(void) { return 0; }

void hostgc_link_stats(HostGCLinkStats *out) {
    if (out) memset(out, 0, sizeof *out);
}
