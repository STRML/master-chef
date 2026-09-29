/* The original engine's own sound state, read from guest memory for the
 * device report (halo.exe c9acf0c46954). Included by directsound.c.
 *
 * Read-only and unsynchronised with the engine thread: a sample may be a
 * frame stale or catch a table mid-update, which is fine for counting, and
 * every table is checked (signature, capacity, element size, bounds) before
 * it is walked. The flat guest mapping covers the whole 32-bit space, so no
 * read can fault; engine_sound_observable keeps it off until the guest has
 * made its DirectSound device (and off in fixtures with a small mapping).
 *
 * Where a sound goes, by the translated functions:
 *   00549AF0 sound_impulse_start makes a source in the sound-source array
 *   (pointer at 0x007252C0; 512 x 0xB0) and asks the sound cache (00443E10)
 *   for its samples without waiting. 0054C020 gives a source whose samples
 *   are loaded one of the engine's channels (0x00724A60, 0x18 each: +0
 *   source or -1; count at 0x007252B4). The DirectSound driver (vtable
 *   0x0069F4C8) gives a channel a voice through 005482E0, recorded per
 *   channel at 0x007252E4 (+0 voice or -1, +2 voice type) and in the voice
 *   record (0x00725430, 0x678 each; count at 0x00725428):
 *     +0x000 state      0 idle, 1 ending, 2 playing (00548050)
 *     +0x002 channel    or -1
 *     +0x008 stopped    set by 00547F60 when it stops the buffer
 *     +0x009 draining   left playing its tail instead (00547F60)
 *     +0x038 type       compared with the type table at 0x0069F528
 *     +0x670 IDirectSoundBuffer8
 *   A voice is taken only if it has no channel and 00547FF0 finds it free:
 *   "stopped" clear, "draining" set, or GetStatus reporting neither PLAYING
 *   nor LOOPING. A sound whose channel finds no voice is dropped at once.
 * The sound cache is a data array (pointer at 0x006AC528; 512 x 16: +2
 * loaded, +5 lock count) filled by the cache reader thread (00443940) from
 * the request table whose pointer is at 0x006AC4A0 (512 x 0x30: +0x1D
 * queued, +0x20 file: 0 the level map, 1 bitmaps.map, 2 sounds.map).
 * Engine flags: 0x00725200 initialised, 0x00725201 enabled, 0x00725202
 * paused by the game, 0x007252B6 disabled. */
enum {
    SOUND_FLAGS_INITIALISED = 0x00725200u, SOUND_FLAGS_ENABLED = 0x00725201u,
    SOUND_FLAGS_PAUSED = 0x00725202u, SOUND_FLAGS_DISABLED = 0x007252B6u,
    SOUND_SOURCES = 0x007252C0u, SOUND_LOOPING = 0x00724A50u,
    SOUND_CHANNELS = 0x00724A60u, SOUND_CHANNEL_COUNT = 0x007252B4u, SOUND_CHANNEL_VOICES = 0x007252E4u,
    SOUND_VOICES = 0x00725430u, SOUND_VOICE_COUNT = 0x00725428u, SOUND_VOICE_TYPES = 0x0069F528u,
    SOUND_CACHE = 0x006AC528u, SOUND_READ_REQUESTS = 0x006AC4A0u,
    SOUND_ARRAY_SIGNATURE = 0x64407440u, /* "d@t@" */
    SOUND_VOICE_STRIDE = 0x678u, SOUND_CHANNEL_STRIDE = 0x18u,
    SOUND_TABLE_LIMIT = 64u, SOUND_READ_SLOTS = 512u, SOUND_READ_STRIDE = 0x30u
};
static int sound_guest_span(uint32_t address, uint64_t length) {
    return address >= 0x10000u && (uint64_t)address + length <= UINT64_C(0x100000000);
}
/* A Halo data array: name[32], +0x20 capacity, +0x22 element size,
 * +0x28 signature, +0x30 live count, +0x34 elements. Returns the elements,
 * or 0 when the table is not (yet) what it should be. */
