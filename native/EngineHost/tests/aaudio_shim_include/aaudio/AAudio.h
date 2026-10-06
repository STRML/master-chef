/* aaudio/AAudio.h - minimal host-test stub for the NDK's AAudio surface.
 * Only the types, constants and prototypes used by
 * native/EngineHost/android/aaudio_output.c are declared, with the NDK's
 * exact enum values (aaudio_output.c static_asserts three of them).
 * Used by tests/test_aaudio_dispose_race.c to compile the real device sink
 * on macOS under -D__ANDROID__ with a fake backend. Do not ship. */
#ifndef HALO_TEST_AAUDIO_STUB_H
#define HALO_TEST_AAUDIO_STUB_H

#include <stdint.h>

typedef int32_t aaudio_result_t;
typedef int32_t aaudio_format_t;
typedef int32_t aaudio_sharing_mode_t;
typedef int32_t aaudio_performance_mode_t;
typedef int32_t aaudio_usage_t;

enum {
    AAUDIO_OK = 0,
    AAUDIO_FORMAT_UNSPECIFIED = 0,
    AAUDIO_FORMAT_PCM_I16 = 1,
    AAUDIO_FORMAT_PCM_FLOAT = 2,
    AAUDIO_SHARING_MODE_EXCLUSIVE = 0,
    AAUDIO_PERFORMANCE_MODE_LOW_LATENCY = 10,
    AAUDIO_USAGE_MEDIA = 1,
    AAUDIO_ERROR_TIMEOUT = -892,
    AAUDIO_ERROR_INVALID_STATE = -895,
    AAUDIO_ERROR_DISCONNECTED = -899
};

typedef struct AAudioStream AAudioStream;
typedef struct AAudioStreamBuilder AAudioStreamBuilder;

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **builder);
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *builder, int32_t sampleRate);
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *builder, int32_t channelCount);
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *builder, aaudio_format_t format);
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *builder, aaudio_performance_mode_t mode);
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *builder, aaudio_sharing_mode_t mode);
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *builder, aaudio_usage_t usage);
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *builder, AAudioStream **stream);
void AAudioStreamBuilder_delete(AAudioStreamBuilder *builder);
aaudio_result_t AAudioStream_requestStart(AAudioStream *stream);
aaudio_result_t AAudioStream_requestPause(AAudioStream *stream);
aaudio_result_t AAudioStream_requestStop(AAudioStream *stream);
aaudio_result_t AAudioStream_close(AAudioStream *stream);
aaudio_result_t AAudioStream_write(AAudioStream *stream, const void *audioData,
                                   int32_t numFrames, int64_t timeoutNanos);

#endif /* HALO_TEST_AAUDIO_STUB_H */
