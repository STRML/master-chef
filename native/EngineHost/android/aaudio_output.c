/* aaudio_output.c - AAudio device sink for the DirectSound shim's AudioQueue
 * facade; the Quest 3 device build of the native Halo host.
 *
 * Implements the same contract as android/directsound_stub.c (see
 * fake_audioqueue.h): the client's audio_callback receives a completed
 * buffer, renders the mixer's next block into it, and re-enqueues it. The
 * stub paces that loop with wall-clock sleeps and discards the PCM; here the
 * loop is paced by the device itself: the worker writes the callback's
 * freshly filled buffer to an AAudio stream, and AAudioStream_write blocks
 * while the stream's ring is full, which is exactly the cadence a real
 * output device imposes.
 *
 * Stream choice: NONE sharing mode, LOW_LATENCY performance, the shim's
 * stereo float32 @ its output rate, no AAudio data callback - the callback
 * stays in directsound.c, the single mixer the port must not fork.
 *
 * Failure: a write to a disconnected stream makes every later enqueue fail
 * with AAUDIO_OUTPUT_ERR_DISCONNECTED, which is what directsound.c's
 * watchdog reacts to (it disposes the queue and rebuilds). Dispose stops and
 * closes the stream and joins the worker, with no lock held while the
 * client callback runs - the stub's threading contract, unchanged.
 *
 * Requires API 26+; the build targets API 29 (aarch64-linux-android29-clang).
 * The pure helpers (format negotiation, frame counting, status mapping) are
 * portable and unit-tested on the host: native/EngineHost/tests/
 * test_aaudio_output.c. */
#include "aaudio_output.h"
#include "engine_compat_android.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <aaudio/AAudio.h>
/* The facade's format constants mirror AudioToolbox; assert the AAudio
 * mapping the host tests pin down against the real SDK values. */
_Static_assert(AAUDIO_FORMAT_PCM_FLOAT == 2, "aaudio float format moved");
_Static_assert(AAUDIO_ERROR_DISCONNECTED == -899, "aaudio disconnect code moved");
_Static_assert(AAUDIO_ERROR_INVALID_STATE == -895, "aaudio invalid-state code moved");
#endif

/* --- Pure helpers (no device I/O; host-unit-tested) --------------------- */

int aaudio_output_map_format(const AudioStreamBasicDescription *asbd,
                             int *out_format) {
    if (!asbd || !out_format) return -1; /* kAudio_ParamError */
    /* The shim only ever asks for packed 32-bit float, 1 or 2 channels,
     * at a fixed positive rate (see audio_prepare_locked in directsound.c). */
    if (asbd->mFormatID != kAudioFormatLinearPCM) return -1;
    if (!(asbd->mFormatFlags & kAudioFormatFlagIsFloat)) return -1;
    if (!(asbd->mFormatFlags & kAudioFormatFlagIsPacked)) return -1;
    if (asbd->mBitsPerChannel != 32) return -1;
    if (asbd->mChannelsPerFrame < 1 || asbd->mChannelsPerFrame > 2) return -1;
    if (asbd->mFramesPerPacket != 1) return -1;
    if (asbd->mSampleRate <= 0.0) return -1;
    *out_format = 2; /* AAUDIO_FORMAT_PCM_FLOAT */
    return 0;
}

uint32_t aaudio_output_buffer_frames(const AudioStreamBasicDescription *asbd,
                                      UInt32 byte_size) {
    UInt32 bytes_per_frame = asbd->mBytesPerFrame;
    if (!bytes_per_frame) bytes_per_frame = asbd->mChannelsPerFrame * 4u;
    if (!bytes_per_frame) return 0;
    return byte_size / bytes_per_frame;
}

int aaudio_output_status_for(int aaudio_result) {
    if (aaudio_result >= 0) return 0;
    /* Disconnection (and the illegal-state report of a dead stream) is the
     * failure the shim's watchdog must be able to name. */
    if (aaudio_result == -899 /* AAUDIO_ERROR_DISCONNECTED */ ||
        aaudio_result == -895 /* AAUDIO_ERROR_INVALID_STATE */)
        return AAUDIO_OUTPUT_ERR_DISCONNECTED;
    return aaudio_result;
}

/* --- Device implementation (Android only) --------------------------------- */
#ifdef __ANDROID__

struct FakeAudioQueue {
    AudioQueueCallback callback;
    void *user;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    int thread_started;
    int quit;
    int running;
    AudioQueueBufferRef head, tail, all;
    AudioStreamBasicDescription format; /* the negotiated stream format */
    AAudioStream *stream;
    int disconnected;
};

/* The worker owns the buffer loop: pop a completed buffer, hand it to the
 * client callback (which renders the mixer's next block into mAudioData and
 * re-enqueues it via AudioQueueEnqueueBuffer), then write exactly the
 * callback's frames to the device. AAudioStream_write blocks while the
 * stream ring is full, so the hardware consumes the queue at the cadence
 * the stub only simulated. */
