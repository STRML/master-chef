#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#if TARGET_OS_VISION
#import <AVFAudio/AVFAudio.h>
#import <UIKit/UIApplication.h>
#import <dispatch/dispatch.h>
#endif
#import <os/lock.h>
#include <pthread.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include "EngineVisionBridge.h"
#include "gamecontroller.h"
#include "directsound.h"

int host_run(const char *exe, const char *root);
void host_draw_profile_snapshot(uint64_t out[12]);   /* d3d9_render.inc, via host.h */
uint64_t host_pass_profile_total_ns(void);
extern int host_trace_imports;

static char host_cmdline_buf[512] = "\"C:\\Halo\\halo.exe\" -window -novideo";
const char *host_command_line = host_cmdline_buf;
extern int host_quit_requested;

static os_unfair_lock runtime_lock = OS_UNFAIR_LOCK_INIT;
static int runtime_state = ENGINEVISION_IDLE;
static int runtime_exit = 0;
static char runtime_status[512] = "Import an owned Halo game folder to begin.";
static char runtime_root[PATH_MAX];
static char runtime_exe[PATH_MAX];
static uint8_t *latest_frame;
static size_t latest_capacity, latest_bytes;
static int32_t latest_width, latest_height;
static uint64_t latest_sequence;
#include "panorama.h"
#include "haptics.h"
#include "pointer.h"
#include "threading.h"
#include "core_telemetry.h"
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <unistd.h>
/* The engine thread, for its CPU time in the device report. */
static _Atomic mach_port_t engine_worker_port;
static _Atomic uint32_t engine_cpu_frames[16];
/* proc_pid_rusage is exported by the xrOS SDK libSystem stub, but that
 * SDK omits libproc.h. Match the libproc declaration; weak import and report
 * errno/ENOSYS if the running OS cannot supply the requested V6 counters. */
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer) __attribute__((weak_import));
extern uint64_t host_present_sleep_ns;   /* shims_kernel32.c */
static HaloFrameSplit engine_frame_split;
/* rusage CPU/QoS/runnable times use Mach units; THREAD_EXTENDED_INFO uses ns. */
static uint64_t engine_mach_ns(uint64_t mach, mach_timebase_info_data_t timebase) {
    return (uint64_t)((__uint128_t)mach * timebase.numer / timebase.denom);
}
static int core_telemetry_on(void) { return __atomic_load_n(&host_core_telemetry, __ATOMIC_RELAXED); }
/* Once per Present, on the engine thread. With the core telemetry off
 * (HALO_CORE_TELEMETRY=0) only the CPU histogram it always kept. */
static void engine_thread_sample(void) {
    size_t cpu = 0;
    if (!pthread_cpu_number_np(&cpu)) atomic_fetch_add_explicit(&engine_cpu_frames[cpu & 15], 1, memory_order_relaxed);
    if (!core_telemetry_on()) return;
    int saved_errno = errno;
    HaloFrameSample sample; memset(&sample, 0, sizeof sample);
    sample.now_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    sample.pass_ns = host_pass_profile_total_ns();
    sample.idle_ns = host_yield_spin_ns;
    sample.sleep_ns = host_present_sleep_ns;
    sample.wait_ns = host_thread_wait_ns; sample.waits = host_thread_waits;
    halo_game_time_read(engine_flat_base, &sample);
    thread_extended_info_data_t info = {0};
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO,
                    (thread_info_t)&info, &count) == KERN_SUCCESS) {
        sample.counts_valid = 1;
        sample.cpu_ns = info.pth_user_time + info.pth_system_time;
    }
    halo_frame_split_add(&engine_frame_split, &sample);
    errno = saved_errno;
}
#include "metalrenderer.h"
#include "frame_pacer.h"
/* The display as the frame pacer (frame_pacer.h) needs it: when the
 * compositor takes the newest frame and how often, from its own leases, and
 * how long a committed frame takes the GPU. Written by the compositor and by
 * Metal's completion thread, read on the engine thread. Atomic slots avoid
 * data races; independent samples may be one update old. These are app
 * submission/publication observations, not display scanout timestamps. */
static _Atomic uint64_t display_latch_ns, display_interval_ns[8], publish_lag_ns[16];
static _Atomic uint32_t display_interval_next, publish_lag_next;
static void display_latch_note(void) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t last = atomic_exchange_explicit(&display_latch_ns, now, memory_order_relaxed);
    if (last && now > last && now - last < 100000000ull) {
        uint32_t i = atomic_fetch_add_explicit(&display_interval_next, 1, memory_order_relaxed) & 7u;
        atomic_store_explicit(&display_interval_ns[i], now - last, memory_order_relaxed);
    }
}
static HaloFramePacerDisplay display_for_pacer(void) {
    HaloFramePacerDisplay d = { atomic_load_explicit(&display_latch_ns, memory_order_relaxed), 0, 0 };
    uint64_t v[16];
    for (int i = 0; i < 8; i++) v[i] = atomic_load_explicit(&display_interval_ns[i], memory_order_relaxed);
    d.period_ns = halo_frame_pacer_median(v, 8);
    for (int i = 0; i < 16; i++) v[i] = atomic_load_explicit(&publish_lag_ns[i], memory_order_relaxed);
    d.lag_ns = halo_frame_pacer_median(v, 16);
    return d;
}
static uint8_t *latest_panorama;
static size_t panorama_capacity;
static EngineVisionPanoramaInfo panorama_info;

bool enginevision_panorama_info(EngineVisionPanoramaInfo *out) {
    if(!out)return false;
    os_unfair_lock_lock(&runtime_lock); *out=panorama_info;
    bool valid=panorama_info.byte_count>0;
    os_unfair_lock_unlock(&runtime_lock); return valid;
}
bool enginevision_copy_panorama(void *destination,size_t capacity,EngineVisionPanoramaInfo *out) {
    if(!out||!destination)return false;
    os_unfair_lock_lock(&runtime_lock); *out=panorama_info;
    bool valid=panorama_info.byte_count>0&&capacity>=panorama_info.byte_count;
    if(valid)memcpy(destination,latest_panorama,panorama_info.byte_count);
    os_unfair_lock_unlock(&runtime_lock); return valid;
}

