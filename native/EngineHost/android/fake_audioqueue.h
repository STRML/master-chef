/* fake_audioqueue.h - the AudioToolbox AudioQueue API surface used by
 * directsound.c, with identical types and calling conventions, backed by a
 * silent software output for the Android headless build.
 *
 * Buffers are delivered to the callback from a dedicated thread on a
 * wall-clock cadence, so directsound.c runs its exact asynchronous
 * production path (mixer render, play cursor, watchdog, diagnostics) with
 * no hardware to talk to. Implementation: android/directsound_stub.c.
 *
 * Only the declarations directsound.c references are provided.
 */
#ifndef HALO_FAKE_AUDIOQUEUE_H
#define HALO_FAKE_AUDIOQUEUE_H

#include <stdint.h>
#include <stddef.h>

typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef uint8_t Boolean;
typedef double Float64;

#define noErr ((OSStatus)0)

/* FourCCs, matching the AudioToolbox values. */
#define kAudioFormatLinearPCM       ((UInt32)0x6C70636Du) /* 'lpcm' */
#define kAudioFormatFlagIsFloat     ((UInt32)(1u << 0))
#define kAudioFormatFlagIsPacked    ((UInt32)(1u << 3))
#define kAudioQueueProperty_IsRunning ((UInt32)0x6171726Eu) /* 'aqrn' */

typedef struct {
    Float64 mSampleRate;
    UInt32 mFormatID;
    UInt32 mFormatFlags;
    UInt32 mBytesPerPacket;
    UInt32 mFramesPerPacket;
    UInt32 mBytesPerFrame;
    UInt32 mChannelsPerFrame;
    UInt32 mBitsPerChannel;
    UInt32 mReserved;
} AudioStreamBasicDescription;

typedef struct FakeAudioQueueBuffer {
    struct FakeAudioQueueBuffer *next;     /* FIFO link, private to the queue */
    struct FakeAudioQueueBuffer *all_next; /* allocation list, private to the queue */
    void *mAudioData;
    UInt32 mAudioDataBytesCapacity;
    UInt32 mAudioDataByteSize;
    void *mUserData;
} *AudioQueueBufferRef;

typedef struct FakeAudioQueue *AudioQueueRef;

typedef void (*AudioQueueCallback)(void *inUserData, AudioQueueRef inAQ,
                                   AudioQueueBufferRef inCompleteAQBuffer);

OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *inASBD,
                             AudioQueueCallback inCallback, void *inCallbackUserData,
                             void *inCallbackRunLoop, const void *inCallbackRunLoopMode,
                             UInt32 inFlags, AudioQueueRef *outAQ);
OSStatus AudioQueueAllocateBuffer(AudioQueueRef inAQ, UInt32 inBufferRefSize,
                                  AudioQueueBufferRef *outBuffer);
OSStatus AudioQueueEnqueueBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer,
                                 UInt32 inNumberParams, const void *inParams);
OSStatus AudioQueueStart(AudioQueueRef inAQ, const void *inStartTime);
OSStatus AudioQueuePause(AudioQueueRef inAQ);
OSStatus AudioQueueDispose(AudioQueueRef inAQ, Boolean inImmediate);
OSStatus AudioQueueGetProperty(AudioQueueRef inAQ, UInt32 inPropertyID,
                               void *outData, UInt32 *ioDataSize);

#endif /* HALO_FAKE_AUDIOQUEUE_H */
