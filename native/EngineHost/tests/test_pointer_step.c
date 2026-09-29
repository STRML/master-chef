/* The gaze pointer's servo step against the engine's measured mouse
 * acceleration: converges on the target from any start in a few frames,
 * never rings, and keeps converging if the curve is a fifth off.
 *
 * clang -O2 native/EngineHost/tests/test_pointer_step.c -lm -o /tmp/halo-pointer && /tmp/halo-pointer
 */
#include "../pointer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* Only the pure step is under test; the servo's globals are not linked. */
#include "../pointer_step.inc"

static int failures;
static void check(int ok, const char *what) {
    printf("%-66s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}
/* What the engine does with a delta: the probe's measured curve, with a
 * different constant to stand in for a curve the probe never measured. */
static int plant(int counts, float k) {
    float d = (float)abs(counts);
    float moved = d <= 5.f ? d : d + k * d * d;
    int px = (int)lroundf(moved);
    return counts < 0 ? -px : px;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
/* Frames until the cursor sits within two pixels of the target. */
static int settle(int x0, int y0, int tx, int ty, float k, int *max_overshoot) {
    int x = x0, y = y0, worst = 0;
    for (int frame = 1; frame <= 60; frame++) {
        int dx, dy; host_pointer_step(x, y, tx, ty, &dx, &dy);
        x = clampi(x + plant(dx, k), 0, 640); y = clampi(y + plant(dy, k), 0, 480);
        int over = (tx > x0) ? x - tx : tx - x;
        if (over > worst) worst = over;
        if (abs(x - tx) <= 2 && abs(y - ty) <= 2) { if (max_overshoot) *max_overshoot = worst; return frame; }
    }
    if (max_overshoot) *max_overshoot = worst;
    return -1;
}

int main(void) {
    int over = 0;
    int frames = settle(0, 0, 479, 240, 0.047f, &over);
    printf("  corner to (479,240) on the measured curve: %d frames, overshoot %d\n", frames, over);
    check(frames > 0 && frames <= 6, "a glance across the panel lands within six frames");
    check(over <= 6, "without ringing");
    frames = settle(640, 480, 20, 20, 0.047f, &over);
    printf("  far corner back to (20,20): %d frames, overshoot %d\n", frames, over);
    check(frames > 0 && frames <= 6 && over <= 6, "and back the other way");
    frames = settle(320, 240, 330, 244, 0.047f, NULL);
    check(frames > 0 && frames <= 3, "a small correction lands at once");
    frames = settle(0, 0, 479, 240, 0.060f, &over);
    printf("  a curve a quarter steeper than measured: %d frames, overshoot %d\n", frames, over);
    check(frames > 0 && frames <= 10 && over <= 40, "still converges on a steeper curve");
    frames = settle(0, 0, 479, 240, 0.035f, &over);
    check(frames > 0 && frames <= 10, "and on a gentler one");
    int dx, dy; host_pointer_step(100, 100, 100, 100, &dx, &dy);
    check(dx == 0 && dy == 0, "on target there is no motion");
    host_pointer_step(0, 0, 639, 479, &dx, &dy);
    check(dx > 0 && dx <= 75 && dy > 0 && dy <= 75, "a full jump is at most seventy-five counts");
    printf("%s (%d failing)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