static void *aaudio_output_worker(void *arg) {
    struct FakeAudioQueue *q = (struct FakeAudioQueue *)arg;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->quit && !q->head)
            pthread_cond_wait(&q->cond, &q->lock);
        if (q->quit) { pthread_mutex_unlock(&q->lock); break; }
        AudioQueueBufferRef b = q->head;
        q->head = b->next;
        if (!q->head) q->tail = NULL;
        int running = q->running, dead = q->disconnected;
        AudioStreamBasicDescription asbd = q->format;
        pthread_mutex_unlock(&q->lock);

        /* Client callback runs with no queue lock held: it may call
         * AudioQueueEnqueueBuffer (and the watchdog may Dispose), exactly
         * as on the stub and on the real AudioQueue. */
        if (!running) {
            /* Paused: park the buffer at the tail so Start re-arms it; no
             * callback, no device write, like the stub. Re-check quit/dispose
             * under the lock: Dispose may have completed while we were
             * unlocked (its join cannot see us until this line), in which
             * case b and the queue internals are already freed. */
            pthread_mutex_lock(&q->lock);
            if (q->quit || q->disconnected) {
                pthread_mutex_unlock(&q->lock);
                continue;
            }
            b->next = NULL;
            if (q->tail) q->tail->next = b; else q->head = b;
            q->tail = b;
            pthread_mutex_unlock(&q->lock);
            continue;
        }
        if (q->quit || dead) continue; /* buffer drops out; Dispose frees the all list */
        /* Client callback runs with no queue lock held: it renders the
         * mixer's next block into mAudioData and re-enqueues the buffer
         * (AudioQueueEnqueueBuffer) before returning. */
        q->callback(q->user, q, b);
        UInt32 frames = aaudio_output_buffer_frames(&asbd, b->mAudioDataByteSize);
        if (!frames) continue;
        /* Blocking write: full ring paces the loop at the device rate. */
        aaudio_result_t wr = AAudioStream_write(q->stream, b->mAudioData,
                                                 (int32_t)frames,
                                                 (int64_t)1000000000 /* block <=1s */);
        if (wr < 0) {
            int st = aaudio_output_status_for((int)wr);
            if (st == AAUDIO_OUTPUT_ERR_DISCONNECTED) {
                pthread_mutex_lock(&q->lock);
                q->disconnected = 1;
                pthread_mutex_unlock(&q->lock);
            }
        }
    }
    return NULL;
}

OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *inASBD,
                             AudioQueueCallback inCallback, void *inCallbackUserData,
                             void *inCallbackRunLoop, const void *inCallbackRunLoopMode,
                             UInt32 inFlags, AudioQueueRef *outAQ) {
    (void)inCallbackRunLoop; (void)inCallbackRunLoopMode; (void)inFlags;
    if (!inASBD || !inCallback || !outAQ) return -1; /* kAudio_ParamError */
    int fmt;
    if (aaudio_output_map_format(inASBD, &fmt) != 0) return -1;
    struct FakeAudioQueue *q = (struct FakeAudioQueue *)calloc(1, sizeof *q);
    if (!q) return -1;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->callback = inCallback;
    q->user = inCallbackUserData;
    q->format = *inASBD;

    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t res = AAudio_createStreamBuilder(&builder);
    if (res == AAUDIO_OK) {
        AAudioStreamBuilder_setSampleRate(builder, (int32_t)inASBD->mSampleRate);
        AAudioStreamBuilder_setChannelCount(builder, (int32_t)inASBD->mChannelsPerFrame);
        AAudioStreamBuilder_setFormat(builder, (aaudio_format_t)fmt);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
        AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
        res = AAudioStreamBuilder_openStream(builder, &q->stream);
        AAudioStreamBuilder_delete(builder);
    }
    if (res != AAUDIO_OK) {
        /* Open failed: hand back the param error; directsound.c reports
         * "output prepare failed" and retries on the next resume. */
        free(q);
        return aaudio_output_status_for((int)res);
    }
    *outAQ = q;
    return noErr;
}

OSStatus AudioQueueAllocateBuffer(AudioQueueRef inAQ, UInt32 inBufferRefSize,
                                  AudioQueueBufferRef *outBuffer) {
    if (!inAQ || !outBuffer || !inBufferRefSize) return -1;
    AudioQueueBufferRef b = (AudioQueueBufferRef)calloc(1, sizeof *b);
    if (!b) return -1;
    b->mAudioData = calloc(1, inBufferRefSize);
    if (!b->mAudioData) { free(b); return -1; }
    b->mAudioDataBytesCapacity = inBufferRefSize;
    b->mAudioDataByteSize = 0;
    pthread_mutex_lock(&inAQ->lock);
    b->all_next = inAQ->all;
    inAQ->all = b;
    pthread_mutex_unlock(&inAQ->lock);
    *outBuffer = b;
    return noErr;
}

