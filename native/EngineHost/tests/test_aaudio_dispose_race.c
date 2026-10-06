/* native/EngineHost/tests/test_aaudio_dispose_race.c - host regression for
 * the Android AAudio sink's rebuild-churn contract (android/aaudio_output.c).
 *
 * The device watchdog disposes and rebuilds the output queue every ~2 s
 * while the worker thread pops buffers and delivers them to the client
 * callback. Two defects killed the Quest 3 run:
 *  - AudioQueueDispose read thread_started OUTSIDE the queue lock and could
 *    skip pthread_join, freeing the queue/buffers while the worker was still
 *    rendering into a popped buffer (heap-use-after-free under ASan).
 *  - AudioQueueDispose walked the allocation list with the FIFO link
 *    (b->next) instead of all_next, so only the list head was freed: every
 *    rebuild leaked two buffers, their 12 KiB mAudioData, and the queue.
 *
 * Build (macOS arm64; the __ANDROID__ worker body compiles against the stub
 * headers in tests/aaudio_shim_include/):
 *   clang -O1 -g -fsanitize=address,undefined -std=c11 -w -D__ANDROID__ \
 *     -I native/EngineHost/tests/aaudio_shim_include \
 *     -I native/EngineHost/android -I native/EngineReuse \
 *     native/EngineHost/android/aaudio_output.c \
 *     native/EngineHost/tests/test_aaudio_dispose_race.c -o /tmp/aaudio_race
 * Run: /tmp/aaudio_race   (expect: PASS ... ASan-clean, exit 0)
 */
#include <aaudio/AAudio.h>   /* the test-stub NDK surface (tests/aaudio_shim_include) */
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "fake_audioqueue.h"

/* --- allocation balance interposer (classic tcmalloc-style trick) ---------
 * Both translation units link into one executable, so their calloc/free
 * references bind here; dylibs keep their two-level binding to libc. ASan's
 * redzones still catch any use-after-free; this counter catches the quieter
 * failure: a dispose that frees the wrong list and leaks. */
static _Atomic long g_alloc_count, g_free_count;
void *malloc(size_t n) { static void *(*r)(size_t); if (!r) r = dlsym(RTLD_NEXT, "malloc"); void *p = r(n); if (p) atomic_fetch_add(&g_alloc_count, 1); return p; }
void *calloc(size_t a, size_t b) { static void *(*r)(size_t, size_t); if (!r) r = dlsym(RTLD_NEXT, "calloc"); void *p = r(a, b); if (p) atomic_fetch_add(&g_alloc_count, 1); return p; }
void free(void *p) { static void (*r)(void *); if (!r) r = dlsym(RTLD_NEXT, "free"); if (p) atomic_fetch_add(&g_free_count, 1); r(p); }

/* --- fake AAudio backend (the NDK runtime, replaced for the host test) --- */
typedef int aaudio_result_t;
static _Atomic int g_write_calls, g_close_calls;
static _Atomic int g_in_write;
static AAudioStreamBuilder *s_builder_storage;
static char g_stream_storage[16];
static AAudioStream *const g_stream = (AAudioStream *)g_stream_storage;

