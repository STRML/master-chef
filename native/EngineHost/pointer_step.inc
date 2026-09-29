/* The gaze pointer's servo step, shared by pointer.c and its test.
 *
 * The engine accelerates the mouse: the desktop probe measured a delta of d
 * counts moving the cursor d pixels up to about five, and d + 0.047 d^2
 * beyond (8 -> 11, 16 -> 28, 32 -> 83, 64 -> 268, 96 -> 520). A plain
 * proportional step therefore rings between the panel's edges. The step is
 * planned in pixels and converted through the inverse of that curve. */
static int host_pointer_counts_for(int pixels) {
    const float k = 0.047f;
    float p = (float)(pixels < 0 ? -pixels : pixels);
    float d = p <= 5.f ? p : (-1.f + sqrtf(1.f + 4.f * k * p)) / (2.f * k);
    int counts = (int)lroundf(d);
    return pixels < 0 ? -counts : counts;
}

void host_pointer_step(int cursor_x, int cursor_y, int target_x, int target_y, int *dx, int *dy) {
    /* Seven tenths of the remaining distance a frame, in pixels: a glance
     * across the panel lands in three frames, a small correction at once.
     * Capped so a jump reads as a short sweep, not a teleport. */
    const float gain = 0.7f; const int cap = 300;
    int mx = (int)lroundf((float)(target_x - cursor_x) * gain);
    int my = (int)lroundf((float)(target_y - cursor_y) * gain);
    if (mx > cap) mx = cap;
    if (mx < -cap) mx = -cap;
    if (my > cap) my = cap;
    if (my < -cap) my = -cap;
    *dx = host_pointer_counts_for(mx); *dy = host_pointer_counts_for(my);
}
