/* Report what the impact detector makes of a real captured game mix.
 *
 * Diagnostic, not a pass/fail test: it prints the event count and when the
 * events land, so a run can be judged against what was happening on screen.
 * Produce the input with HALO_AUDIO_CAPTURE_WAV and HALO_AUDIO_CAPTURE_SECONDS.
 *
 * clang -O2 -I native/EngineHost native/EngineHost/tests/haptics_from_capture.c \
 *   native/EngineHost/haptics.c native/EngineHost/halo_settings.c -lm -o /tmp/halo-haptics-wav
 * /tmp/halo-haptics-wav <mix.wav>
 */

#include "haptics.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { RATE = 48000, CHUNK = 512 };
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 44, SEEK_SET);
    long frames = (n - 44) / 4;
    short *pcm = malloc((size_t)frames * 4);
    frames = (long)fread(pcm, 4, (size_t)frames, f); fclose(f);
    printf("%.1f seconds of captured game audio\n", (double)frames / RATE);
    float buffer[CHUNK*2];
    int events = 0, per10[512]; memset(per10, 0, sizeof per10);
    for (long base = 0; base + CHUNK <= frames; base += CHUNK) {
        for (int i = 0; i < CHUNK; ++i) {
            buffer[2*i]   = pcm[2*(base+i)]   / 32768.f;
            buffer[2*i+1] = pcm[2*(base+i)+1] / 32768.f;
        }
        halo_haptics_observe(buffer, CHUNK, RATE);
        float intensity, sharpness;
        if (halo_haptics_take(&intensity, &sharpness)) {
            events++;
            int bucket = (int)((base / (double)RATE) / 10);
            if (bucket < 512) per10[bucket]++;
        }
    }
    printf("haptic events: %d\n", events);
    for (int i = 0; i * 10 < frames / RATE + 10; ++i)
        if (per10[i]) printf("  %3d-%3d s : %d\n", i*10, i*10+10, per10[i]);
    free(pcm);
    return 0;
}
