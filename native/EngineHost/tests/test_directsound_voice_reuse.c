/* Halo's voice reuse against the production DirectSound shim; no AudioQueue,
 * engine run or owned assets.
 *
 * Build75 went silent 35 seconds into b30 and stayed silent until the pause
 * menu. Halo streams every sound through a pool of looping DirectSound
 * buffers ("voices": 24 positional and three of each 2D kind on the
 * headset). A voice whose sound ends is stopped by 00547F60, which marks it
 * "stopped"; the allocator 005482E0 takes such a voice again only when
 * 00547FF0's GetStatus finds neither PLAYING nor LOOPING. The shim reported
 * LOOPING for a buffer stopped after a looping Play, so every voice started
 * one sound and was held until sound_stop_all (a level load, the pause
 * menu, a checkpoint revert) cleared the marks. In the session's trace no
 * voice plays twice between two stop-alls, on the headset and in the Mac
 * runs build44-b30-audio-live and build59-d40-audio-nocut alike.
 *
 * This drives the engine's allocation through the shim's COM entry points
 * with the engine's own voice records in guest memory: a sound starts on a
 * voice the allocator picks (Lock, Unlock, SetCurrentPosition and a looping
 * Play, as 00547C80 does), plays, and ends through 00547F60's stop path and
 * 00548410's release. By default 005482E0 and 00547FF0 are C transcriptions;
 * with -DHALO_TRANSLATED_ALLOCATOR and -I <translation directory> the test
 * runs the translated original functions themselves.
 *
 * clang -O2 -pthread -DENGINE_FLAT_MEMORY=1 -I native/EngineHost -I native/EngineReuse \
 *   native/EngineHost/tests/test_directsound_voice_reuse.c native/EngineHost/directsound_mixer.c \
 *   -framework AudioToolbox -lm -o /tmp/voice-reuse && /tmp/voice-reuse
 * Add -DHALO_TRANSLATED_ALLOCATOR -I native/build/engine-reuse/<build> for the
 * original functions. -DBUILD75_GET_STATUS answers GetStatus as Build75 did
 * and reproduces the silence: every voice once, then nothing. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "../directsound.c"

uint8_t *engine_flat_base;
static uint32_t heap_next = 0x01000000u;
static char last_log[4096];
static unsigned engine_logs;
void host_log(const char *format, ...) {
    va_list args; va_start(args, format); vsnprintf(last_log, sizeof last_log, format, args); va_end(args);
    engine_logs += strncmp(last_log, "[audio-engine]", 14) == 0;
}
uint32_t guest_alloc(uint32_t n) { uint32_t p = heap_next; heap_next += (n + 15u) & ~15u; assert(heap_next < 0x04000000u); return p; }
void guest_free(uint32_t p) { (void)p; }
void halo_haptics_observe(const float *p, uint32_t n, uint32_t r) { (void)p; (void)n; (void)r; }
void halo_haptics_voice_onset(float l, float f, int a) { (void)l; (void)f; (void)a; }
float halo_settings_head_yaw(void) { return 0.f; }
float halo_settings_self_gain(void) { return 1.f; }

#ifdef BUILD75_GET_STATUS
/* host_dsound_buffer_9 as Build75 shipped it: LOOPING whenever the last Play
 * looped, playing or not. */
static void build75_get_status(EngineCPU *cpu) { DsObject*o=object_from_guest(ARG(0)); if(!o||!ARG(1))RET_STDCALL(DSERR_INVALIDPARAM,2); pthread_mutex_lock(&mixer.lock); uint32_t s=o->voice.playing?DSBSTATUS_PLAYING:0; if(o->voice.looping)s|=DSBSTATUS_LOOPING; pthread_mutex_unlock(&mixer.lock); S32(ARG(1),s); RET_STDCALL(DS_OK,2); }
static const HostShim get_status = build75_get_status;
#else
static const HostShim get_status = host_dsound_buffer_9;
#endif
/* The COM vtables point at synthetic addresses; engine_dispatch routes them
 * back to the shim entries, as the host's import table does. */
