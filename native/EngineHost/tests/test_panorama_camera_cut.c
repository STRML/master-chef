/* Camera cuts must not publish a mixture of two shots, on CPU or GPU. */
#define main pose_export_fixture_main
#include "test_panorama_pose_export.c"
#undef main
int main(void) {
    backbuffer.renderer=(void*)1;
    float a[9], b[9]; camera(a,0,0,1,0); camera(b,0,0,1,1);
    draw(a,HALO_PANORAMA_MONO_MASK); assert(panorama_info.valid); present();
    host_panorama_reset(); host_panorama_set_camera(b);
    assert(!host_panorama_all_layers_ready());
    host_panorama_begin(1); host_panorama_projection(1,1,0,0,4,4,0); host_panorama_end(1);
    assert(!panorama_info.valid); /* no old-camera CPU carry */
    present(); draw(b,HALO_PANORAMA_MONO_MASK); assert(panorama_info.valid); present();
    HaloPanoramaGPUSink sink={acquire,release,carry}; host_panorama_set_gpu_sink(&sink);
    host_panorama_invalidate(); draw(b,HALO_PANORAMA_MONO_MASK); host_panorama_gpu_carry_missing();
    published=panorama_info; host_panorama_gpu_handoff(); present();
    draw(a,1u<<1); host_panorama_gpu_carry_missing();
    assert(!panorama_info.valid); /* sink offers old metadata: reject it */
    host_panorama_gpu_handoff(); present(); draw(a,HALO_PANORAMA_MONO_MASK); host_panorama_gpu_carry_missing();
    assert(panorama_info.valid);
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++) if(HALO_PANORAMA_MONO_MASK&(1u<<k))
        assert(panorama_info.layer_epoch[k]>=panorama_info.cut_epoch);
    /* The Maw exterior flyby travels more than two units each frame. It
     * must not start a new scene every tick and supersede every GPU result. */
    present(); host_panorama_set_gpu_sink(NULL);
    float sweep[9]; camera(sweep,0,0,1,0);
    draw(sweep,HALO_PANORAMA_MONO_MASK); present();
    camera(sweep,6,0,1,0); draw(sweep,HALO_PANORAMA_MONO_MASK); present();
    uint64_t scene=panorama_scene_epoch;
    for(int i=2;i<32;i++) {
        camera(sweep,6.f*i,0.01f*i*i,1,0);
        draw(sweep,HALO_PANORAMA_MONO_MASK);
        assert(panorama_info.valid && panorama_scene_epoch==scene); present();
    }
    /* A discontinuity during that sweep must still invalidate old images. */
    sweep[0]+=40.f; host_panorama_set_camera(sweep);
    assert(panorama_scene_epoch>scene && !host_panorama_all_layers_ready());
    puts("PASS camera cut: old shots rejected; full refresh recovers; fast continuous cinematic motion retains publication continuity");
}
