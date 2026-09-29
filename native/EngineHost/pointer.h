#ifndef HALO_POINTER_H
#define HALO_POINTER_H
#include <stdint.h>
/* The gaze pointer: look at a menu item and the original engine's own cursor
 * goes there; pinch, or press the pad's Cross, and it is chosen.
 *
 * Halo's front-end and pause menus follow a DirectInput mouse. The headset
 * has no mouse, but it knows where the viewer is looking, and the engine
 * keeps its cursor's position in two globals the desktop probe located by
 * moving a mouse and watching guest memory. Each
 * frame the menu is up, the host reads that position, compares it with
 * where the gaze meets the menu panel, and feeds the difference to the
 * engine as mouse motion, so the cursor converges on the gaze whatever the
 * engine's own mouse scaling is. In play the pointer does nothing: the
 * mouse is the player's aim there and stays with the controller. */

/* The system only reveals a gaze selection ray during an interaction.
 * HOVER moves the preview, TAP queues a completed pinch's single left-click,
 * CANCEL discards pending taps and releases the pointer on the engine thread.
 * The target is normalized (0,0 top left). A queued tap retains its own target
 * until the cursor arrives; later hover samples cannot erase or redirect it. */
enum { HOST_POINTER_CANCEL = -1, HOST_POINTER_HOVER = 0, HOST_POINTER_TAP = 2 };
void host_pointer_set(float u, float v, int on_panel, int action);
/* Engine thread, once per presented frame, before DirectInput is read. */
void host_pointer_servo(void);
/* The mouse motion that moves the cursor from where it is toward the
 * target, this frame. Pure, for the test: cursor and target in the same
 * pixel space; the step is a fraction of the remaining distance, capped, so
 * the loop converges for any engine-side scaling under about three. */
void host_pointer_step(int cursor_x, int cursor_y, int target_x, int target_y, int *dx, int *dy);
/* Whether optional head-follow preview is enabled; system taps remain enabled. */
int host_pointer_enabled(void);
/* For the report: frames the servo ran and the last cursor it read. */
void host_pointer_stats(uint64_t *frames, int *cursor_x, int *cursor_y);
#endif
