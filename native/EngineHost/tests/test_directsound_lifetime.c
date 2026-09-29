/* Source-only DirectSound object lifecycle, no AudioQueue or owned assets.
 * clang -O2 -pthread -I native/EngineHost -I native/EngineReuse \
 *   native/EngineHost/tests/test_directsound_lifetime.c \
 *   native/EngineHost/directsound_mixer.c -framework AudioToolbox -lm -o /tmp/ds-life
 * IO_LIFETIME_SOURCE can name an older directsound.c to reproduce the failures. */
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#ifdef IO_LIFETIME_SOURCE
#include IO_LIFETIME_SOURCE
#else
#include "../directsound.c"
#endif

uint8_t *engine_flat_base;
static uint32_t heap_next = 0x10000, allocation_count, live_allocations;
static uint32_t allocations[32768];
static int fail_after = -1;
void host_log(const char *fmt, ...) { (void)fmt; }
uint32_t guest_alloc(uint32_t n) {
    if (fail_after == 0) { fail_after = -1; return 0; }
    if (fail_after > 0) fail_after--;
    uint32_t p = heap_next; heap_next += (n + 15u) & ~15u;
    assert(heap_next < 8*1024*1024 && allocation_count < 32768);
    allocations[allocation_count++] = p; live_allocations++; return p;
}
void guest_free(uint32_t p) {
    for (uint32_t i = 0; i < allocation_count; i++) if (allocations[i] == p) {
        allocations[i] = 0; live_allocations--; return;
    }
    assert(!"free must match one live allocation");
}
uint32_t host_proc_address(const char *dll, const char *name) { (void)dll; (void)name; return 0; }
void halo_haptics_observe(const float *p, uint32_t n, uint32_t r) { (void)p; (void)n; (void)r; }
void halo_haptics_voice_onset(float l, float f, int a) { (void)l; (void)f; (void)a; }
float halo_settings_head_yaw(void) { return 0.f; }
float halo_settings_self_gain(void) { return 1.f; }
static const DsMixerFormat format = {1, 1, 16, 2, 48000, 96000};
static _Atomic int stop_renderer;
static _Atomic unsigned render_calls;
static void *render_thread(void *unused) {
    (void)unused; float output[128];
    while (!atomic_load(&stop_renderer)) {
        ds_mixer_render(&mixer, output, 64, 48000);
        atomic_fetch_add(&render_calls, 1);
        for (unsigned i = 0; i < 128; i++) assert(isfinite(output[i]));
    }
    return NULL;
}
int main(void) {
    engine_flat_base = calloc(1, 8*1024*1024); assert(engine_flat_base);
    pthread_once(&mixer_once, mixer_initialize_once);
    pthread_t renderer; assert(!pthread_create(&renderer, NULL, render_thread, NULL));
    uint32_t stale_guest = 0, stale_3d = 0;
    for (unsigned i = 0; i < DS_OBJECT_MAX*2; i++) {
        DsObject *o = object_new(DSBCAPS_CTRL3D, 8, &format, 0); assert(o);
        assert(!object_from_guest(stale_guest) && !object_from_guest(stale_3d));
        uint32_t guest = o->guest;
        /* QueryInterface extends the same voice lifetime. */
        S32(0x200, 0x279afa86u); assert(buffer_qi(o, 0x200, 0x220));
        uint32_t guest_3d = G32(0x220); assert(object_from_guest(guest_3d) == o);
        pthread_mutex_lock(&mixer.lock);
        int16_t pcm[4] = {4096, 8192, -4096, 0}; memcpy(o->voice.data, pcm, sizeof pcm);
        o->voice.playing = o->voice.looping = 1;
        pthread_mutex_unlock(&mixer.lock);
        assert(object_release(o) == 1 && object_from_guest(guest));
        assert(object_release(o) == 0 && !object_from_guest(guest) && !object_from_guest(guest_3d));
        assert(live_allocations == 0);
        stale_guest = guest; stale_3d = guest_3d;
    }
    atomic_store(&stop_renderer, 1); assert(!pthread_join(renderer, NULL));
    assert(atomic_load(&render_calls) > 0);
    puts("PASS 2048 create/play/3D-alias/release lifetimes with concurrent mixer rendering");

    /* Failure after mixer insertion must return its slot and both heaps. */
    for (int stage = 0; stage < 2; stage++) {
        fail_after = stage; assert(!object_new(0, 8, &format, 0));
        assert(!live_allocations);
        for (int i = 0; i < DS_MIXER_MAX_VOICES; i++) assert(!mixer.voices[i]);
    }
    DsObject *voices[DS_MIXER_MAX_VOICES];
    for (int i = 0; i < DS_MIXER_MAX_VOICES; i++) { voices[i] = object_new(0, 8, &format, 0); assert(voices[i]); }
    uint32_t before = allocation_count;
    for (unsigned i = 0; i < DS_OBJECT_MAX*2; i++) assert(!object_new(0, 8, &format, 0));
    assert(allocation_count == before); /* Rejected mixer slots consume no guest storage. */
    assert(object_release(voices[0]) == 0);
    voices[0] = object_new(0, 8, &format, 0); assert(voices[0]);
    for (int i = 0; i < DS_MIXER_MAX_VOICES; i++) assert(object_release(voices[i]) == 0);
    assert(!live_allocations);
    puts("PASS guest-allocation failure and 2048 mixer-capacity failures recover without slot/storage loss");

    DsObject *primary[DS_OBJECT_MAX-1];
    for (unsigned i = 0; i < DS_OBJECT_MAX-1; i++) { primary[i] = object_new(1, 0, &format, 1); assert(primary[i]); }
    assert(!object_new(1, 0, &format, 1));
    uint32_t stale = primary[0]->guest;
    assert(object_release(primary[0]) == 0);
    primary[0] = object_new(1, 0, &format, 1); assert(primary[0] && !object_from_guest(stale));
    S32(0x200, 0x279afa84u); assert(buffer_qi(primary[0], 0x200, 0x220));
    assert(object_release(primary[0]) == 1);
    for (unsigned i = 0; i < DS_OBJECT_MAX-1; i++) assert(object_release(primary[i]) == 0);
    assert(!live_allocations && !output_queue && !output_requested);
    free(engine_flat_base);
    puts("PASS live object-table capacity, primary/listener release and reusable slots; no playback claim");
}
