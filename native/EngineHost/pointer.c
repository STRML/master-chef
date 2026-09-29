#include "pointer.h"
#include "host.h"
#include "halo_settings.h"
#include <math.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Halo PC 1.10: the UI cursor's position as two 32-bit integers, located
 * by tracing mouse movement in guest memory. The
 * engine clamps them to the interface's own extent, which the same probe
 * read from the clamps; the target below is mapped into that extent. */
enum { CURSOR_X_ADDRESS = 0x00718f84u, CURSOR_Y_ADDRESS = 0x00718f88u };
enum { UI_WIDTH = 640, UI_HEIGHT = 480 };   /* the clamps the probe read: 640 and 480 */
static const int ui_width = UI_WIDTH, ui_height = UI_HEIGHT;

/* Publish coordinates and actions together. A completed tap must outlive the
 * compositor callback and the cursor's several-frame trip to its target. */
typedef struct { int x, y, valid; } PointerTarget;
enum { POINTER_TAP_CAPACITY = 16 };
static pthread_mutex_t pointer_lock = PTHREAD_MUTEX_INITIALIZER;
static PointerTarget hover_target, taps[POINTER_TAP_CAPACITY];
static unsigned tap_head, tap_count;
static int pointer_down;
static _Atomic uint64_t servo_frames;
static _Atomic int last_cursor_x, last_cursor_y;

extern int32_t host_mouse_dx, host_mouse_dy;
extern uint8_t host_mouse_buttons[8];
int host_dinput_menu_active(void);

#include "pointer_step.inc"

/* Live, from the settings panel; HALO_GAZE_POINTER=0 seeds it off. */
int host_pointer_enabled(void) { return halo_settings_gaze_pointer(); }

void host_pointer_set(float u, float v, int on_panel, int action) {
    PointerTarget target = {0};
    if (on_panel && isfinite(u) && isfinite(v)) {
        target.x = (int)lroundf(fmaxf(0.f, fminf(1.f, u)) * (float)(ui_width - 1));
        target.y = (int)lroundf(fmaxf(0.f, fminf(1.f, v)) * (float)(ui_height - 1));
        target.valid = 1;
    }
    pthread_mutex_lock(&pointer_lock);
    if (action == HOST_POINTER_CANCEL) {
        tap_count = tap_head = 0;
        hover_target.valid = 0;
    } else {
        hover_target = target;
        if (action == HOST_POINTER_TAP && target.valid && tap_count < POINTER_TAP_CAPACITY) {
            taps[(tap_head + tap_count) % POINTER_TAP_CAPACITY] = target;
            tap_count++;
        }
    }
    pthread_mutex_unlock(&pointer_lock);
}

void host_pointer_servo(void) {
    pthread_mutex_lock(&pointer_lock);
    /* Never carry pending menu input into gameplay or the next menu. The
     * controller continues to own gameplay aiming and firing. */
    if (!host_dinput_menu_active()) {
        tap_count = tap_head = 0; hover_target.valid = 0;
        if (pointer_down) { host_mouse_buttons[0] = 0; pointer_down = 0; }
        pthread_mutex_unlock(&pointer_lock);
        return;
    }
    int cursor_x = (int32_t)G32(CURSOR_X_ADDRESS), cursor_y = (int32_t)G32(CURSOR_Y_ADDRESS);
    atomic_store_explicit(&last_cursor_x, cursor_x, memory_order_relaxed);
    atomic_store_explicit(&last_cursor_y, cursor_y, memory_order_relaxed);
    atomic_fetch_add_explicit(&servo_frames, 1u, memory_order_relaxed);
    /* One engine frame down, then one frame up, even when the next tap is
     * already queued. Do not move to its target on this release frame. */
    if (pointer_down) {
        host_mouse_buttons[0] = 0; pointer_down = 0;
        pthread_mutex_unlock(&pointer_lock);
        return;
    }
    PointerTarget target = tap_count ? taps[tap_head] : hover_target;
    if (target.valid) {
        int arrived = abs(target.x - cursor_x) + abs(target.y - cursor_y) <= 6;
        if (tap_count && arrived) {
            host_mouse_buttons[0] = 0x80; pointer_down = 1;
            tap_head = (tap_head + 1) % POINTER_TAP_CAPACITY; tap_count--;
        } else {
            int dx, dy; host_pointer_step(cursor_x, cursor_y, target.x, target.y, &dx, &dy);
            host_mouse_dx += dx; host_mouse_dy += dy;
        }
    }
    pthread_mutex_unlock(&pointer_lock);
}

void host_pointer_stats(uint64_t *frames, int *cursor_x, int *cursor_y) {
    if (frames) *frames = atomic_load_explicit(&servo_frames, memory_order_relaxed);
    if (cursor_x) *cursor_x = atomic_load_explicit(&last_cursor_x, memory_order_relaxed);
    if (cursor_y) *cursor_y = atomic_load_explicit(&last_cursor_y, memory_order_relaxed);
}