/* ---- Zero-copy panorama pool ------------------------------------------------
 * Three slots of four BGRA8 textures. The engine thread acquires a free slot,
 * blits its views into it (metalrenderer), and metalwin_present_gpu commits
 * the frame with a completion handler that publishes the slot. The
 * compositor leases the latest published slot per command buffer. */
enum { PANORAMA_GPU_SLOTS = 3, PANORAMA_GPU_LAYERS = HALO_PANORAMA_LAYERS };
enum { GPU_SLOT_FREE = 0, GPU_SLOT_RENDERING = 1, GPU_SLOT_READY = 2, GPU_SLOT_RETIRING = 3 };
typedef struct { id<MTLTexture> textures[PANORAMA_GPU_LAYERS]; uint32_t width, height; int state, leases; EngineVisionPanoramaInfo info; } PanoramaGPUSlot;
static PanoramaGPUSlot gpu_slots[PANORAMA_GPU_SLOTS];
static int gpu_latest = -1, gpu_enabled;
static uint64_t gpu_frames_published, gpu_frames_dropped;
static uint64_t gpu_publish_failed, gpu_carry_copies, gpu_carry_failed;
static uint64_t gpu_flat_sequence, gpu_publish_superseded;
static uint64_t gpu_newest_scene_epoch;
static char gpu_last_error[128];

static void gpu_slot_retire(int index) { /* runtime_lock held */
    PanoramaGPUSlot *s = &gpu_slots[index];
    s->state = s->leases ? GPU_SLOT_RETIRING : GPU_SLOT_FREE;
}
static int gpu_acquire(uint32_t w, uint32_t h, void *textures[HALO_PANORAMA_LAYERS], int *slot) {
    if (!gpu_enabled || !w || !h || w > 8192 || h > 8192) return 0;
    os_unfair_lock_lock(&runtime_lock);
    int pick = -1;
    for (int i = 0; i < PANORAMA_GPU_SLOTS; i++)
        if (gpu_slots[i].state == GPU_SLOT_FREE && gpu_slots[i].leases == 0 && i != gpu_latest) { pick = i; break; }
    if (pick < 0) { gpu_frames_dropped++; os_unfair_lock_unlock(&runtime_lock); return 0; }
    PanoramaGPUSlot *s = &gpu_slots[pick];
    if (s->width != w || s->height != h || !s->textures[PANORAMA_GPU_LAYERS - 1]) {
        id<MTLDevice> device = (__bridge id<MTLDevice>)mr_shared_device();
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        /* Render target too: world layers arrive through the FXAA pass. */
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
        for (int k = 0; k < PANORAMA_GPU_LAYERS; k++) {
            s->textures[k] = device ? [device newTextureWithDescriptor:d] : nil;
            if (!s->textures[k]) { s->width = s->height = 0; os_unfair_lock_unlock(&runtime_lock); return 0; }
            s->textures[k].label = [NSString stringWithFormat:@"Halo panorama GPU slot %d layer %d", pick, k];
        }
        s->width = w; s->height = h;
    }
    s->state = GPU_SLOT_RENDERING;
    for (int k = 0; k < PANORAMA_GPU_LAYERS; k++) textures[k] = (__bridge void *)s->textures[k];
    *slot = pick;
    os_unfair_lock_unlock(&runtime_lock);
    return 1;
}
static void gpu_release(int slot) {
    if (slot < 0 || slot >= PANORAMA_GPU_SLOTS) return;
    os_unfair_lock_lock(&runtime_lock);
    if (gpu_slots[slot].state == GPU_SLOT_RENDERING) gpu_slot_retire(slot);
    os_unfair_lock_unlock(&runtime_lock);
}
/* Bring layers this frame did not redraw across from the last published slot.
 *
 * The engine holds its layers between frames, so the rotating schedule can
 * redraw a few each tick and leave the rest standing. A zero-copy frame has
 * nowhere to stand: every frame publishes a different slot, so an untouched
 * layer would arrive as whatever that slot held two or three frames ago, or
 * black on the first pass through the pool. These copies ride the same
 * command buffer as the engine's own pass blits, so ordering is program
 * order, and a layer that is redrawn afterwards simply overwrites its copy. */