static const HostShimEntry shims[] = { HOST_DSOUND_SHIM_ENTRIES };
enum { SHIM_BASE = 0xFF000000u };
uint32_t host_proc_address(const char *dll, const char *name) {
    (void)dll;
    for (uint32_t i = 0; i < sizeof shims / sizeof *shims; i++) if (!strcmp(shims[i].name, name)) return SHIM_BASE | i;
    return 0;
}
static __attribute__((unused)) void translated_allocate(EngineCPU *cpu, int16_t channel);
static int translated_dispatch(EngineCPU *cpu, uint32_t address);
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    if ((address & 0xFF000000u) == SHIM_BASE) {
        HostShim fn = shims[address & 0xFFFFu].fn; (fn == host_dsound_buffer_9 ? get_status : fn)(cpu); return;
    }
    if (translated_dispatch(cpu, address)) return;
    fprintf(stderr, "unexpected call to %08x\n", address); abort();
}

static uint32_t call(HostShim fn, unsigned nargs, ...) {
    EngineCPU cpu = {0}; cpu.gpr[4] = 0x1000; S32(0x1000, 0x1234);
    va_list args; va_start(args, nargs); for (unsigned i = 0; i < nargs; i++) S32(0x1004 + 4*i, va_arg(args, uint32_t)); va_end(args);
    fn(&cpu); assert(cpu.pc == 0x1234 && cpu.gpr[4] == 0x1004 + 4*nargs); return cpu.gpr[0];
}

/* The engine's tables (directsound_engine_state.inc has the layout). */
#define VOICE(i) (SOUND_VOICES + SOUND_VOICE_STRIDE * (uint32_t)(i))
#define CHANNEL_MAP(c) (SOUND_CHANNEL_VOICES + 4u * (uint32_t)(c))
enum { POSITIONAL_TYPE_FLAGS = 9, FIRST_VOICE_OF_TYPE = 0x00746028u, STATUS_OUT = 0x2000u, LOCKS = 0x2100u };
static const uint32_t ring_bytes = 132300; /* three seconds of 22,050 Hz mono, as Halo makes them */
static unsigned voice_count;

