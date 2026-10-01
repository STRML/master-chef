/* Host unit tests for the AAudio sink's pure helpers (Phase 5 audio).
 *
 * aaudio_output.c guards its entire device implementation on __ANDROID__
 * (the AAudio calls) and its SDK asserts too, so on macOS the file
 * compiles to just the three pure functions plus the facade types — no
 * stub AAudio symbols needed. These tests pin the format negotiation,
 * frame accounting, and status mapping the device build relies on; the
 * SDK constant mapping (AAUDIO_FORMAT_PCM_FLOAT==2, DISCONNECTED==-899,
 * INVALID_STATE==-895) is asserted at device compile time inside
 * aaudio_output.c itself.
 *
 * Run:
 *   cc -std=c11 -Wall -Wextra -I../android -I../../EngineReuse \
 *      test_aaudio_output.c ../android/aaudio_output.c -o /tmp/test_aaudio_output
 *   /tmp/test_aaudio_output
 */
#include "../android/aaudio_output.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* The ASBD the shim builds in audio_prepare_locked (directsound.c):
 * packed float32 stereo at 48 kHz. */
static AudioStreamBasicDescription stereo_f32_48k(void) {
    AudioStreamBasicDescription a;
    a.mSampleRate = 48000.0;
    a.mFormatID = kAudioFormatLinearPCM;
    a.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    a.mBytesPerPacket = 8;
    a.mFramesPerPacket = 1;
    a.mBytesPerFrame = 8;
    a.mChannelsPerFrame = 2;
    a.mBitsPerChannel = 32;
    a.mReserved = 0;
    return a;
}

static void test_map_format_accepts_float_pcm(void) {
    AudioStreamBasicDescription a = stereo_f32_48k();
    int fmt = -12345;
    assert(aaudio_output_map_format(&a, &fmt) == 0);
    assert(fmt == 2); /* AAUDIO_FORMAT_PCM_FLOAT */

    a = stereo_f32_48k(); a.mChannelsPerFrame = 1; /* mono is fine */
    assert(aaudio_output_map_format(&a, &fmt) == 0 && fmt == 2);

    a = stereo_f32_48k(); a.mSampleRate = 44100.0;
    assert(aaudio_output_map_format(&a, &fmt) == 0 && fmt == 2);
}

static void test_map_format_rejects_everything_else(void) {
    AudioStreamBasicDescription a; int fmt;
    assert(aaudio_output_map_format(NULL, &fmt) == -1);
    a = stereo_f32_48k();
    assert(aaudio_output_map_format(&a, NULL) == -1);

    a = stereo_f32_48k(); a.mFormatID = 0x6D703464u; /* 'mp4d' */
    assert(aaudio_output_map_format(&a, &fmt) == -1);

    a = stereo_f32_48k(); a.mFormatFlags = kAudioFormatFlagIsPacked; /* int16 */
    assert(aaudio_output_map_format(&a, &fmt) == -1);

    a = stereo_f32_48k(); a.mFormatFlags = kAudioFormatFlagIsFloat; /* unpacked */
    assert(aaudio_output_map_format(&a, &fmt) == -1);

    a = stereo_f32_48k(); a.mBitsPerChannel = 64; /* double not supported */
    assert(aaudio_output_map_format(&a, &fmt) == -1);
    a = stereo_f32_48k(); a.mBitsPerChannel = 16;
    assert(aaudio_output_map_format(&a, &fmt) == -1);

    a = stereo_f32_48k(); a.mChannelsPerFrame = 0;
    assert(aaudio_output_map_format(&a, &fmt) == -1);
    a = stereo_f32_48k(); a.mChannelsPerFrame = 3; /* > stereo rejected */
    assert(aaudio_output_map_format(&a, &fmt) == -1);

    a = stereo_f32_48k(); a.mFramesPerPacket = 2; /* VBR-shaped packet */
    assert(aaudio_output_map_format(&a, &fmt) == -1);

    a = stereo_f32_48k(); a.mSampleRate = 0.0;
    assert(aaudio_output_map_format(&a, &fmt) == -1);
    a = stereo_f32_48k(); a.mSampleRate = -1.0;
    assert(aaudio_output_map_format(&a, &fmt) == -1);
}

static void test_buffer_frames(void) {
    AudioStreamBasicDescription a = stereo_f32_48k();
    /* 960 bytes of packed stereo float32 = 120 frames = 2.5 ms @48k,
     * the shim's block size at one cadence tick. */
    assert(aaudio_output_buffer_frames(&a, 960) == 120u);
    assert(aaudio_output_buffer_frames(&a, 0) == 0u);
    /* Trailing partial frame is dropped, never rounded up (writing a
     * frame's worth of stale tail to the device is a glitch). */
    assert(aaudio_output_buffer_frames(&a, 963) == 120u);

    /* mBytesPerFrame missing: derive from channels * 4 (float32). */
    a = stereo_f32_48k(); a.mBytesPerFrame = 0;
    assert(aaudio_output_buffer_frames(&a, 800) == 100u);
    a = stereo_f32_48k(); a.mBytesPerFrame = 0; a.mChannelsPerFrame = 1;
    assert(aaudio_output_buffer_frames(&a, 800) == 200u);
    /* Degenerate: no frame size at all -> 0, not a div-by-zero. */
    a = stereo_f32_48k(); a.mBytesPerFrame = 0; a.mChannelsPerFrame = 0;
    assert(aaudio_output_buffer_frames(&a, 800) == 0u);
}

static void test_status_for(void) {
    /* Success and the frame-count return of a healthy write map to 0. */
    assert(aaudio_output_status_for(0) == 0);
    assert(aaudio_output_status_for(480) == 0);
    /* Disconnected + the illegal-state report of a dead stream are named
     * with the distinct code so directsound.c's watchdog rebuild can. */
    assert(aaudio_output_status_for(-899) == AAUDIO_OUTPUT_ERR_DISCONNECTED);
    assert(aaudio_output_status_for(-895) == AAUDIO_OUTPUT_ERR_DISCONNECTED);
    assert(AAUDIO_OUTPUT_ERR_DISCONNECTED == -10868);
    /* Every other negative passes through unchanged: the shim only treats
     * non-zero as "enqueue failed", the value is for the device report. */
    assert(aaudio_output_status_for(-1) == -1);
    assert(aaudio_output_status_for(-890) == -890);
    assert(aaudio_output_status_for(-9999) == -9999);
}

int main(void) {
    test_map_format_accepts_float_pcm();
    test_map_format_rejects_everything_else();
    test_buffer_frames();
    test_status_for();
    printf("test_aaudio_output: all assertions passed\n");
    return 0;
}