static uint32_t sound_array(uint32_t pointer_at, uint32_t capacity, uint32_t element, int32_t *live) {
    uint32_t array=G32(pointer_at);
    if(!sound_guest_span(array,0x38)||G32(array+0x28)!=SOUND_ARRAY_SIGNATURE||
       G16(array+0x20)!=capacity||G16(array+0x22)!=element)return 0;
    uint32_t elements=G32(array+0x34);
    if(!sound_guest_span(elements,(uint64_t)capacity*element))return 0;
    if(live)*live=(int16_t)G16(array+0x30);
    return elements;
}
static int sound_voice_type(uint32_t voice) {
    uint16_t type=G16(voice+0x38);
    for(int t=0;t<HOST_DS_VOICE_TYPES;t++)if(G16(SOUND_VOICE_TYPES+2u*(uint32_t)t)==type)return t;
    return -1;
}
/* The engine's tables that need no host lock: its flags, the cache reader's
 * queue, the sound cache, sources, looping sounds and channels. Sets every
 * engine-side field, -1 where unreadable. */
static void sound_engine_tables(HostDsVoiceStats *s) {
    s->engine_flags=-1;
    s->sources=s->looping_sounds=s->channels=s->channels_busy=s->channels_starved=-1;
    s->voices=s->voices_assigned=s->voices_free=s->voices_held=-1;
    s->cache_sounds=s->cache_loaded=s->cache_locked=-1;
    for(int t=0;t<HOST_DS_VOICE_TYPES;t++)s->voices_by_type[t]=s->free_by_type[t]=s->held_by_type[t]=-1;
    for(int f=0;f<3;f++)s->reads_queued[f]=-1;
    if(!engine_flat_base||!atomic_load_explicit(&engine_sound_observable,memory_order_acquire))return;
    s->engine_flags=(G8(SOUND_FLAGS_INITIALISED)?1:0)|(G8(SOUND_FLAGS_ENABLED)?2:0)|
                    (G8(SOUND_FLAGS_PAUSED)?4:0)|(G8(SOUND_FLAGS_DISABLED)?8:0);
    uint32_t requests=G32(SOUND_READ_REQUESTS);
    if(sound_guest_span(requests,(uint64_t)SOUND_READ_SLOTS*SOUND_READ_STRIDE)) {
        for(int f=0;f<3;f++)s->reads_queued[f]=0;
        for(uint32_t i=0;i<SOUND_READ_SLOTS;i++) {
            uint32_t slot=requests+i*SOUND_READ_STRIDE; uint8_t file=G8(slot+0x20);
            if(G8(slot+0x1D)&&file<3)s->reads_queued[file]++;
        }
    }
    uint32_t cache=sound_array(SOUND_CACHE,512,16,&s->cache_sounds);
    if(cache) {
        s->cache_loaded=s->cache_locked=0;
        for(uint32_t i=0;i<512;i++) {
            uint32_t entry=cache+16u*i; if(!G16(entry))continue;
            s->cache_loaded+=G8(entry+2)!=0; s->cache_locked+=G8(entry+5)!=0;
        }
    }
    if(!(s->engine_flags&1))return;
    sound_array(SOUND_SOURCES,512,0xB0,&s->sources);
    sound_array(SOUND_LOOPING,128,0xE4,&s->looping_sounds);
    int32_t channels=(int16_t)G16(SOUND_CHANNEL_COUNT);
    if(channels>=0&&(uint32_t)channels<=SOUND_TABLE_LIMIT) {
        s->channels=channels; s->channels_busy=s->channels_starved=0;
        for(int32_t i=0;i<channels;i++) {
            if(G32(SOUND_CHANNELS+SOUND_CHANNEL_STRIDE*(uint32_t)i)==UINT32_MAX)continue;
            s->channels_busy++;
            if((int16_t)G16(SOUND_CHANNEL_VOICES+4u*(uint32_t)i)<0)s->channels_starved++;
        }
    }
}
/* The voices, each matched to its DirectSound buffer to answer as 00547FF0
 * would. Caller holds objects_lock and mixer.lock, after
 * sound_engine_tables. */