/* 00547FF0: may a stopped, unassigned voice be taken? */
static int model_voice_free(int16_t index) {
    uint32_t v = VOICE(index);
    if ((int32_t)call(get_status, 2, G32(v + 0x670), STATUS_OUT) < 0) return 0;
    if (G8(v + 9)) { S32(v + 0x84, UINT32_MAX); S8(v + 8, 0); return 1; }
    if (G32(STATUS_OUT) & 5u) return 0;
    S8(v + 8, 0); return 1;
}
/* 005482E0: give channel a voice of its type; the voice goes in its map. */
static __attribute__((unused)) void model_allocate(int16_t channel) {
    uint32_t map = CHANNEL_MAP(channel); int16_t type = (int16_t)G16(map + 2);
    int16_t i = (int16_t)G16(FIRST_VOICE_OF_TYPE + 2u*(uint16_t)type);
    while ((int16_t)G16(map) == -1 && i < (int16_t)G16(SOUND_VOICE_COUNT)) {
        uint32_t v = VOICE(i);
        if (G16(v + 0x38) != G16(SOUND_VOICE_TYPES + 2u*(uint16_t)type)) break;
        if ((int16_t)G16(v + 2) == -1 && (!G8(v + 8) || model_voice_free(i))) S16(map, (uint16_t)i);
        i++;
    }
    if ((int16_t)G16(map) != -1) S16(VOICE((int16_t)G16(map)) + 2, (uint16_t)channel);
}
static int16_t allocate(int16_t channel) {
#ifdef HALO_TRANSLATED_ALLOCATOR
    EngineCPU cpu = {0}; translated_allocate(&cpu, channel);
#else
    model_allocate(channel);
#endif
    return (int16_t)G16(CHANNEL_MAP(channel));
}
/* 00547C80 from idle: fill the whole ring from the start, position, Play looping. */
static void start_sound(int16_t index, uint8_t level) {
    uint32_t v = VOICE(index), buffer = G32(v + 0x670);
    S16(v, 1); S32(v + 0x78, 0); S32(v + 0x7C, UINT32_MAX);
    assert(call(host_dsound_buffer_11, 8, buffer, 0u, ring_bytes, LOCKS, LOCKS + 4, LOCKS + 8, LOCKS + 12, 0u) == DS_OK);
    memset(GPTR(G32(LOCKS)), level, G32(LOCKS + 4));
    assert(call(host_dsound_buffer_19, 5, buffer, G32(LOCKS), G32(LOCKS + 4), G32(LOCKS + 8), G32(LOCKS + 12)) == DS_OK);
    assert(call(host_dsound_buffer_13, 2, buffer, 0u) == DS_OK);
    assert(call(host_dsound_buffer_12, 4, buffer, 0u, 0u, 1u) == DS_OK);
    S16(v, 2);
}
/* 00548410 then 00547F60's stop path: Stop, mark stopped, release. */
static void end_sound(int16_t channel) {
    int16_t index = (int16_t)G16(CHANNEL_MAP(channel)); uint32_t v = VOICE(index);
    assert(call(host_dsound_buffer_18, 1, G32(v + 0x670)) == DS_OK);
    S32(v + 0x84, UINT32_MAX); S8(v + 9, 0); S8(v + 0x98, 0); S16(v, 0); S8(v + 8, 1); S16(v + 4, 0xFFFF);
    S16(v + 2, 0xFFFF); S16(CHANNEL_MAP(channel), 0xFFFF);
}
static uint32_t make_buffer(void) {
    uint32_t fmt = guest_alloc(18), desc = guest_alloc(36), out = guest_alloc(4);
    DsMixerFormat format = {1, 1, 16, 2, 22050, 44100}; write_format(fmt, &format);
    S32(desc, 36); S32(desc + 4, DSBCAPS_CTRL3D | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GETCURRENTPOSITION2);
    S32(desc + 8, ring_bytes); S32(desc + 16, fmt);
    assert(call(host_dsound_device_3, 4, 0u, desc, out, 0u) == DS_OK);
    return G32(out);
}
/* The engine as it stands after sound_stop_all: every voice idle, unmarked. */
static void engine_voices(unsigned count) {
    voice_count = count;
    S8(SOUND_FLAGS_INITIALISED, 1); S8(SOUND_FLAGS_ENABLED, 1);
    S16(SOUND_VOICE_COUNT, (uint16_t)count); S16(SOUND_VOICE_TYPES, POSITIONAL_TYPE_FLAGS); S16(FIRST_VOICE_OF_TYPE, 0);
    for (unsigned i = 0; i < count; i++) {
        uint32_t v = VOICE(i); memset(GPTR(v), 0, SOUND_VOICE_STRIDE);
        S16(v + 2, 0xFFFF); S16(v + 4, 0xFFFF); S16(v + 0x38, POSITIONAL_TYPE_FLAGS); S32(v + 0x670, make_buffer());
    }
    for (int16_t c = 0; c < 4; c++) { S16(CHANNEL_MAP(c), 0xFFFF); S16(CHANNEL_MAP(c) + 2, 0); }
}

