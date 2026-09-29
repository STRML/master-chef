/* Production pool/publication logic; deterministic delayed completions and
 * fake texture copies. No GPU, engine assets, or headset required. */
#include <stdint.h>
#include <time.h>
#include <assert.h>
static uint64_t test_now_ns = 1000000000ull;
static uint64_t publication_clock(clockid_t clock) {
    assert(clock == CLOCK_UPTIME_RAW);
    return test_now_ns;
}
#define clock_gettime_nsec_np publication_clock
#include "../Sources/EngineVisionRuntime.m"
#undef clock_gettime_nsec_np

int host_quit_requested;
/* The host counters each Present samples for the frame split (core_telemetry.h). */
uint8_t *engine_flat_base;
uint64_t host_yield_spin_ns, host_present_sleep_ns;
_Thread_local uint64_t host_thread_wait_ns, host_thread_waits;
int host_core_telemetry = 1;   /* on, as engine_worker leaves it by default */
static uint64_t pass_ns_total;
uint64_t host_pass_profile_total_ns(void) { return pass_ns_total; }
void host_draw_profile_snapshot(uint64_t out[12]) { memset(out,0,12*sizeof *out); out[1]=pass_ns_total; }
static unsigned copies;
static HaloPanoramaInfo incoming_frame;
static int incoming_complete;
static unsigned pacing_calls, commits;
static uint64_t paced_release_ns;
static void (*pending_completion)(void *, int);
static void *pending_publication;
uint64_t host_frame_pacer_present(const HaloFramePacerDisplay *display) {
    assert(display && display->latch_ns == atomic_load(&display_latch_ns));
    assert(display->latch_ns > 0 && display->latch_ns <= test_now_ns);
    assert(os_unfair_lock_trylock(&runtime_lock));
    os_unfair_lock_unlock(&runtime_lock);
    assert(!pending_completion && pacing_calls == commits);
    pacing_calls++;
    test_now_ns += 3000000ull; /* Simulate the blocking wait. */
    paced_release_ns = test_now_ns;
    return paced_release_ns;
}
int mr_commit_async(void (*done)(void *, int), void *arg) {
    assert(done && arg && pacing_calls == commits + 1);
    PanoramaPublish *publication = arg;
    assert(publication->release_ns == paced_release_ns);
    assert(os_unfair_lock_trylock(&runtime_lock));
    os_unfair_lock_unlock(&runtime_lock);
    commits++; pending_completion = done; pending_publication = arg;
    return 0;
}
void host_log(const char *format, ...) { (void)format; }
const char *mr_last_commit_error(void) { return "injected failure"; }
int mr_blit_texture_copy(void *source,void *destination) {
    assert(source&&destination&&source!=destination);copies++;return 0;
}
int host_panorama_frame(HaloPanoramaInfo *info,const void **layers) {
    *info=incoming_frame;memset(layers,0,HALO_PANORAMA_LAYERS*sizeof *layers);return incoming_complete;
}
static void complete(int slot,uint64_t sequence,int ok) {
    PanoramaPublish *p=calloc(1,sizeof *p);assert(p);
    test_now_ns += 2000000ull; p->release_ns = test_now_ns - 1000000ull;
    p->slot=slot;p->info.sequence=p->info.flat_sequence=p->info.source_epoch=sequence;
    p->info.scene_epoch=7;p->info.width=p->info.height=4;p->info.status=HALO_PANORAMA_COMPLETE;
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++){
        p->info.layer_epoch[k]=sequence-k;p->info.projection_x[k]=(float)sequence+k;
        p->info.projection_y[k]=2;p->info.viewport_u_max[k]=p->info.viewport_v_max[k]=1;
        for(int j=0;j<HALO_PANORAMA_POSE_FLOATS;j++)p->info.layer_pose[k][j]=(float)(sequence*100+k*10+j);
    }
    p->info.cut_epoch=sequence-3;
    gpu_slots[slot].state=GPU_SLOT_RENDERING;
    gpu_published(p,ok);
}
int main(void) { @autoreleasepool {
    HaloFramePacerDisplay display=display_for_pacer();
    assert(!display.latch_ns&&!display.period_ns&&!display.lag_ns);
    for(int s=0;s<PANORAMA_GPU_SLOTS;s++){
        gpu_slots[s].width=gpu_slots[s].height=4;
        for(int k=0;k<HALO_PANORAMA_LAYERS;k++)gpu_slots[s].textures[k]=(id<MTLTexture>)[NSObject new];
    }
    complete(0,10,1);complete(1,12,1);complete(2,11,1);
    assert(gpu_latest==1&&panorama_info.sequence==12&&gpu_publish_superseded==1);
    assert(gpu_slots[2].state==GPU_SLOT_FREE);
    complete(0,13,0);assert(gpu_latest==1&&gpu_publish_failed==1);
    assert(atomic_load(&publish_lag_next)==2); /* Failed and stale completions are excluded. */
    gpu_slots[2].state=GPU_SLOT_RENDERING;
    HaloPanoramaInfo source={0};
    assert(gpu_carry(2,5,&source)==5&&copies==2);
    assert(source.source_epoch==12&&source.scene_epoch==7&&source.width==4);
    assert(source.layer_epoch[2]==10&&source.projection_x[2]==14);
    assert(source.viewport_u_max[2]==1);
    /* Each carried layer keeps the camera it was drawn with, and the cut
     * epoch travels with the images it describes. */
    assert(source.layer_pose[2][4]==12*100+2*10+4&&source.layer_pose[0][0]==1200&&source.cut_epoch==9);
    gpu_release(2);
    /* A pending earlier world completion cannot resurrect the world over a
     * newer loading/menu frame. */
    latest_sequence=20;uint32_t frame[16]={0};metalwin_present(frame,4,4);
    assert(gpu_flat_sequence==21&&panorama_info.status==HALO_PANORAMA_FLAT);
    complete(0,18,1);assert(gpu_latest==1&&panorama_info.flat_sequence==21);
    EngineVisionPanoramaGPUSnapshot lease={0};assert(!enginevision_panorama_gpu_latest(&lease));
    incoming_frame.status=HALO_PANORAMA_WORLD_INCOMPLETE;incoming_frame.scene_epoch=7;
    metalwin_present_dropped(4,4);
    assert(!enginevision_panorama_gpu_latest(&lease));
    complete(0,22,1);assert(enginevision_panorama_gpu_latest(&lease)&&lease.slot==0);
    complete(1,23,1);assert(gpu_slots[0].state==GPU_SLOT_RETIRING&&gpu_slots[0].leases==1);
    enginevision_panorama_gpu_release(lease.slot);assert(gpu_slots[0].state==GPU_SLOT_FREE);
    /* A same-size Reset has no flat frame, but its new generation still
     * blocks stale images and pending publication from the preceding scene. */
    incoming_frame.scene_epoch=8;metalwin_present_dropped(4,4);
    assert(!enginevision_panorama_gpu_latest(&lease));
    complete(2,24,1);assert(gpu_slots[2].state==GPU_SLOT_FREE&&gpu_latest==1);
    assert(gpu_frames_published==4&&gpu_publish_superseded==3);
    /* Every Present feeds the frame split. First sight of game-time globals
     * is unknown; later continuous counter deltas count completed ticks. */
    uint8_t *flat=calloc(1,0x800000);assert(flat);engine_flat_base=flat;
    uint32_t globals=0x700000;memcpy(flat+0x6F1D6C,&globals,4);
    HaloFrameSplitTotals before,after;halo_frame_split_read(&engine_frame_split,&before);
    assert(before.presents==3&&before.ticks==0);
    for(uint32_t tick=100;tick<=106;tick+=2){
        uint16_t last_call=2;memcpy(flat+globals+0xC,&tick,4);memcpy(flat+globals+0x10,&last_call,2);
        pass_ns_total+=1000;metalwin_present_dropped(4,4);
    }
    halo_frame_split_read(&engine_frame_split,&after);
    assert(after.presents==7&&after.ticks==6&&after.tick_frames[2]==3);
    assert(after.tick_resyncs==before.tick_resyncs+1&&after.tick_mismatch_frames==before.tick_mismatch_frames);
    assert(after.pass_ns-before.pass_ns==4000&&after.frames==before.frames+4);
    assert(after.tick_unknown_frames==before.tick_unknown_frames+1);
    EngineVisionCoreTelemetry core;enginevision_core_telemetry(&core);
    assert(core.snapshot_available&&core.presents==7&&core.ticks==6);
    assert(core.frames==after.frames&&core.outside_pass_ns==after.outside_pass_ns);
    assert(core.thread_info_available==0); /* test has no engine_worker */
    engine_flat_base=NULL;free(flat);
    errno=E2BIG;engine_thread_sample();assert(errno==E2BIG);
    /* HALO_CORE_TELEMETRY=0: a Present keeps only the CPU histogram it always
     * kept, and the report gets no groups and asks the kernel nothing. */
    HaloFrameSplitTotals on,off;halo_frame_split_read(&engine_frame_split,&on);
    host_core_telemetry=0;
    uint32_t cpu_frames_before=0,cpu_frames_after=0;
    for(int k=0;k<16;k++)cpu_frames_before+=engine_cpu_frames[k];
    size_t cpu_now=0;int cpu_number=!pthread_cpu_number_np(&cpu_now);
    pass_ns_total+=1000;metalwin_present_dropped(4,4);
    for(int k=0;k<16;k++)cpu_frames_after+=engine_cpu_frames[k];
    assert(!cpu_number||cpu_frames_after==cpu_frames_before+1);
    halo_frame_split_read(&engine_frame_split,&off);
    assert(!memcmp(&off,&on,sizeof off));
    EngineVisionCoreTelemetry zero;memset(&zero,0,sizeof zero);memset(&core,0xA5,sizeof core);
    assert(!enginevision_core_telemetry(&core)&&!memcmp(&core,&zero,sizeof core));
    errno=E2BIG;engine_thread_sample();assert(errno==E2BIG);
    host_core_telemetry=1;
    assert(atomic_load(&publish_lag_next)==4); /* Only accepted publications. */
    /* Exercise the actual pacing/commit path, which dead stripping otherwise
     * removes from this pool test. Delayed completion must not publish early. */
    gpu_slots[2].state=GPU_SLOT_RENDERING;
    metalwin_present_gpu(2,4,4); /* An incomplete panorama never waits or commits. */
    assert(pacing_calls==0&&commits==0&&gpu_slots[2].state==GPU_SLOT_FREE);
    incoming_complete=1;incoming_frame.status=HALO_PANORAMA_COMPLETE;
    incoming_frame.source_epoch=100;gpu_slots[2].state=GPU_SLOT_RENDERING;
    int previous_latest=gpu_latest;
    metalwin_present_gpu(2,4,4);
    assert(pacing_calls==1&&commits==1&&pending_completion);
    assert(gpu_latest==previous_latest&&gpu_slots[2].state==GPU_SLOT_RENDERING);
    assert(atomic_load(&publish_lag_next)==4);
    test_now_ns+=2000000ull;
    pending_completion(pending_publication,1);pending_completion=NULL;pending_publication=NULL;
    assert(gpu_latest==2&&gpu_frames_published==5);
    assert(atomic_load(&publish_lag_next)==5&&atomic_load(&publish_lag_ns[4])==2000000ull);
    EngineVisionPanoramaGPUSnapshot paced_lease={0};
    assert(enginevision_panorama_gpu_latest(&paced_lease)&&paced_lease.slot==2);
    assert(atomic_load(&display_latch_ns)==test_now_ns);
    enginevision_panorama_gpu_release(paced_lease.slot);
    /* The display sample is a compositor lease attempt, even while a menu
     * prevents a world lease. Repeated attempts at the same timestamp and
     * long loading gaps must not become display-period samples. */
    for(int i=0;i<8;i++)atomic_store(&display_interval_ns[i],0);
    atomic_store(&display_interval_next,0);
    uint64_t nominal_period=11111111ull;
    for(int i=0;i<8;i++) {
        test_now_ns+=nominal_period;
        assert(enginevision_panorama_gpu_latest(&paced_lease));
        enginevision_panorama_gpu_release(paced_lease.slot);
    }
    display=display_for_pacer();
    assert(display.latch_ns==test_now_ns&&display.period_ns==nominal_period);
    assert(display.lag_ns==1000000ull); /* Four earlier 1 ms samples outweigh the 2 ms completion. */
    unsigned samples=atomic_load(&display_interval_next);
    gpu_flat_sequence=UINT64_MAX;
    assert(!enginevision_panorama_gpu_latest(&paced_lease));
    assert(atomic_load(&display_interval_next)==samples);
    test_now_ns+=300000000ull;
    assert(!enginevision_panorama_gpu_latest(&paced_lease));
    assert(atomic_load(&display_interval_next)==samples);
    assert(display_for_pacer().latch_ns==test_now_ns);
    free(latest_frame);latest_frame=NULL;
    puts("PASS publication order, flat-frame precedence, exact layer camera metadata, leases, frame split and core telemetry, paced commit, publication lag and display sampling");
} }