aaudio_result_t AAudio_createStreamBuilder(AAudioStreamBuilder **b) { *b = &s_builder_storage; return AAUDIO_OK; }
void AAudioStreamBuilder_setSampleRate(AAudioStreamBuilder *b, int32_t r) { (void)b;(void)r; }
void AAudioStreamBuilder_setChannelCount(AAudioStreamBuilder *b, int32_t c) { (void)b;(void)c; }
void AAudioStreamBuilder_setFormat(AAudioStreamBuilder *b, aaudio_format_t f) { (void)b;(void)f; }
void AAudioStreamBuilder_setPerformanceMode(AAudioStreamBuilder *b, aaudio_performance_mode_t m) { (void)b;(void)m; }
void AAudioStreamBuilder_setSharingMode(AAudioStreamBuilder *b, aaudio_sharing_mode_t m) { (void)b;(void)m; }
void AAudioStreamBuilder_setUsage(AAudioStreamBuilder *b, aaudio_usage_t u) { (void)b;(void)u; }
aaudio_result_t AAudioStreamBuilder_openStream(AAudioStreamBuilder *b, AAudioStream **s) { (void)b; *s = g_stream; return AAUDIO_OK; }
void AAudioStreamBuilder_delete(AAudioStreamBuilder *b) { (void)b; }
aaudio_result_t AAudioStream_requestStart(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestPause(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_requestStop(AAudioStream *s) { (void)s; return AAUDIO_OK; }
aaudio_result_t AAudioStream_close(AAudioStream *s) { (void)s; atomic_fetch_add(&g_close_calls, 1); return AAUDIO_OK; }
aaudio_result_t AAudioStream_write(AAudioStream *s, const void *data, int32_t frames, int64_t timeout) {
    (void)s; (void)timeout;
    atomic_fetch_add(&g_in_write, 1);
    volatile const float *p = (const volatile float *)data;  /* touch the buffer: ASan must see the access */
    float sink = 0.f;
    for (int32_t i = 0; i < frames * 2 && i < 64; i++) sink += p[i];
    (void)sink;
    atomic_fetch_sub(&g_in_write, 1);
    atomic_fetch_add(&g_write_calls, 1);
    return frames;
}

/* --- directsound.c's audio_callback stand-in: render + re-enqueue -------- */
static _Atomic unsigned g_callbacks;
static void client_callback(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    (void)user;
    float *s = (float *)b->mAudioData;
    for (UInt32 i = 0; i < b->mAudioDataBytesCapacity / sizeof(float); i++) s[i] = 0.25f;  /* "PCM" */
    b->mAudioDataByteSize = b->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(q, b, 0, NULL);
    atomic_fetch_add(&g_callbacks, 1);
}

#define CYCLES 200
#define PER_CYCLE 7   /* queue + 3 buffers + 3 mAudioData */

int main(void) {
    long alloc0 = 0, free0 = 0;  /* measured after warm-up below */
    for (int cycle = 0; cycle < CYCLES; cycle++) {
        AudioStreamBasicDescription f = {0};
        f.mSampleRate = 48000; f.mFormatID = kAudioFormatLinearPCM;
        f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        f.mBytesPerPacket = f.mBytesPerFrame = 8; f.mFramesPerPacket = 1;
        f.mChannelsPerFrame = 2; f.mBitsPerChannel = 32;
        AudioQueueRef q;
        if (cycle == 1) { alloc0 = atomic_load(&g_alloc_count); free0 = atomic_load(&g_free_count); }
        if (AudioQueueNewOutput(&f, client_callback, NULL, NULL, NULL, 0, &q) != noErr) { puts("prepare failed"); return 1; }
        AudioQueueBufferRef bufs[3];
        for (int i = 0; i < 3; i++)
            if (AudioQueueAllocateBuffer(q, 1536 * 8, &bufs[i]) != noErr) { puts("alloc failed"); return 1; }
        UInt32 running = 123; UInt32 sz = sizeof running;
        if (AudioQueueGetProperty(q, kAudioQueueProperty_IsRunning, &running, &sz) != noErr || running != 0) { puts("IsRunning contract"); return 1; }
        for (int i = 0; i < 3; i++)
            if (AudioQueueEnqueueBuffer(q, bufs[i], 0, NULL) != noErr) { puts("enqueue failed"); return 1; }
        if (AudioQueueStart(q, NULL) != noErr) { puts("start failed"); return 1; }
        struct timespec ts = { 0, (time_t)(2000000 + (cycle % 5) * 9000000) };  /* vary: catch the worker at every phase */
        nanosleep(&ts, NULL);
        if (cycle % 7 == 3) { AudioQueuePause(q); nanosleep(&ts, NULL); }  /* exercise the parked-restore path */
        if (AudioQueueDispose(q, 1) != noErr) { puts("dispose failed"); return 1; }
    }
    unsigned cbs = atomic_load(&g_callbacks);
    long allocs = atomic_load(&g_alloc_count) - alloc0;
    long frees = atomic_load(&g_free_count) - free0;
    printf("cycles=%d callbacks=%u allocs=%ld frees=%ld closes=%d\n",
           CYCLES, cbs, allocs, frees, atomic_load(&g_close_calls));
    if (cbs == 0) { puts("worker never delivered - facade broken"); return 1; }
    if (allocs != (long)(CYCLES - 1) * PER_CYCLE || frees != allocs) {
        printf("LEAK: allocs=%ld frees=%ld expected=%d\n", allocs, frees, (CYCLES - 1) * PER_CYCLE);
        return 1;
    }
    puts("PASS dispose/join + full buffer reclaim under rebuild churn (ASan-clean)");
    return 0;
}