static void check_status_and_cursors(void) {
    uint32_t buffer = G32(VOICE(0) + 0x670), positions = 0x2200; DsObject *o = object_from_guest(buffer); assert(o);
    assert(call(get_status, 2, buffer, STATUS_OUT) == DS_OK && G32(STATUS_OUT) == 0);
    assert(call(host_dsound_buffer_12, 4, buffer, 0u, 0u, 1u) == DS_OK);
    assert(call(get_status, 2, buffer, STATUS_OUT) == DS_OK && G32(STATUS_OUT) == (DSBSTATUS_PLAYING | DSBSTATUS_LOOPING));
    float out[2*512]; ds_mixer_render(&mixer, out, 512, 48000);
    assert(call(host_dsound_buffer_4, 3, buffer, positions, positions + 4) == DS_OK);
    uint32_t play = G32(positions), write = G32(positions + 4);
    assert(play == (uint32_t)o->voice.cursor_frames * 2u && play > 0);
    assert(write == (play + 22050u/20u*2u) % ring_bytes);          /* the lead while playing */
    assert(call(host_dsound_buffer_18, 1, buffer) == DS_OK);
#ifndef BUILD75_GET_STATUS
    assert(call(get_status, 2, buffer, STATUS_OUT) == DS_OK && G32(STATUS_OUT) == 0);
    assert(call(host_dsound_buffer_4, 3, buffer, positions, positions + 4) == DS_OK);
    assert(G32(positions) == play && G32(positions + 4) == play);   /* stopped: one position */
    assert(call(host_dsound_buffer_11, 8, buffer, 0u, 4u, LOCKS, LOCKS + 4, LOCKS + 8, LOCKS + 12, DSLOCK_FROMWRITECURSOR) == DS_OK);
    assert(G32(LOCKS) == o->guest_data + play);
    assert(call(host_dsound_buffer_19, 5, buffer, G32(LOCKS), G32(LOCKS + 4), G32(LOCKS + 8), G32(LOCKS + 12)) == DS_OK);
    assert(call(host_dsound_buffer_12, 4, buffer, 0u, 0u, 0u) == DS_OK);
    assert(call(get_status, 2, buffer, STATUS_OUT) == DS_OK && G32(STATUS_OUT) == DSBSTATUS_PLAYING);
    assert(call(host_dsound_buffer_18, 1, buffer) == DS_OK);
    puts("PASS GetStatus: nothing when stopped (LOOPING only with PLAYING); cursors: write leads play only while playing");
#else
    assert(call(get_status, 2, buffer, STATUS_OUT) == DS_OK && G32(STATUS_OUT) == DSBSTATUS_LOOPING);
    puts("REPRODUCED GetStatus reports LOOPING for a stopped buffer");
#endif
}

/* The Build75 session: sounds come and go on four channels. Each needs one
 * of the 24 positional voices. */
static unsigned play_sounds(unsigned sounds) {
    unsigned started = 0; float out[2*256];
    for (unsigned k = 0; k < sounds; k++) {
        int16_t channel = (int16_t)(k % 4u), index = allocate(channel);
        if (index < 0) continue;                     /* the engine drops the sound */
        assert((unsigned)index < voice_count && (int16_t)G16(VOICE(index) + 2) == channel);
        started++; start_sound(index, (uint8_t)(k + 1));
        ds_mixer_render(&mixer, out, 256, 48000);
        end_sound(channel);
    }
    return started;
}

