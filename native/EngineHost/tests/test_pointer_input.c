/* Exercises the production queue + servo with Halo's measured mouse curve. */
#include "../pointer.c"
#include <assert.h>

uint8_t *engine_flat_base;
int32_t host_mouse_dx, host_mouse_dy;
uint8_t host_mouse_buttons[8];
static int menu_active = 1;
int host_dinput_menu_active(void) { return menu_active; }
int halo_settings_gaze_pointer(void) { return 0; }
static int clicks, releases, previous_down;
static int click_x[64], click_y[64];

static int plant(int d) {
    float a = (float)abs(d);
    int moved = (int)lroundf(a <= 5 ? a : a + 0.047f * a * a);
    return d < 0 ? -moved : moved;
}
static int clamp(int v, int high) { return v < 0 ? 0 : v > high ? high : v; }
static void frame(void) {
    host_pointer_servo();
    int x = G32(CURSOR_X_ADDRESS), y = G32(CURSOR_Y_ADDRESS);
    int down = host_mouse_buttons[0] != 0;
    if (down && !previous_down) { click_x[clicks] = x; click_y[clicks++] = y; }
    if (!down && previous_down) releases++;
    previous_down = down;
    S32(CURSOR_X_ADDRESS, clamp(x + plant(host_mouse_dx), 640));
    S32(CURSOR_Y_ADDRESS, clamp(y + plant(host_mouse_dy), 480));
    host_mouse_dx = host_mouse_dy = 0;
}
static void reset(void) {
    host_pointer_set(-1, -1, 0, HOST_POINTER_CANCEL);
    frame();
    clicks = releases = previous_down = 0;
    S32(CURSOR_X_ADDRESS, 0); S32(CURSOR_Y_ADDRESS, 0);
}
static void run_frames(int n) { for (int i = 0; i < n; i++) frame(); }
static void at(int index, int x, int y) {
    assert(abs(click_x[index] - x) + abs(click_y[index] - y) <= 6);
}
int main(void) {
    engine_flat_base = calloc(1, CURSOR_Y_ADDRESS + 4);
    assert(engine_flat_base);
    reset();
    host_pointer_set(1, 1, 1, HOST_POINTER_TAP);
    host_pointer_set(0, 0, 1, HOST_POINTER_HOVER);
    run_frames(30);
    assert(clicks == 1 && releases == 1); at(0, 639, 479);
    puts("A fast tap survives release/hover and waits for the intended target");
    reset();
    host_pointer_set(1, 1, 1, HOST_POINTER_TAP);
    host_pointer_set(0, 0, 1, HOST_POINTER_TAP);
    host_pointer_set(0.5f, 0.5f, 1, HOST_POINTER_TAP);
    run_frames(60);
    assert(clicks == 3 && releases == 3);
    at(0, 639, 479); at(1, 0, 0); at(2, 320, 240);
    puts("Rapid taps preserve separate target, down, and up transitions");
    reset();
    host_pointer_set(1, 1, 1, HOST_POINTER_TAP);
    frame();
    host_pointer_set(-1, -1, 0, HOST_POINTER_CANCEL);
    run_frames(30); assert(clicks == 0);
    host_pointer_set(0, 0, 1, HOST_POINTER_TAP);
    while (!clicks) frame();
    host_pointer_set(-1, -1, 0, HOST_POINTER_CANCEL);
    frame(); assert(releases == 1 && host_mouse_buttons[0] == 0);
    puts("Lifecycle cancellation drops pending clicks and releases delivered clicks");
    reset();
    host_pointer_set(1, 1, 1, HOST_POINTER_TAP);
    menu_active = 0; frame(); menu_active = 1;
    run_frames(30); assert(clicks == 0 && G32(CURSOR_X_ADDRESS) == 0);
    host_pointer_set(0, 0, 1, HOST_POINTER_TAP); frame(); assert(clicks == 1);
    menu_active = 0; frame(); assert(releases == 1);
    /* Gameplay state owned by the controller is not overwritten once released. */
    host_mouse_buttons[0] = 0x80; frame(); assert(host_mouse_buttons[0] == 0x80);
    host_mouse_buttons[0] = 0; menu_active = 1;
    puts("Leaving menus clears pending input and preserves controller ownership");
    reset();
    host_pointer_set(NAN, 0, 1, HOST_POINTER_TAP);
    host_pointer_set(INFINITY, 0, 1, HOST_POINTER_TAP);
    host_pointer_set(0, 0, 0, HOST_POINTER_TAP);
    run_frames(10); assert(clicks == 0);
    for (int i = 0; i < 100; i++) host_pointer_set(0, 0, 1, HOST_POINTER_TAP);
    run_frames(60); assert(clicks == POINTER_TAP_CAPACITY && releases == POINTER_TAP_CAPACITY);
    puts("Invalid targets cannot click and pending taps are bounded");
    free(engine_flat_base);
    puts("Pointer event queue passed");
}