static uint32_t gpu_carry(int slot, uint32_t layer_mask, HaloPanoramaInfo *metadata) {
    if (slot < 0 || slot >= PANORAMA_GPU_SLOTS || !layer_mask || !metadata) return 0;
    uint32_t copied = 0;
    os_unfair_lock_lock(&runtime_lock);
    int source = gpu_latest;
    PanoramaGPUSlot *destination = &gpu_slots[slot];
    int usable = source >= 0 && source < PANORAMA_GPU_SLOTS && source != slot &&
        gpu_slots[source].state == GPU_SLOT_READY && destination->state == GPU_SLOT_RENDERING &&
        gpu_slots[source].info.flat_sequence > gpu_flat_sequence &&
        gpu_slots[source].info.scene_epoch >= gpu_newest_scene_epoch &&
        gpu_slots[source].width == destination->width &&
        gpu_slots[source].height == destination->height && destination->width && destination->height;
    if (usable) {
        const EngineVisionPanoramaInfo *info = &gpu_slots[source].info;
        metadata->width=info->width; metadata->height=info->height;
        metadata->source_epoch=info->source_epoch; metadata->scene_epoch=info->scene_epoch;
        memcpy(metadata->layer_epoch,info->layer_epoch,sizeof metadata->layer_epoch);
        memcpy(metadata->projection_x,info->projection_x,sizeof metadata->projection_x);
        memcpy(metadata->projection_y,info->projection_y,sizeof metadata->projection_y);
        memcpy(metadata->viewport_u_min,info->viewport_u_min,sizeof metadata->viewport_u_min);
        memcpy(metadata->viewport_v_min,info->viewport_v_min,sizeof metadata->viewport_v_min);
        memcpy(metadata->viewport_u_max,info->viewport_u_max,sizeof metadata->viewport_u_max);
        memcpy(metadata->viewport_v_max,info->viewport_v_max,sizeof metadata->viewport_v_max);
        memcpy(metadata->layer_pose,info->layer_pose,sizeof metadata->layer_pose);
        metadata->cut_epoch=info->cut_epoch;
    }
    for (int k = 0; k < PANORAMA_GPU_LAYERS; k++) {
        if (!(layer_mask & (1u << k))) continue;
        if (usable && gpu_slots[source].textures[k] && destination->textures[k]) {
            gpu_carry_copies++;
            if (!mr_blit_texture_copy((__bridge void *)gpu_slots[source].textures[k],
                                      (__bridge void *)destination->textures[k])) {
                copied |= 1u << k;
                continue;
            }
        }
        gpu_carry_failed++;
        snprintf(gpu_last_error, sizeof gpu_last_error, "carry of layer %d from slot %d to %d failed", k, source, slot);
        if (gpu_carry_failed <= 8) host_log("[panorama-gpu] %s", gpu_last_error);
    }
    os_unfair_lock_unlock(&runtime_lock);
    return copied;
}
void enginevision_panorama_gpu_stats(EngineVisionPanoramaGPUStats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    os_unfair_lock_lock(&runtime_lock);
    out->published = gpu_frames_published; out->dropped = gpu_frames_dropped;
    out->publish_failed = gpu_publish_failed;
    out->publish_superseded = gpu_publish_superseded;
    out->carry_copies = gpu_carry_copies; out->carry_failed = gpu_carry_failed;
    out->latest_slot = gpu_latest;
    for (int i = 0; i < PANORAMA_GPU_SLOTS && i < 3; i++) { out->slot_state[i] = gpu_slots[i].state; out->slot_leases[i] = gpu_slots[i].leases; }
    memcpy(out->last_error, gpu_last_error, sizeof out->last_error);
    os_unfair_lock_unlock(&runtime_lock);
}
void enginevision_draw_profile(EngineVisionDrawProfile *out) {
    if (!out) return;
    uint64_t v[12] = {0};
    host_draw_profile_snapshot(v);   /* plain reads of monotonic counters; no lock needed */
    out->passes = v[0]; out->pass_ns = v[1]; out->readback_ns = v[2]; out->readback_calls = v[3];
    out->draws = v[4]; out->vertex_ns = v[5]; out->submit_ns = v[6]; out->upload_ns = v[7];
    out->sleep_calls = v[8]; out->sleep_ns = v[9]; out->yield_calls = v[10];
    out->haptic_onsets = halo_haptics_onset_count();
    out->panorama_extra_half = host_panorama_budget_extra();
    out->panorama_tier = host_panorama_budget_tier();
    out->panorama_busy_seconds = host_panorama_busy_seconds();
    uint64_t pointer_frames = 0; int pointer_x = 0, pointer_y = 0;
    host_pointer_stats(&pointer_frames, &pointer_x, &pointer_y);
    out->pointer_frames = pointer_frames; out->pointer_x = pointer_x; out->pointer_y = pointer_y;
    out->audio_rebuilds = host_dsound_watchdog_rebuilds();
    mach_port_t port = engine_worker_port;
    thread_basic_info_data_t info; mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (port && thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &count) == KERN_SUCCESS)
        out->engine_cpu_ns = ((uint64_t)info.user_time.seconds + (uint64_t)info.system_time.seconds) * 1000000000ull
                           + ((uint64_t)info.user_time.microseconds + (uint64_t)info.system_time.microseconds) * 1000ull;
    for (int i = 0; i < 16; i++) out->engine_cpu_frames[i] = atomic_load_explicit(&engine_cpu_frames[i], memory_order_relaxed);
    static int32_t cores[2] = {-1, -1};
    if (cores[0] < 0) {
        size_t size = sizeof cores[0];
        if (sysctlbyname("hw.perflevel0.logicalcpu", &cores[0], &size, NULL, 0)) cores[0] = 0;
        size = sizeof cores[1];
        if (sysctlbyname("hw.perflevel1.logicalcpu", &cores[1], &size, NULL, 0)) cores[1] = 0;
    }
    out->performance_cores = (uint32_t)cores[0]; out->efficiency_cores = (uint32_t)cores[1];
    mr_program_compile_stats(&out->program_compiles, &out->program_compile_ns);
    mr_pipeline_stats_t pipelines; mr_pipeline_stats(&pipelines);
    out->program_waits = pipelines.waits; out->program_wait_ns = pipelines.wait_ns;
    out->program_background_pipelines = pipelines.background_pipelines;
    out->program_background_functions = pipelines.background_functions;
    out->program_background_ns = pipelines.background_ns;
    out->program_prewarm_hits = pipelines.prewarm_hits; out->program_archive_hits = pipelines.archive_hits;
    out->program_engine_functions = pipelines.engine_functions;
}
bool enginevision_core_telemetry(EngineVisionCoreTelemetry *out) {
    if (!out) return false;
    memset(out, 0, sizeof *out);
    /* Off (or the engine not started yet): no kernel query, and the report
     * leaves the three groups out. */
    if (!core_telemetry_on()) return false;
    HaloFrameSplitTotals t = {0};
    out->snapshot_available = halo_frame_split_read(&engine_frame_split, &t);
    out->frames = t.frames; out->wall_ns = t.wall_ns; out->pass_ns = t.pass_ns; out->idle_ns = t.idle_ns;
    out->wait_ns = t.wait_ns; out->waits = t.waits; out->sleep_ns = t.sleep_ns; out->other_ns = t.other_ns;
    out->cpu_ns = t.cpu_ns; out->off_core_ns = t.off_core_ns; out->unaccounted_ns = t.unaccounted_ns;
    out->outside_pass_ns = t.outside_pass_ns;
    out->counts_frames = t.counts_frames;
    out->stall_outside_pass_ns = t.stall_outside_pass_ns;
    out->stall_tick_unknown_frames = t.stall_tick_unknown_frames;
    out->tick_unknown_frames = t.tick_unknown_frames;
    out->ticks = t.ticks; out->tick_resyncs = t.tick_resyncs; out->tick_mismatch_frames = t.tick_mismatch_frames;
    for (int k = 0; k < HALO_TICK_BUCKETS; k++) { out->tick_frames[k] = t.tick_frames[k]; out->tick_other_ns[k] = t.tick_other_ns[k]; }
    out->max_ticks = t.max_ticks; out->counts_available = t.counts_available;
    out->stall_frames = t.stall_frames; out->stall_ns = t.stall_ns; out->stall_ticks = t.stall_ticks; out->presents = t.presents;
    mach_port_t port = engine_worker_port;
    thread_extended_info_data_t thread = {0}; mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    out->thread_info_result = port ? thread_info(port, THREAD_EXTENDED_INFO, (thread_info_t)&thread, &count) : KERN_INVALID_ARGUMENT;
    if (out->thread_info_result == KERN_SUCCESS) {
        out->thread_info_available = 1;
        out->thread_run_state = thread.pth_run_state; out->thread_flags = thread.pth_flags;
        out->thread_max_priority = thread.pth_maxpriority;
        out->thread_policy = thread.pth_policy; out->thread_priority = thread.pth_curpri;
        out->thread_base_priority = thread.pth_priority; out->thread_cpu_usage = thread.pth_cpu_usage;
        out->thread_run_ns = thread.pth_user_time + thread.pth_system_time;
    }
    struct rusage_info_v6 usage = {0};
    out->process_rusage_result = proc_pid_rusage ?
        (proc_pid_rusage(getpid(), RUSAGE_INFO_V6, (rusage_info_t *)&usage) ? errno : 0) : ENOSYS;
    mach_timebase_info_data_t timebase = {0};
    out->process_timebase_result = mach_timebase_info(&timebase);
    if (out->process_timebase_result == KERN_SUCCESS && (!timebase.numer || !timebase.denom))
        out->process_timebase_result = KERN_FAILURE;
    if (!out->process_rusage_result && out->process_timebase_result == KERN_SUCCESS) {
        out->process_available = 1;
        out->process_cpu_ns = engine_mach_ns(usage.ri_user_time + usage.ri_system_time, timebase);
        out->process_performance_ns = engine_mach_ns(usage.ri_user_ptime + usage.ri_system_ptime, timebase);
        out->process_runnable_ns = engine_mach_ns(usage.ri_runnable_time, timebase);
        out->process_cycles = usage.ri_cycles; out->process_instructions = usage.ri_instructions;
        out->process_performance_cycles = usage.ri_pcycles; out->process_performance_instructions = usage.ri_pinstructions;
        const uint64_t qos[7] = { usage.ri_cpu_time_qos_default, usage.ri_cpu_time_qos_maintenance,
            usage.ri_cpu_time_qos_background, usage.ri_cpu_time_qos_utility, usage.ri_cpu_time_qos_legacy,
            usage.ri_cpu_time_qos_user_initiated, usage.ri_cpu_time_qos_user_interactive };
        for (int k = 0; k < 7; k++) out->process_qos_ns[k] = engine_mach_ns(qos[k], timebase);
        out->process_energy_nj = usage.ri_energy_nj; out->process_performance_energy_nj = usage.ri_penergy_nj;
        out->process_disk_read_bytes = usage.ri_diskio_bytesread; out->process_pageins = usage.ri_pageins;
        out->process_interrupt_wakeups = usage.ri_interrupt_wkups; out->process_idle_wakeups = usage.ri_pkg_idle_wkups;
    }
    task_power_info_v2_data_t power = {0}; count = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&power, &count) == KERN_SUCCESS) {
        out->process_pset_switches_available = 1;
        out->process_pset_switches = power.task_pset_switches;
    }
    return true;
}
void enginevision_pointer(float u, float v, int on_panel, int action) {
    host_pointer_set(u, v, on_panel, action);
}
/* release_ns: when the pacer let the frame go to the GPU; 0 if unknown. */
typedef struct { int slot; EngineVisionPanoramaInfo info; uint64_t release_ns; } PanoramaPublish;
static void gpu_published(void *arg, int ok) {
    PanoramaPublish *p = arg;
    os_unfair_lock_lock(&runtime_lock);
    if (p->slot >= 0 && p->slot < PANORAMA_GPU_SLOTS) {
        PanoramaGPUSlot *s = &gpu_slots[p->slot];
        if (!ok || s->state != GPU_SLOT_RENDERING) {
            gpu_publish_failed++;
            snprintf(gpu_last_error, sizeof gpu_last_error, "publish of slot %d %s (state %d) %s",
                     p->slot, ok ? "found the slot not rendering" : "command buffer failed", s->state,
                     ok ? "" : mr_last_commit_error());
            if (gpu_publish_failed <= 8 || gpu_publish_failed % 256 == 0)
                host_log("[panorama-gpu] %s; failed=%llu published=%llu",
                         gpu_last_error, (unsigned long long)gpu_publish_failed, (unsigned long long)gpu_frames_published);
            gpu_slot_retire(p->slot);
        } else if (p->info.flat_sequence <= gpu_flat_sequence || p->info.scene_epoch < gpu_newest_scene_epoch ||
                   (gpu_latest >= 0 && p->info.sequence <= gpu_slots[gpu_latest].info.sequence)) {
            /* Completion callbacks may reach this lock out of order. Neither
             * an older world frame nor one superseded by a menu may become
             * the newest visible frame when it completes late. */
            gpu_publish_superseded++;
            gpu_slot_retire(p->slot);
        } else {
            int previous = gpu_latest;
            s->state = GPU_SLOT_READY; s->info = p->info; gpu_latest = p->slot;
            if (previous >= 0 && previous != p->slot) gpu_slot_retire(previous);
            panorama_info = p->info; /* state queries see the complete frame; no bytes */
            gpu_frames_published++;
            /* Include completion-handler lock contention in the lead estimate:
             * only now can the compositor lease this completed frame. A failed
             * or superseded frame never supplies a publication-time sample. */
            uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (p->release_ns && now > p->release_ns && now - p->release_ns < 250000000ull) {
                uint32_t i = atomic_fetch_add_explicit(&publish_lag_next, 1, memory_order_relaxed) & 15u;
                atomic_store_explicit(&publish_lag_ns[i], now - p->release_ns, memory_order_relaxed);
            }
        }
    }
    os_unfair_lock_unlock(&runtime_lock);
    free(p);
}
bool enginevision_panorama_gpu_enabled(void) { return gpu_enabled != 0; }
bool enginevision_panorama_gpu_latest(EngineVisionPanoramaGPUSnapshot *out) {
    if (!out) return false;
    os_unfair_lock_lock(&runtime_lock);
    /* Observe the lease after waiting for publication, at the instant the
     * compositor can select its picture. The lock is never held by the pacer. */
    display_latch_note();
    /* A newer flat frame (menu, loading screen) supersedes the last world frame,
     * exactly as the byte path clears its snapshot; an incomplete world frame
     * keeps the last complete panorama on screen. */
    bool ok = gpu_latest >= 0 && gpu_slots[gpu_latest].state == GPU_SLOT_READY &&
              gpu_slots[gpu_latest].info.flat_sequence > gpu_flat_sequence &&
              gpu_slots[gpu_latest].info.scene_epoch >= gpu_newest_scene_epoch;
    if (ok) {
        PanoramaGPUSlot *s = &gpu_slots[gpu_latest];
        s->leases++; out->slot = gpu_latest; out->info = s->info;
        for (int k = 0; k < PANORAMA_GPU_LAYERS; k++) out->textures[k] = (__bridge void *)s->textures[k];
    }
    os_unfair_lock_unlock(&runtime_lock);
    return ok;
}
void enginevision_panorama_gpu_release(int32_t slot) {
    if (slot < 0 || slot >= PANORAMA_GPU_SLOTS) return;
    os_unfair_lock_lock(&runtime_lock);
    PanoramaGPUSlot *s = &gpu_slots[slot];
    if (s->leases > 0) s->leases--;
    if (s->leases == 0 && s->state == GPU_SLOT_RETIRING) s->state = GPU_SLOT_FREE;
    os_unfair_lock_unlock(&runtime_lock);
}
void metalwin_present_gpu(int slot, int width, int height) {
    engine_thread_sample();
    HaloPanoramaInfo incoming; const void *layers[HALO_PANORAMA_LAYERS];
    int complete = host_panorama_frame(&incoming, layers);
    EngineVisionPanoramaInfo info; memset(&info, 0, sizeof info);
    os_unfair_lock_lock(&runtime_lock);
    latest_sequence++;
    if(incoming.scene_epoch>gpu_newest_scene_epoch)gpu_newest_scene_epoch=incoming.scene_epoch;
    info.sequence = info.flat_sequence = latest_sequence;
    info.source_epoch = incoming.source_epoch; info.status = incoming.status; info.failure_reason = incoming.failure_reason;
    info.scene_epoch = incoming.scene_epoch;
    memcpy(info.layer_epoch,incoming.layer_epoch,sizeof incoming.layer_epoch);
    info.stereo = incoming.stereo;
    info.width = width; info.height = height; info.byte_count = 0;
    memcpy(info.projection_x, incoming.projection_x, sizeof incoming.projection_x);
    memcpy(info.projection_y, incoming.projection_y, sizeof incoming.projection_y);
    memcpy(info.viewport_u_min, incoming.viewport_u_min, sizeof incoming.viewport_u_min);
    memcpy(info.viewport_v_min, incoming.viewport_v_min, sizeof incoming.viewport_v_min);
    memcpy(info.viewport_u_max, incoming.viewport_u_max, sizeof incoming.viewport_u_max);
    memcpy(info.viewport_v_max, incoming.viewport_v_max, sizeof incoming.viewport_v_max);
    memcpy(info.layer_pose, incoming.layer_pose, sizeof incoming.layer_pose);
    info.cut_epoch = incoming.cut_epoch;
    runtime_state = ENGINEVISION_RUNNING;
    snprintf(runtime_status, sizeof runtime_status, "Presenting original engine frame %llu (%d x %d, zero-copy).",
             (unsigned long long)latest_sequence, width, height);
    os_unfair_lock_unlock(&runtime_lock);
    PanoramaPublish *p = complete ? calloc(1, sizeof *p) : NULL;
    if (!p) { gpu_release(slot); return; }
    p->slot = slot; p->info = info;
    /* Hold the frame for its slot on an even cadence (off unless enabled);
     * nothing is locked while it waits. */
    HaloFramePacerDisplay display = display_for_pacer();
    p->release_ns = host_frame_pacer_present(&display);
    mr_commit_async(gpu_published, p); /* always invokes gpu_published exactly once */
}