static uint32_t engine_array(uint32_t pointer_at, uint16_t capacity, uint16_t element, int16_t live) {
    uint32_t array = guest_alloc(0x38 + (uint32_t)capacity * element); memset(GPTR(array), 0, 0x38 + (uint32_t)capacity * element);
    S16(array + 0x20, capacity); S16(array + 0x22, element); S32(array + 0x28, SOUND_ARRAY_SIGNATURE);
    S16(array + 0x30, (uint16_t)live); S32(array + 0x34, array + 0x38); S32(pointer_at, array);
    return array + 0x38;
}
static __attribute__((unused)) void check_engine_tables(void) {
    HostDsVoiceStats s;
    atomic_store(&engine_sound_observable, 0);
    host_dsound_get_voice_stats(&s);
    assert(s.engine_flags == -1 && s.sources == -1 && s.voices == -1 && s.reads_queued[2] == -1);
    atomic_store(&engine_sound_observable, 1);
    engine_array(SOUND_SOURCES, 512, 0xB0, 7);
    engine_array(SOUND_LOOPING, 128, 0xE4, 3);
    uint32_t cache = engine_array(SOUND_CACHE, 512, 16, 3);
    S16(cache, 0x8001); S8(cache + 2, 1); S8(cache + 5, 1);        /* loaded and in use */
    S16(cache + 16, 0x8002); S8(cache + 16 + 2, 1);                  /* loaded */
    S16(cache + 32, 0x8003);                                         /* still being read */
    uint32_t requests = guest_alloc(SOUND_READ_SLOTS * SOUND_READ_STRIDE); memset(GPTR(requests), 0, SOUND_READ_SLOTS * SOUND_READ_STRIDE);
    S32(SOUND_READ_REQUESTS, requests);
    S8(requests + 0x1D, 1); S8(requests + 0x20, 2);
    S8(requests + SOUND_READ_STRIDE + 0x1D, 1); S8(requests + SOUND_READ_STRIDE + 0x20, 2);
    S8(requests + 5*SOUND_READ_STRIDE + 0x1D, 1); S8(requests + 5*SOUND_READ_STRIDE + 0x20, 0);
    S8(requests + 9*SOUND_READ_STRIDE + 0x20, 2);                    /* done: not queued */
    S16(SOUND_CHANNEL_COUNT, 5);
    for (unsigned c = 0; c < 5; c++) { S32(SOUND_CHANNELS + SOUND_CHANNEL_STRIDE*c, UINT32_MAX); S16(CHANNEL_MAP(c), 0xFFFF); }
    S32(SOUND_CHANNELS, 0x80010002u); S32(SOUND_CHANNELS + SOUND_CHANNEL_STRIDE, 0x80020003u);
    int16_t index = allocate(0); assert(index >= 0); start_sound(index, 0x40);  /* channel 0 has its voice */
    host_dsound_get_voice_stats(&s);
    assert(s.engine_flags == 3 && s.sources == 7 && s.looping_sounds == 3);
    assert(s.channels == 5 && s.channels_busy == 2 && s.channels_starved == 1);
    assert(s.voices == (int32_t)voice_count && s.voices_assigned == 1 && s.voices_held == 0 && s.voices_free == (int32_t)voice_count - 1);
    assert(s.voices_by_type[0] == (int32_t)voice_count && s.free_by_type[0] == (int32_t)voice_count - 1 && s.held_by_type[0] == 0);
    assert(s.cache_sounds == 3 && s.cache_loaded == 2 && s.cache_locked == 1);
    assert(s.reads_queued[0] == 1 && s.reads_queued[1] == 0 && s.reads_queued[2] == 2);
    assert(s.playing >= 1 && s.buffers >= voice_count && s.playing_peak >= s.playing);
    end_sound(0);
    /* A voice left marked stopped while its buffer still plays is held. */
    uint32_t v = VOICE(1); assert(call(host_dsound_buffer_12, 4, G32(v + 0x670), 0u, 0u, 1u) == DS_OK); S8(v + 8, 1);
    host_dsound_get_voice_stats(&s);
    assert(s.voices_held == 1 && s.held_by_type[0] == 1 && s.voices_free == (int32_t)voice_count - 1);
    assert(call(host_dsound_buffer_18, 1, G32(v + 0x670)) == DS_OK);
    puts("PASS engine tables: flags, sources, looping sounds, channels and starved channels, voices free/held by type, cache, queued reads");
}

