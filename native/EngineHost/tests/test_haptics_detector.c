/* Does the impact detector fire on impacts and stay quiet on music?
 *
 * Halo has no force feedback of its own, so pulses are synthesised from
 * low-frequency transients in the mixed output. This drives the real observer
 * with 48 kHz stereo in 512-frame callback buffers, the same shape the
 * AudioQueue delivers.
 *
 * clang -O2 -I native/EngineHost native/EngineHost/tests/test_haptics_detector.c \
 *   native/EngineHost/haptics.c native/EngineHost/halo_settings.c -lm -o /tmp/halo-haptics
 */

#include "haptics.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RATE = 48000, CHUNK = 512 };
static int failures;
static void check(int ok, const char *what) {
    printf("%-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) failures++;
}

/* Steady low-end music plus a thump every second. */
static int run(double seconds, const double *impacts, int impact_count,
               double music_level, double *first_event_offset) {
    int events = 0; double best = -1;
    uint32_t total = (uint32_t)(seconds*RATE);
    float *buffer = malloc(CHUNK*2u*sizeof(float));
    for (uint32_t base = 0; base + CHUNK <= total; base += CHUNK) {
        for (uint32_t i = 0; i < CHUNK; ++i) {
            double t = (double)(base+i)/RATE;
            /* A 70 Hz bed stands in for music that sits in the haptic band. */
            double s = music_level*sin(2.0*M_PI*70.0*t);
            for (int k = 0; k < impact_count; ++k) {
                double dt = t - impacts[k];
                if (dt >= 0 && dt < 0.15)
                    s += 0.9*exp(-dt*26.0)*sin(2.0*M_PI*55.0*dt);
            }
            if (s > 1) s = 1; if (s < -1) s = -1;
            buffer[2u*i] = buffer[2u*i+1u] = (float)s;
        }
        halo_haptics_observe(buffer, CHUNK, RATE);
        float intensity, sharpness;
        if (halo_haptics_take(&intensity, &sharpness)) {
            events++;
            double now = (double)(base+CHUNK)/RATE;
            for (int k = 0; k < impact_count && best < 0; ++k)
                if (now >= impacts[k] && now - impacts[k] < 0.3) best = now - impacts[k];
        }
    }
    free(buffer);
    if (first_event_offset) *first_event_offset = best;
    return events;
}

int main(void) {
    double impacts[6] = { 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 }, latency = -1;
    int fired = run(9.0, impacts, 6, 0.10, &latency);
    printf("  six impacts over a music bed produced %d events, first within %.0f ms\n",
           fired, latency*1000.0);
    check(fired >= 6, "every impact produces at least one event");
    check(latency >= 0 && latency < 0.05, "an event lands within one buffer of its impact");

    int idle = run(9.0, NULL, 0, 0.10, NULL);
    printf("  steady music alone produced %d events\n", idle);
    check(idle <= 1, "steady music alone does not buzz");

    int silent = run(9.0, NULL, 0, 0.0, NULL);
    printf("  silence produced %d events\n", silent);
    check(silent == 0, "silence produces nothing");

    printf("%s (%d failing)\n", failures ? "FAILURES" : "ALL PASS", failures);
    return failures ? 1 : 0;
}