void metalwin_present_dropped(int width, int height) {
    engine_thread_sample();
    HaloPanoramaInfo incoming; const void *layers[HALO_PANORAMA_LAYERS];
    host_panorama_frame(&incoming, layers);
    os_unfair_lock_lock(&runtime_lock);
    latest_sequence++;
    /* Keep the bridge's state honest (incomplete world, no bytes) without
     * touching the last published zero-copy frame. */
    memset(&panorama_info, 0, sizeof panorama_info);
    panorama_info.flat_sequence = latest_sequence; panorama_info.source_epoch = incoming.source_epoch;
    panorama_info.scene_epoch = incoming.scene_epoch;
    if(incoming.scene_epoch>gpu_newest_scene_epoch)gpu_newest_scene_epoch=incoming.scene_epoch;
    panorama_info.status = incoming.status; panorama_info.failure_reason = incoming.failure_reason;
    panorama_info.width = width; panorama_info.height = height;
    runtime_state = ENGINEVISION_RUNNING;
    snprintf(runtime_status, sizeof runtime_status, "Presenting original engine frame %llu (%d x %d, dropped: %u).",
             (unsigned long long)latest_sequence, width, height, incoming.failure_reason);
    gpu_frames_dropped++;
    os_unfair_lock_unlock(&runtime_lock);
}

