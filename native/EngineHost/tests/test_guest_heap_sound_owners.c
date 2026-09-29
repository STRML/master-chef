/* Production heap + DirectSound ownership, no AudioQueue or game assets.
 * clang -O2 -pthread -I../EngineReuse -ffunction-sections -fdata-sections
 *   tests/test_guest_heap_sound_owners.c directsound_mixer.c
 *   -Wl,-dead_strip -framework AudioToolbox -lm -o /tmp/test_guest_heap_sound_owners */
#include "../host.c"
#include "../directsound.c"
#include <assert.h>
_Thread_local EngineCPU *host_active_cpu;
void halo_haptics_observe(const float *p, uint32_t n, uint32_t r) { (void)p; (void)n; (void)r; }
void halo_haptics_voice_onset(float l, float f, int a) { (void)l; (void)f; (void)a; }
float halo_settings_head_yaw(void) { return 0.f; }
float halo_settings_self_gain(void) { return 1.f; }
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
    engine_flat_base = mmap(NULL, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(engine_flat_base != MAP_FAILED);
    const DsMixerFormat format = {1, 1, 16, 2, 48000, 96000};
    pthread_once(&mixer_once, mixer_initialize_once);
    pthread_t renderer; assert(!pthread_create(&renderer, NULL, render_thread, NULL));
    uint32_t previous_guest = 0;
    for (unsigned i = 0; i < 4096; i++) {
        DsObject *o = object_new(DSBCAPS_CTRL3D, 32768, &format, 0); assert(o);
        uint32_t guest = o->guest, data = o->guest_data;
        if (previous_guest) assert(guest == previous_guest); /* prove real address reuse */
        assert(guest_alloc_size(guest) == 16 && guest_alloc_size(data) == 32768);
        assert(G32(data) == 0 && G32(data + 32764) == 0);
        S32(0x200, 0x279afa86u); assert(buffer_qi(o, 0x200, 0x220));
        uint32_t alias = G32(0x220); assert(object_from_guest(alias) == o);
        assert(guest_alloc_size(alias) == 16 && heap_live_blocks == 3);
        pthread_mutex_lock(&mixer.lock);
        int16_t pcm[4] = {4096, 8192, -4096, 0}; memcpy(o->voice.data, pcm, sizeof pcm);
        o->voice.playing = o->voice.looping = 1;
        pthread_mutex_unlock(&mixer.lock);
        assert(object_release(o) == 1); /* 3D interface still owns all storage */
        assert(object_from_guest(guest) == o && object_from_guest(alias) == o);
        uint32_t scratch = guest_alloc(32768);
        assert(scratch != data && scratch != guest && scratch != alias);
        assert(guest_alloc_size(data) == 32768); guest_free(scratch);
        assert(object_release(o) == 0);
        assert(!object_from_guest(guest) && !object_from_guest(alias));
        assert(!guest_alloc_size(data) && !guest_alloc_size(guest) && !guest_alloc_size(alias));
        assert(heap_live_blocks == 0 && heap_initial.span == HEAP_END - HEAP_START);
        previous_guest = guest;
    }
    atomic_store(&stop_renderer, 1); assert(!pthread_join(renderer, NULL));
    assert(atomic_load(&render_calls) && !output_queue && !output_requested);
    assert(heap_slab_count == 1);
    assert(!munmap(engine_flat_base, GUEST_SIZE));
    puts("PASS: production guest heap with 4096 recycled DirectSound/3D-alias lifetimes; shared refs retain guest data, final release reclaims it, concurrent mixer drains before removal.");
}