int main(void) {
    engine_flat_base = calloc(1, 0x04000000u); assert(engine_flat_base);
    pthread_once(&mixer_once, mixer_initialize_once);
    build_vtables();
    atomic_store(&engine_sound_observable, 1);
    engine_voices(24);
    check_status_and_cursors();
    engine_voices(24);
    uint64_t plays = atomic_load(&voice_plays), stops = atomic_load(&voice_stops); (void)stops;
    uint64_t probes = atomic_load(&voice_status_probes), busy = atomic_load(&voice_status_busy); (void)probes; (void)busy;
    unsigned started = play_sounds(200);
    HostDsVoiceStats s; host_dsound_get_voice_stats(&s);
#ifdef BUILD75_GET_STATUS
    assert(started == 24 && s.plays - plays == 24);
    for (unsigned i = 0; i < voice_count; i++) assert(G8(VOICE(i) + 8) == 1);  /* each used once, all still marked */
    printf("REPRODUCED %s with Build75's GetStatus: 200 sounds, %u started (each of the 24 voices once), then none\n",
#ifdef HALO_TRANSLATED_ALLOCATOR
        "translated 005482E0/00547FF0",
#else
        "C transcription of 005482E0/00547FF0",
#endif
        started);
    return 0;
#else
    assert(started == 200 && s.voices_free == 24 && s.voices_held == 0 && engine_logs == 0);
    assert(s.plays - plays == 200 && s.stops - stops == 200);
    assert(s.status_probes - probes == 199 && s.status_busy == busy && s.rejected == 0);
    printf("PASS %s: 200 sounds on 24 positional voices all started, stopped voices reused; %llu GetStatus probes, none busy\n",
#ifdef HALO_TRANSLATED_ALLOCATOR
        "translated 005482E0/00547FF0",
#else
        "C transcription of 005482E0/00547FF0",
#endif
        (unsigned long long)(s.status_probes - probes));
    check_engine_tables();
    /* Held voices are named once in host.log, and their release once. */
    engine_logs = 0; uint32_t v = VOICE(2);
    assert(call(host_dsound_buffer_12, 4, G32(v + 0x670), 0u, 0u, 1u) == DS_OK); S8(v + 8, 1);
    for (unsigned i = 3; i < voice_count; i++) {
        S8(VOICE(i) + 8, 1); S8(VOICE(i) + 9, 0);
        assert(call(host_dsound_buffer_12, 4, G32(VOICE(i) + 0x670), 0u, 0u, 1u) == DS_OK);
    }
    S16(VOICE(0) + 2, 0); S16(VOICE(1) + 2, 1);                      /* both on channels */
    host_dsound_get_voice_stats(&s); host_dsound_get_voice_stats(&s);
    assert(s.voices_free == 0 && s.voices_held == (int32_t)voice_count - 2 && engine_logs == 1 && strstr(last_log, "voices held types=0x1"));
    for (unsigned i = 2; i < voice_count; i++) assert(call(host_dsound_buffer_18, 1, G32(VOICE(i) + 0x670)) == DS_OK);
    host_dsound_get_voice_stats(&s);
    assert(s.voices_held == 0 && engine_logs == 2 && strstr(last_log, "voices free types=0x0 was=0x1"));
    puts("PASS host.log names held voices once and their release once");
    return 0;
#endif
}

#ifdef HALO_TRANSLATED_ALLOCATOR
/* The original functions, translated; their registers are macros, so they
 * come last. */
#include "engine_flags.h"
#include "engine_hooks.h"
#include "engine_registers.h"
void sub_00547FF0(EngineCPU *cpu);
void sub_005482E0(EngineCPU *cpu);
#include "sub_00547FF0.c"
#include "sub_005482E0.c"
static void translated_allocate(EngineCPU *cpu, int16_t channel) {
    cpu->gpr[4] = 0x1800; engine_push(cpu, (uint16_t)channel, 4); engine_push(cpu, 0xDEAD0000u, 4);
    cpu->pc = 0x005482E0u; sub_005482E0(cpu);
    assert(cpu->pc == 0xDEAD0000u && cpu->gpr[4] == 0x17FC);
}
static int translated_dispatch(EngineCPU *cpu, uint32_t address) {
    if (address == 0x00547FF0u) { cpu->pc = address; sub_00547FF0(cpu); return 1; }
    return 0;
}
#else
static void translated_allocate(EngineCPU *cpu, int16_t channel) { (void)cpu; (void)channel; abort(); }
static int translated_dispatch(EngineCPU *cpu, uint32_t address) { (void)cpu; (void)address; return 0; }
#endif