void enginevision_prepare_controller(void) { hostgc_init(); }

static void set_status(int state, const char *format, ...) {
    char text[sizeof runtime_status];
    va_list args; va_start(args, format); vsnprintf(text, sizeof text, format, args); va_end(args);
    os_unfair_lock_lock(&runtime_lock);
    runtime_state = state;
    snprintf(runtime_status, sizeof runtime_status, "%s", text);
    os_unfair_lock_unlock(&runtime_lock);
}

int enginevision_runtime_state(void) {
    os_unfair_lock_lock(&runtime_lock); int value = runtime_state; os_unfair_lock_unlock(&runtime_lock); return value;
}
int enginevision_exit_code(void) {
    os_unfair_lock_lock(&runtime_lock); int value = runtime_exit; os_unfair_lock_unlock(&runtime_lock); return value;
}
void enginevision_copy_status(char *destination, size_t capacity) {
    if (!destination || !capacity) return;
    os_unfair_lock_lock(&runtime_lock); snprintf(destination, capacity, "%s", runtime_status); os_unfair_lock_unlock(&runtime_lock);
}

bool enginevision_frame_info(EngineVisionFrameInfo *out) {
    if (!out) return false;
    os_unfair_lock_lock(&runtime_lock);
    out->sequence = latest_sequence; out->width = latest_width; out->height = latest_height; out->byte_count = latest_bytes;
    bool present = latest_frame && latest_bytes;
    os_unfair_lock_unlock(&runtime_lock);
    return present;
}

