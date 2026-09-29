/* ABI checks for the mixer: voice setup, resampling values, listener state.
 * Signal-quality checks live in test_mixer_quality.c.
 *
 * The mixer reports transients to the haptics observer, so it links too:
 *
 * clang -O2 -I native/EngineHost native/EngineHost/tests/test_directsound_mixer.c \
 *   native/EngineHost/directsound_mixer.c native/EngineHost/halo_settings.c \
 *   native/EngineHost/haptics.c -lm -o /tmp/halo-mixer-abi
 */
#include "../directsound_mixer.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int near(float a, float b) { return fabsf(a-b) < 0.02f; }

int main(void) {
    DsMixer m; ds_mixer_init(&m);
    int16_t ramp[] = { 0, 16384, 32767, 16384 };
    DsMixerVoice v; memset(&v,0,sizeof v);
    v.data=(uint8_t *)ramp; v.bytes=sizeof ramp;
    v.format=(DsMixerFormat){1,1,16,2,24000,48000};
    v.frequency=24000; v.playing=1; v.looping=1;
    v.current_3d.min_distance=v.deferred_3d.min_distance=1.f;
    v.current_3d.max_distance=v.deferred_3d.max_distance=1000.f;
    v.current_3d.inside_angle=v.deferred_3d.inside_angle=360;
    v.current_3d.outside_angle=v.deferred_3d.outside_angle=360;
    assert(ds_mixer_add(&m,&v));
    /* Doubling the rate lands every other output frame exactly on a source
     * frame, and those come through untouched below the limiter's knee. The
     * frames in between are the Catmull-Rom reconstruction of the four
     * neighbours: on this deliberately jagged ramp they read 0.1875 where
     * straight-line interpolation would read 0.25. Real material is band
     * limited and the cubic curve is the closer fit. The one full-scale frame
     * sits above the 0.90 knee and comes back as 0.95. */
    float out[16]; ds_mixer_render(&m,out,8,48000);
    assert(near(out[0],0.f) && near(out[4],.5f) && near(out[12],.5f));
    assert(near(out[8],.95f) && near(out[2],.1875f));
    assert(near(out[6],out[10]) && out[6] > .78f && out[6] < .8125f);
    for(int i=0;i<8;i++) assert(near(out[2*i],out[2*i+1]));

    pthread_mutex_lock(&m.lock);
    v.cursor_frames=0; v.volume=-602; v.pan=10000;
    pthread_mutex_unlock(&m.lock);
    ds_mixer_render(&m,out,3,24000);
    assert(fabsf(out[1])<.001f && fabsf(out[3])>.20f && fabsf(out[5])>.45f);
    assert(fabsf(out[0])<.001f && fabsf(out[2])<.001f && fabsf(out[4])<.001f);

    pthread_mutex_lock(&m.lock);
    v.cursor_frames=1; v.volume=0; v.pan=0; v.has_3d=1;
    v.current_3d.position[0]=10.f; v.current_3d.min_distance=1.f;
    v.current_3d.max_distance=100.f; v.current_3d.mode=0;
    pthread_mutex_unlock(&m.lock);
    ds_mixer_render(&m,out,1,24000);
    assert(out[1] > 0.04f && fabsf(out[0]) < .001f);

    /* The listener's distance factor is metres per vector unit and belongs to
     * the Doppler calculation, not the rolloff. Halo sets it to 3.048, ten
     * feet in metres. Applying it to the attenuation distance pushed every
     * positional sound 9.7 dB down and meant a sound sitting exactly at its
     * minimum distance could never reach full volume. */
    pthread_mutex_lock(&m.lock);
    v.volume=0; v.pan=0; v.has_3d=1; v.current_3d.mode=0;
    v.current_3d.min_distance=2.f; v.current_3d.max_distance=3.4e38f;
    v.current_3d.position[0]=0.f; v.current_3d.position[1]=0.f; v.current_3d.position[2]=2.f;
    m.current_listener.distance_factor=1.f;
    pthread_mutex_unlock(&m.lock);
    float unit_l,unit_r; ds_mixer_voice_gains(&m,&v,&unit_l,&unit_r);
    m.current_listener.distance_factor=3.048f;
    float scaled_l,scaled_r; ds_mixer_voice_gains(&m,&v,&scaled_l,&scaled_r);
    assert(near(unit_l,scaled_l)&&near(unit_r,scaled_r));
    assert(near(unit_l,1.f)&&near(unit_r,1.f));
    v.current_3d.position[2]=8.f;                 /* four times the minimum */
    float far_l,far_r; ds_mixer_voice_gains(&m,&v,&far_l,&far_r);
    assert(near(far_l,.25f)&&near(far_r,.25f));
    m.current_listener.distance_factor=1.f;

    /* The player's own weapon keeps its lift for the whole sound. Judged per
     * block, the lift fell away mid-shot once a moving player left the shot
     * more than half a unit behind: 18 dB, heard as the sound cutting out. */
    pthread_mutex_lock(&m.lock);
    ds_mixer_voice_restart(&v);
    v.cursor_frames=0; v.playing=1; v.looping=1; v.current_3d.min_distance=1.f; v.current_3d.max_distance=1000.f;
    v.current_3d.position[0]=0.f; v.current_3d.position[1]=0.f; v.current_3d.position[2]=0.2f;
    pthread_mutex_unlock(&m.lock);
    ds_mixer_render(&m,out,4,48000);                /* the shot starts on the listener */
    assert(v.self_voice);
    v.current_3d.position[2]=1.9f;                   /* the player has moved on */
    float kept_l,kept_r; ds_mixer_voice_gains(&m,&v,&kept_l,&kept_r);
    v.self_voice=0;
    float world_l,world_r; ds_mixer_voice_gains(&m,&v,&world_l,&world_r);
    assert(kept_l > 4.f*world_l && kept_r > 4.f*world_r);
    pthread_mutex_lock(&m.lock); v.self_voice=1; ds_mixer_voice_restart(&v); pthread_mutex_unlock(&m.lock);
    assert(!v.self_voice && v.onset_gap==UINT32_MAX && v.onset_armed);
    v.has_3d=0;

    /* A muted voice is not resampled, but it still runs: its cursor moves,
     * a one-shot ends and a loop wraps, exactly as if it had been mixed. */
    pthread_mutex_lock(&m.lock);
    v.cursor_frames=0; v.volume=-10000; v.playing=1; v.looping=0;
    pthread_mutex_unlock(&m.lock);
    ds_mixer_render(&m,out,2,48000);
    assert(near(v.cursor_frames,1.f) && v.playing);
    ds_mixer_render(&m,out,8,48000);
    assert(!v.playing && v.end_count==1);
    pthread_mutex_lock(&m.lock);
    v.cursor_frames=3; v.playing=1; v.looping=1;
    pthread_mutex_unlock(&m.lock);
    ds_mixer_render(&m,out,4,48000);
    assert(v.playing && v.cursor_frames < 4.0 && near(v.cursor_frames,1.f));
    for(int i=0;i<16;i++) assert(out[i]==0.f);
    pthread_mutex_lock(&m.lock); v.volume=0; v.looping=0; pthread_mutex_unlock(&m.lock);

    ds_mixer_remove(&m,&v);
    memset(out,0x55,sizeof out); ds_mixer_render(&m,out,8,48000);
    for(int i=0;i<16;i++) assert(out[i]==0.f);
    puts("directsound mixer tests passed");
    return 0;
}
