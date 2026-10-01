/* aaudio_output.h - AAudio device sink for the DirectSound shim's AudioQueue
 * facade (fake_audioqueue.h).
 *
 * The Android headless build links android/directsound_stub.c, which delivers
 * buffers to the client callback on a wall-clock cadence and discards the
 * mixed PCM. The Quest 3 device build (HALO_AAUDIO_OUTPUT) links this file
 * instead: the same facade API, the same callback protocol, the same threading
 * contract, with the buffer's PCM written to the device through an AAudio
 * stream. The AAudio stream's own blocking write supplies the cadence, so
 * directsound.c's mixer, play cursors, watchdog and diagnostics run
 * unchanged.
 *
 * Requires API 26+ (AAudio); the build targets API 29.
 */
#ifndef HALO_AAUDIO_OUTPUT_H
#define HALO_AAUDIO_OUTPUT_H

#include "fake_audioqueue.h"

/* Pure format negotiation, portable and unit-tested on the host:
 * maps the AudioStreamBasicDescription the shim asks for onto the AAudio
 * sample format constant. Returns 0 on success and writes the AAudio format
 * (AAUDIO_FORMAT_PCM_FLOAT) to *out_format; returns -1 (kAudio_ParamError)
 * for any format the sink does not support (packed 32-bit float, 1 or 2
 * channels only). */
int aaudio_output_map_format(const AudioStreamBasicDescription *asbd,
                             int *out_format);

/* Frames a buffer of mAudioDataByteSize bytes carries in the negotiated
 * format (stereo float32 -> bytes/8). Pure; unit-tested. */
uint32_t aaudio_output_buffer_frames(const AudioStreamBasicDescription *asbd,
                                      UInt32 byte_size);

/* Maps an AAudio stream result to the facade's OSStatus space. The shim
 * treats any non-zero value as an enqueue failure that arms the watchdog
 * rebuild; a disconnected stream maps to a distinct negative code so the
 * device report can name the cause. Pure; unit-tested. */
#define AAUDIO_OUTPUT_ERR_DISCONNECTED (-10868) /* 'dscn' */
int aaudio_output_status_for(int aaudio_result);

#endif /* HALO_AAUDIO_OUTPUT_H */