bool enginevision_copy_latest_frame(void *destination, size_t capacity, EngineVisionFrameInfo *out) {
    if (!destination || !out) return false;
    os_unfair_lock_lock(&runtime_lock);
    bool valid = latest_frame && latest_bytes && capacity >= latest_bytes;
    if (valid) memcpy(destination, latest_frame, latest_bytes);
    out->sequence = latest_sequence; out->width = latest_width; out->height = latest_height; out->byte_count = latest_bytes;
    os_unfair_lock_unlock(&runtime_lock);
    return valid;
}

int metalwin_init(int width, int height, const char *title) {
    (void)title;
    if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        set_status(ENGINEVISION_FAILED, "Engine requested an invalid framebuffer size: %d x %d.", width, height);
        return 1;
    }
    set_status(ENGINEVISION_RUNNING, "Renderer ready at %d x %d; waiting for the first engine frame.", width, height);
    return 0;
}

void metalwin_present(const void *bgra, int width, int height) {
    engine_thread_sample();
    if (!bgra || width <= 0 || height <= 0 || width > 8192 || height > 8192) return;
    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / 4) return;
    size_t bytes = pixels * 4;

    os_unfair_lock_lock(&runtime_lock);
    if (latest_capacity < bytes) {
        uint8_t *replacement = realloc(latest_frame, bytes);
        if (!replacement) {
            runtime_state = ENGINEVISION_FAILED;
            snprintf(runtime_status, sizeof runtime_status, "Unable to allocate %zu bytes for the engine frame.", bytes);
            os_unfair_lock_unlock(&runtime_lock);
            host_quit_requested = 1;
            return;
        }
        latest_frame = replacement; latest_capacity = bytes;
    }
    memcpy(latest_frame, bgra, bytes);
    latest_bytes = bytes; latest_width = width; latest_height = height; latest_sequence++;
    HaloPanoramaInfo incoming; const void *layers[HALO_PANORAMA_LAYERS];
    int complete=host_panorama_frame(&incoming,layers);
    memset(&panorama_info,0,sizeof panorama_info);
    panorama_info.flat_sequence=latest_sequence;
    panorama_info.source_epoch=incoming.source_epoch;
    panorama_info.scene_epoch=incoming.scene_epoch;
    if(incoming.scene_epoch>gpu_newest_scene_epoch)gpu_newest_scene_epoch=incoming.scene_epoch;
    memcpy(panorama_info.layer_epoch,incoming.layer_epoch,sizeof incoming.layer_epoch);
    panorama_info.status=incoming.status;panorama_info.failure_reason=incoming.failure_reason;
    if(incoming.status==HALO_PANORAMA_FLAT)gpu_flat_sequence=latest_sequence;
    panorama_info.stereo=incoming.stereo;
    if(complete&&(incoming.width!=(uint32_t)width||incoming.height!=(uint32_t)height)){
        panorama_info.status=HALO_PANORAMA_WORLD_INCOMPLETE;
        panorama_info.failure_reason=HALO_PANORAMA_SIZE_MISMATCH;
        complete=0;
    }
    if(complete) {
        size_t total=bytes*HALO_PANORAMA_LAYERS;
        if(panorama_capacity<total){
            uint8_t *replacement=realloc(latest_panorama,total);
            if(replacement){latest_panorama=replacement;panorama_capacity=total;}
        }
        if(panorama_capacity>=total){
            for(int k=0;k<HALO_PANORAMA_LAYERS;k++)memcpy(latest_panorama+bytes*k,layers[k],bytes);
            panorama_info.sequence=latest_sequence;panorama_info.width=width;panorama_info.height=height;
            panorama_info.byte_count=total;
            memcpy(panorama_info.projection_x,incoming.projection_x,sizeof incoming.projection_x);
            memcpy(panorama_info.projection_y,incoming.projection_y,sizeof incoming.projection_y);
            memcpy(panorama_info.viewport_u_min,incoming.viewport_u_min,sizeof incoming.viewport_u_min);
            memcpy(panorama_info.viewport_v_min,incoming.viewport_v_min,sizeof incoming.viewport_v_min);
            memcpy(panorama_info.viewport_u_max,incoming.viewport_u_max,sizeof incoming.viewport_u_max);
            memcpy(panorama_info.viewport_v_max,incoming.viewport_v_max,sizeof incoming.viewport_v_max);
            memcpy(panorama_info.layer_pose,incoming.layer_pose,sizeof incoming.layer_pose);
            panorama_info.cut_epoch=incoming.cut_epoch;
        }else{
            panorama_info.status=HALO_PANORAMA_WORLD_INCOMPLETE;
            panorama_info.failure_reason=HALO_PANORAMA_ALLOCATION_FAILED;
        }
    }
    runtime_state = ENGINEVISION_RUNNING;
    snprintf(runtime_status, sizeof runtime_status, "Presenting original engine frame %llu (%d x %d, %zu bytes).",
             (unsigned long long)latest_sequence, width, height, bytes);
    os_unfair_lock_unlock(&runtime_lock);
}

