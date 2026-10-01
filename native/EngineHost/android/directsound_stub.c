/* directsound_stub.c - silent AudioQueue replacement for the Android
 * headless build (see android/fake_audioqueue.h for the API contract).
 *
 * The queue keeps a FIFO of enqueued buffers and a worker thread that hands
 * them to the client callback at wall-clock cadence (one buffer per buffer
 * duration), so the DirectSound shim's mixer, play cursors, watchdog and
 * diagnostics run exactly as they do on macOS with a real output device.
 * Nothing is ever sent to a sink: the mixed PCM is simply discarded.
 *
 * Locking: the callback runs with no queue lock held (it may call
 * AudioQueueEnqueueBuffer, as the real AudioQueue callback does). Dispose
 * joins the worker, so a caller that holds its own lifecycle lock while
 * disposing must not hold anything the callback needs - the same contract
 * the real AudioQueue imposes (its callback can run on the disposing thread
 * during AudioQueueDispose with invokeCallback=true).
 */
#include "fake_audioqueue.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    UInt32 sample_rate;      /* from the format, for pacing */
    UInt32 buffer_frames;    /* most recent allocation size */
};

static void fake_sleep_ns(uint64_t ns) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / UINT64_C(1000000000));
    ts.tv_nsec = (long)(ns % UINT64_C(1000000000));
    nanosleep(&ts, NULL);
}

static void *fake_audioqueue_worker(void *arg) {
    struct FakeAudioQueue *q = (struct FakeAudioQueue *)arg;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (!q->quit && !q->head)
            pthread_cond_wait(&q->cond, &q->lock);
        if (q->quit) { pthread_mutex_unlock(&q->lock); break; }
        AudioQueueBufferRef b = q->head;
        q->head = b->next;
        if (!q->head) q->tail = NULL;
        int running = q->running;
        pthread_mutex_unlock(&q->lock);
        if (!running) {
            /* Paused: park the buffer at the tail so Start re-arms it. */
            pthread_mutex_lock(&q->lock);
            b->next = NULL;
            if (q->tail) q->tail->next = b; else q->head = b;
            q->tail = b;
            pthread_mutex_unlock(&q->lock);
            continue;
        }
        /* One buffer per its duration: the cadence a real output device sets. */
        if (q->sample_rate && q->buffer_frames)
            fake_sleep_ns((uint64_t)q->buffer_frames * UINT64_C(1000000000) / q->sample_rate);
    }
    return NULL;
}

OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *inASBD,
                             AudioQueueCallback inCallback, void *inCallbackUserData,
                             void *inCallbackRunLoop, const void *inCallbackRunLoopMode,
                             UInt32 inFlags, AudioQueueRef *outAQ) {
    (void)inCallbackRunLoop; (void)inCallbackRunLoopMode; (void)inFlags;
    if (!inASBD || !inCallback || !outAQ) return -1; /* kAudio_ParamError */
    struct FakeAudioQueue *q = (struct FakeAudioQueue *)calloc(1, sizeof *q);
    if (!q) return -1;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->callback = inCallback;
    q->user = inCallbackUserData;
    q->sample_rate = (UInt32)(inASBD->mSampleRate > 0.0 ? inASBD->mSampleRate : 48000.0);
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
    b->next = inAQ->all;
    inAQ->all = b;
    if (inAQ->sample_rate)
        inAQ->buffer_frames = inBufferRefSize / 8u; /* stereo float32 */
    pthread_mutex_unlock(&inAQ->lock);
    *outBuffer = b;
    return noErr;
}

OSStatus AudioQueueEnqueueBuffer(AudioQueueRef inAQ, AudioQueueBufferRef inBuffer,
                                 UInt32 inNumberParams, const void *inParams) {
    (void)inNumberParams; (void)inParams;
    if (!inAQ || !inBuffer) return -1;
    pthread_mutex_lock(&inAQ->lock);
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
    pthread_mutex_lock(&inAQ->lock);
    inAQ->running = 1;
    if (!inAQ->thread_started) {
        if (pthread_create(&inAQ->thread, NULL, fake_audioqueue_worker, inAQ) != 0) {
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
    pthread_mutex_lock(&inAQ->lock);
    inAQ->running = 0;
    pthread_mutex_unlock(&inAQ->lock);
    return noErr;
}

OSStatus AudioQueueDispose(AudioQueueRef inAQ, Boolean inImmediate) {
    (void)inImmediate;
    if (!inAQ) return -1;
    pthread_mutex_lock(&inAQ->lock);
    inAQ->quit = 1;
    inAQ->running = 0;
    pthread_cond_broadcast(&inAQ->cond);
    pthread_mutex_unlock(&inAQ->lock);
    if (inAQ->thread_started) pthread_join(inAQ->thread, NULL);
    /* The worker parked above holds no buffer: every buffer is either in the
     * FIFO, in the all list, or owned by the caller's state. Free the all
     * list; the client dropped its references (that is the real AudioQueue's
     * dispose contract, and directsound.c NULLs its buffer pointers). */
    for (AudioQueueBufferRef b = inAQ->all; b;) {
        AudioQueueBufferRef next = b->next;
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
