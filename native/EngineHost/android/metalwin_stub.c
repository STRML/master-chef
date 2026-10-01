/* metalwin_stub.c - windowless stand-in for the Cocoa/Metal window.
 *
 * d3d9.c calls these on the present path. Headless: init "succeeds" so the
 * device reaches the ready state (window_ready=1), present is a no-op, and
 * should_close stays 0 so the run is bounded only by --frames. */
#include "metalwin.h"

int metalwin_init(int width, int height, const char *title) {
    (void)width; (void)height; (void)title;
    return 0;
}
void metalwin_present(const void *bgra, int width, int height) {
    (void)bgra; (void)width; (void)height;
}
void metalwin_present_gpu(int slot, int width, int height) {
    (void)slot; (void)width; (void)height;
}
void metalwin_present_dropped(int width, int height) {
    (void)width; (void)height;
}
int metalwin_should_close(void) {
    return 0;
}
void metalwin_poll(void) { }