int metalwin_should_close(void) { return host_quit_requested != 0; }
void metalwin_poll(void) {}
void enginevision_request_stop(void) {
    host_quit_requested = 1;
    os_unfair_lock_lock(&runtime_lock);
    snprintf(runtime_status, sizeof runtime_status, "Stop requested; waiting for the original engine to reach its next host boundary.");
    os_unfair_lock_unlock(&runtime_lock);
}

#include "audio_session.inc"

static void *engine_worker(void *unused) {
    (void)unused;
    engine_worker_port = pthread_mach_thread_np(pthread_self());
#if TARGET_OS_VISION
    /* Match the verified campaign probe without relying on launch-time shell
     * variables, which a normal headset launch does not supply. */
    const char *settings[][2] = {
        {"HALO_FF3D", "1"},
        {"HALO_PANORAMA", "1"},
        {"HALO_PANORAMA_DENSE", "1"},
        /* Build26/27 original-engine probes verified central weapon history,
         * uncropped world-space arms, firing, reload and checkpoint recovery. */
        {"HALO_PANORAMA_WORLD_FP", "1"},
        /* Exact-token skinning validated against original campaign calls. */
        {"HALO_PV_SKIN_FAST", "1"},
        /* Build82: exact native light/shadow gathers, with per-view visibility.
         * Explicit HALO_NATIVE_GATHER=0 retains the translated comparison path. */
        {"HALO_NATIVE_GATHER", "1"},
        /* Build90: reuse verified static geometry and unchanged encoder state.
         * Geometry ownership is capped at 128 MiB; changed/streamed buffers
         * fall back to arena copies. Explicit 0 retains the comparison path. */
        {"HALO_DRAW_FASTPATH", "1"},
        {"HALO_PAD2KEY", "1"}, {"HALO_A10_AUTOPLAY", "0"},
        {"HALO_UNLOCK_CAMPAIGN", "1"},
        /* Bounded five-second DirectSound/decoder summaries for diagnosing
         * campaign silence on ordinary headset launches. */
        {"HALO_AUDIO_VOICE_TRACE", "1"},
        {"HALO_CTLLOG", "1"}, {"HALO_HSC_TRACE", "120"},
        {"HALO_HSC_TRACE_MAX", "128"},
        /* Core residency, scheduling and frame-split telemetry
         * (CORE_TELEMETRY.md), measurement only. "0" turns it off and
         * restores the pre-telemetry Present, wait and report paths. */
        {"HALO_CORE_TELEMETRY", "1"},
    };
    for (size_t i = 0; i < sizeof settings / sizeof settings[0]; ++i) {
        setenv(settings[i][0], settings[i][1], 0);
        fprintf(stderr, "[device-config] %s=%s\n", settings[i][0], getenv(settings[i][0]));
    }
    /* The initial diagnostics snapshot can query the cached renderer switch
     * before this worker installs its defaults. Apply the resolved setting on
     * the engine thread before any draws, preserving an explicit override. */
    const char *draw_fast = getenv("HALO_DRAW_FASTPATH");
    mr_set_fast_paths(draw_fast && !strcmp(draw_fast, "1"));
    NSString *texturePack = [[NSBundle mainBundle] pathForResource:@"TextureMods" ofType:@"hvt"];
    if(texturePack) setenv("HALO_TEXTURE_PACK", texturePack.fileSystemRepresentation, 0);
    NSString *shaderPack = [[NSBundle mainBundle] pathForResource:@"ShaderMods" ofType:@"hvs"];
    if(shaderPack) setenv("HALO_SHADER_PACK", shaderPack.fileSystemRepresentation, 0);
#endif
    /* Decided once, before the engine runs, so every Present, guest wait and
     * report sees the same answer. On unless HALO_CORE_TELEMETRY=0. */
    { const char *telemetry = getenv("HALO_CORE_TELEMETRY");
      __atomic_store_n(&host_core_telemetry, !(telemetry && telemetry[0] == '0'), __ATOMIC_RELAXED); }
    set_status(ENGINEVISION_STARTING, "Loading Halo and preparing the renderer.");
    int result = host_run(runtime_exe, runtime_root);
    os_unfair_lock_lock(&runtime_lock); runtime_exit = result; os_unfair_lock_unlock(&runtime_lock);
    if (host_quit_requested && result == 0) set_status(ENGINEVISION_STOPPED, "Engine stopped after the app requested shutdown. Relaunch the app to run it again.");
    else if (result == 0) set_status(ENGINEVISION_STOPPED, "Original engine exited normally. Relaunch the app to run it again.");
    else set_status(ENGINEVISION_FAILED, "Original engine stopped with host status %d. Review the device console for the host trace.", result);
    return NULL;
}