OSStatus AudioQueueEnqueueBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer,
                                 UInt32 inNumberParams, const void *inParams) {
    (void)inNumberParams; (void)inParams;
    if (!inAQ || !inBuffer) return -1;
    pthread_mutex_lock(&inAQ->lock);
    if (inAQ->disconnected) {
        pthread_mutex_unlock(&inAQ->lock);
        return AAUDIO_OUTPUT_ERR_DISCONNECTED;
    }
    inBuffer->next = NULL;
    if (inAQ->tail) inAQ->tail->next = inBuffer; else inAQ->head = inBuffer;
    inAQ->tail = inBuffer;
    pthread_cond_signal(&inAQ->cond);
    pthread_mutex_unlock(&inAQ->lock);
    return noErr;
}

OSStatus AudioQueueStart(AudioQueueRef inAQ, const void *inStartTime) {
    (void)inStartTime;
    if (!inAQ) return -1;
    aaudio_result_t res = AAudioStream_requestStart(inAQ->stream);
    if (res != AAUDIO_OK) return aaudio_output_status_for((int)res);
    pthread_mutex_lock(&inAQ->lock);
    inAQ->running = 1;
    if (!inAQ->thread_started) {
        if (pthread_create(&inAQ->thread, NULL, aaudio_output_worker, inAQ) != 0) {
            pthread_mutex_unlock(&inAQ->lock);
            return -1;
        }
        inAQ->thread_started = 1;
    }
    pthread_cond_signal(&inAQ->cond);
    pthread_mutex_unlock(&inAQ->lock);
    return noErr;
}

OSStatus AudioQueuePause(AudioQueueRef inAQ) {
    if (!inAQ) return -1;
    aaudio_result_t res = AAudioStream_requestPause(inAQ->stream);
    if (res != AAUDIO_OK && res != AAUDIO_ERROR_INVALID_STATE)
        return aaudio_output_status_for((int)res);
    pthread_mutex_lock(&inAQ->lock);
    inAQ->running = 0;
    pthread_mutex_unlock(&inAQ->lock);
    return noErr;
}

/* Dispose stops the stream, joins the worker (unblocking any in-flight
 * AAudioStream_write via quit+broadcast), then frees every buffer. The
 * callback runs with no lock held, so a watchdog Dispose from inside the
 * callback cannot deadlock; directsound.c NULLs its buffer pointers on the
 * client side, matching the real AudioQueue dispose contract. */
OSStatus AudioQueueDispose(AudioQueueRef inAQ, Boolean inImmediate) {
    (void)inImmediate;
    if (!inAQ) return -1;
    pthread_mutex_lock(&inAQ->lock);
    inAQ->quit = 1;
    inAQ->running = 0;
    /* Snapshot thread_started inside the lock: AudioQueueStart publishes it
     * under the same mutex, so a Dispose that follows a Start can never read
     * a stale 0 here and skip the join. Skipping the join would free buffers,
     * the stream and the queue while the worker is still rendering into a
     * popped buffer (and re-enqueuing via the callback) — a use-after-free
     * that bionic hands straight back to the next AudioQueueAllocateBuffer,
     * scribbling mixer output over fresh allocations. */
    int join_worker = inAQ->thread_started;
    pthread_cond_broadcast(&inAQ->cond);
    pthread_mutex_unlock(&inAQ->lock);
    if (join_worker) pthread_join(inAQ->thread, NULL);
    if (inAQ->stream) {
        AAudioStream_requestStop(inAQ->stream);
        AAudioStream_close(inAQ->stream);
        inAQ->stream = NULL;
    }
    /* Buffers are linked into the queue's allocation list through all_next;
     * next is the FIFO link (NULL on every buffer once the worker stops).
     * Walking next here freed only the list head and leaked the other two
     * buffers, their mAudioData and the queue on every watchdog rebuild. */
    for (AudioQueueBufferRef b = inAQ->all; b;) {
        AudioQueueBufferRef next = b->all_next;
        free(b->mAudioData);
        free(b);
        b = next;
    }
    pthread_mutex_destroy(&inAQ->lock);
    pthread_cond_destroy(&inAQ->cond);
    free(inAQ);
    return noErr;
}

OSStatus AudioQueueGetProperty(AudioQueueRef inAQ, UInt32 inPropertyID,
                               void *outData, UInt32 *ioDataSize) {
    if (!inAQ || !outData || !ioDataSize) return -1;
    if (inPropertyID != kAudioQueueProperty_IsRunning) return -1; /* kAudio_UnimplementedError */
    if (*ioDataSize < sizeof(UInt32)) return -1;
    UInt32 running = 0;
    pthread_mutex_lock(&inAQ->lock);
    running = (UInt32)inAQ->running;
    pthread_mutex_unlock(&inAQ->lock);
    *(UInt32 *)outData = running;
    *ioDataSize = sizeof(UInt32);
    return noErr;
}

#endif /* __ANDROID__ */