static void sound_engine_voices_locked(HostDsVoiceStats *s) {
    if(!(s->engine_flags>=0&&(s->engine_flags&1)))return;
    int32_t voices=(int16_t)G16(SOUND_VOICE_COUNT);
    if(voices<0||(uint32_t)voices>SOUND_TABLE_LIMIT)return;
    s->voices=voices; s->voices_assigned=s->voices_free=s->voices_held=0;
    for(int t=0;t<HOST_DS_VOICE_TYPES;t++)s->voices_by_type[t]=s->free_by_type[t]=s->held_by_type[t]=0;
    for(int32_t i=0;i<voices;i++) {
        uint32_t voice=SOUND_VOICES+SOUND_VOICE_STRIDE*(uint32_t)i; int type=sound_voice_type(voice);
        if(type>=0)s->voices_by_type[type]++;
        if((int16_t)G16(voice+2)>=0){s->voices_assigned++;continue;}
        DsObject *o=object_from_guest(G32(voice+0x670));
        uint32_t status=o?voice_status(&o->voice):0;
        if(!G8(voice+8)||G8(voice+9)||!(status&(DSBSTATUS_PLAYING|DSBSTATUS_LOOPING))) {
            s->voices_free++; if(type>=0)s->free_by_type[type]++;
        } else { s->voices_held++; if(type>=0)s->held_by_type[type]++; }
    }
}
/* The host's counters and buffers, then the engine's voices. Caller holds
 * objects_lock and mixer.lock and has filled the tables. */
static void voice_stats_locked(HostDsVoiceStats *s) {
    s->plays=atomic_load_explicit(&voice_plays,memory_order_relaxed);
    s->stops=atomic_load_explicit(&voice_stops,memory_order_relaxed);
    s->status_probes=atomic_load_explicit(&voice_status_probes,memory_order_relaxed);
    s->status_busy=atomic_load_explicit(&voice_status_busy,memory_order_relaxed);
    s->rejected=atomic_load_explicit(&voice_calls_rejected,memory_order_relaxed);
    s->buffers=s->playing=0;
    for(uint32_t i=1;i<object_count;i++) {
        DsObject *o=&objects[i]; if(!o->alive||o->primary)continue;
        s->buffers++; s->playing+=o->voice.playing!=0;
    }
    uint32_t peak=atomic_load_explicit(&voices_playing_peak,memory_order_relaxed);
    while(peak<s->playing&&!atomic_compare_exchange_weak_explicit(&voices_playing_peak,&peak,s->playing,memory_order_relaxed,memory_order_relaxed)){}
    s->playing_peak=atomic_load_explicit(&voices_playing_peak,memory_order_relaxed);
    sound_engine_voices_locked(s);
}
/* One host.log line when a voice type is left with no voice the engine can
 * take while some of its voices sit idle but unusable ("held": no channel,
 * stopped, yet not free by 00547FF0's test), and one when that clears. All
 * of a type's voices busy on sounds is ordinary and not logged. In Build75
 * every positional voice was held this way from 35 seconds into b30. */
static void voice_exhaustion_note(const HostDsVoiceStats *s) {
    static _Atomic unsigned exhausted_types, notes;
    unsigned now=0;
    for(int t=0;t<HOST_DS_VOICE_TYPES;t++)if(s->held_by_type[t]>0&&s->free_by_type[t]==0)now|=1u<<t;
    unsigned before=atomic_exchange_explicit(&exhausted_types,now,memory_order_relaxed);
    if(before==now||atomic_fetch_add_explicit(&notes,1,memory_order_relaxed)>=64)return;
    host_log("[audio-engine] %s types=0x%x was=0x%x frames=%llu voices=%d assigned=%d free=%d held=%d free-by-type=%d/%d,%d/%d,%d/%d,%d/%d sources=%d channels=%d/%d plays=%llu stops=%llu busy-probes=%llu/%llu",
        now?"voices held":"voices free",now,before,(unsigned long long)host_dsound_output_frames(),
        s->voices,s->voices_assigned,s->voices_free,s->voices_held,
        s->free_by_type[0],s->voices_by_type[0],s->free_by_type[1],s->voices_by_type[1],
        s->free_by_type[2],s->voices_by_type[2],s->free_by_type[3],s->voices_by_type[3],
        s->sources,s->channels_busy,s->channels,(unsigned long long)s->plays,(unsigned long long)s->stops,
        (unsigned long long)s->status_busy,(unsigned long long)s->status_probes);
}
void host_dsound_get_voice_stats(HostDsVoiceStats *out) {
    if(!out)return;
    pthread_once(&mixer_once,mixer_initialize_once);
    memset(out,0,sizeof *out);
    sound_engine_tables(out);
    pthread_mutex_lock(&objects_lock);
    pthread_mutex_lock(&mixer.lock);
    voice_stats_locked(out);
    pthread_mutex_unlock(&mixer.lock);
    pthread_mutex_unlock(&objects_lock);
    voice_exhaustion_note(out);
}