int enginevision_start(const char *game_root) {
    if (!game_root || !game_root[0]) return EINVAL;
#if TARGET_OS_VISION
    /* Set the original engine's mode before it builds projection/HUD state.
     * 4:3 keeps the HUD layout. 2560x1920 (Build30) matched the panel density
     * but the M2 could not fill three such views per frame at a usable rate;
     * 1920x1440 is 2.25x the pixels of 1280x960 instead of 4x. Stereo then
     * doubled the number of world passes, so the default stepped down again.
     * Removing the duplicate view the game asked for then cut engine work per
     * pass four-fold, and pixels cost the GPU rather than the engine that
     * limits this port, so the mode moves back up to 2048x1536 to spend the
     * recovered budget on sharpness. HALO_VIDMODE overrides it. */
    { const char *mode = getenv("HALO_VIDMODE"); char buf[64]; char chosen[32] = "";
      /* The settings window stores a resolution the player picked; it takes
       * effect at the next launch because the engine fixes its back buffer
       * when it creates the device. The headset's engine ran at four frames
       * a second in play at 2048x1536 while the Mac ran at sixteen at
       * 1280x960, and whether pixels are the reason is exactly what a
       * resolution the player can change without a rebuild will tell. */
      NSString *stored = [[NSUserDefaults standardUserDefaults] stringForKey:@"HaloRenderResolution"];
      if (stored.length && stored.length < sizeof chosen) {
          unsigned w = 0, h = 0;
          if (sscanf(stored.UTF8String, "%ux%u", &w, &h) == 2 && w >= 640 && h >= 480 && w <= 4096 && h <= 3072)
              snprintf(chosen, sizeof chosen, "%u,%u,60", w, h);
      }
      snprintf(buf, sizeof buf, "-vidmode %s", mode && mode[0] ? mode : (chosen[0] ? chosen : "2048,1536,60"));
      setenv("HALO_CMDLINE_EXTRA", buf, 0); }
#endif
    /* Zero-copy panorama hand-off. The headset takes it unless
     * HALO_PANORAMA_GPU=0 asks for the readback path; the Mac leaves it off
     * unless HALO_PANORAMA_GPU=1 asks for it, because the desktop probe
     * presents from the CPU copy. Reachable on both on purpose: while this
     * path ran only on the headset, a lease that dropped three of the ten
     * layers could not be caught anywhere cheap, and shipped as a black
     * screen that every desktop composite said was fine. */
    { const char *flag = getenv("HALO_PANORAMA_GPU");
#if TARGET_OS_VISION
      gpu_enabled = !(flag && flag[0] == '0');
#else
      gpu_enabled = flag && flag[0] && flag[0] != '0';
#endif
      if (gpu_enabled) { static const HaloPanoramaGPUSink sink = { gpu_acquire, gpu_release, gpu_carry }; host_panorama_set_gpu_sink(&sink); } }
    { const char *extra = getenv("HALO_CMDLINE_EXTRA");
      if (extra && extra[0] && !strstr(host_cmdline_buf, extra)) {
          size_t n = strlen(host_cmdline_buf);
          snprintf(host_cmdline_buf + n, sizeof host_cmdline_buf - n, " %s", extra); } }
    os_unfair_lock_lock(&runtime_lock);
    if (runtime_state != ENGINEVISION_IDLE) { os_unfair_lock_unlock(&runtime_lock); return EALREADY; }
    if (snprintf(runtime_root, sizeof runtime_root, "%s", game_root) >= (int)sizeof runtime_root ||
        snprintf(runtime_exe, sizeof runtime_exe, "%s/halo.exe", game_root) >= (int)sizeof runtime_exe) {
        runtime_state = ENGINEVISION_FAILED; snprintf(runtime_status, sizeof runtime_status, "Imported game path is too long.");
        os_unfair_lock_unlock(&runtime_lock); return ENAMETOOLONG;
    }
    runtime_state = ENGINEVISION_STARTING;
    snprintf(runtime_status, sizeof runtime_status, "Creating the original-engine worker thread.");
    os_unfair_lock_unlock(&runtime_lock);

#if TARGET_OS_VISION
    @autoreleasepool { audio_session_prepare(); }
#endif
    host_trace_imports = 0; host_quit_requested = 0;
    pthread_attr_t attr;
    int error = pthread_attr_init(&attr);
    int attr_initialized = error == 0;
    if (!error) error = pthread_attr_setstacksize(&attr, (size_t)1536 * 1024 * 1024);
    /* The engine thread is the game. Created with no class it ran as an
     * ordinary default-priority thread beside a compositor that renders at
     * 90 Hz at user-interactive priority, eligible for the efficiency cores
     * and first to lose the performance ones. The headset's engine ran at
     * four frames a second in play while the Mac, with an idle compositor,
     * ran at sixteen, and drawing half the bearings changed nothing there:
     * the thread was not short of work to skip, it was short of a core. */
    if (!error) error = pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
    size_t accepted = 0;
    if (!error) error = pthread_attr_getstacksize(&attr, &accepted);
    if (!error && accepted != (size_t)1536 * 1024 * 1024) error = EINVAL;
    if (error) {
        if (attr_initialized) pthread_attr_destroy(&attr);
        set_status(ENGINEVISION_FAILED, "visionOS rejected the required 1.5 GB engine worker stack (%s, code %d). No unsafe fallback was started.", strerror(error), error);
        return error;
    }
    pthread_t worker;
    error = pthread_create(&worker, &attr, engine_worker, NULL);
    pthread_attr_destroy(&attr);
    if (error) {
        set_status(ENGINEVISION_FAILED, "Unable to create the 1.5 GB engine worker thread (%s, code %d). No unsafe fallback was started.", strerror(error), error);
        return error;
    }
    pthread_detach(worker);
    return 0;
}

void enginevision_frame_digest(EngineFrameDigest *out) {
    if(!out)return;memset(out,0,sizeof *out);
    os_unfair_lock_lock(&runtime_lock);out->sequence=latest_sequence;
    if(latest_frame && latest_bytes>=4) {
        size_t pixels=latest_bytes/4,step=pixels>1024?pixels/1024:1;
        uint32_t first;memcpy(&first,latest_frame,4);first&=0xFFFFFF;
        out->hash=14695981039346656037ULL;
        for(size_t i=0;i<pixels && out->sample_count<1024;i+=step){
            uint32_t value;memcpy(&value,latest_frame+i*4,4);value&=0xFFFFFF;
            out->sample_count++;out->nonblack_count+=value!=0;out->differing_count+=value!=first;
            out->hash=(out->hash^value)*1099511628211ULL;
        }
    }
    os_unfair_lock_unlock(&runtime_lock);
}

int host_dinput_menu_active(void);
int enginevision_menu_active(void) { return host_dinput_menu_active(); }
