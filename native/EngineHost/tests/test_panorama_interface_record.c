/* The game's interface record, run through the lifted original renderer.
 *
 * Executes the translated 0050BEA0 (the renderer's record loop), 0050BDC0
 * (the interface record, and a player view with no camera), 0050CC40 (the
 * projection builder) and 005175C0 (view setup) with their callees, under
 * the production panorama hook, in a flat fake guest memory with a fake
 * Direct3D device whose methods only record their calls. The world view
 * (0050BA80) and the 2D entries 0050BDC0 calls are recorders.
 *
 * Checks that only the left-eye centre pass hands 0050BEA0 the game's record
 * count; that the interface then runs once a frame, after the world view,
 * with EAX 0 and every 2D entry inside the HUD routing; that it clears
 * nothing and reports no projection, even with HALO_PANORAMA_DENSE; that the
 * interface record keeps the camera the game built and leaves it in the view
 * globals; and that a player view with no camera (EAX 1) stays a world view.
 * Times a frame with the record against one without it.
 *
 * Needs the generated engine sources on the include path, which
 * tools/run_source_checks.py adds when it finds them. Prints SKIP without
 * them. */
#if !__has_include("sub_0050BEA0.c")
#include <stdio.h>
int main(void) { puts("SKIP: generated engine sources (sub_0050BEA0.c) are not on the include path"); return 0; }
#else
#include "../host.h"
#include "../halo_settings.h"
#include "../panorama.h"
/* Options change between the cases below; production caches them. */
#undef HOST_ENV
#define HOST_ENV(name) getenv(name)
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "engine_flags.h"
#include "engine_functions.h"
#include "engine_registers.h"
#include "sub_0050BEA0.c"
#include "sub_0050BDC0.c"
#include "sub_0050CC40.c"
#include "sub_00401990.c"
#include "sub_004CB7A0.c"
#include "sub_004CBDE0.c"
#include "sub_004CBF10.c"
#include "sub_005175C0.c"
#include "sub_005176D0.c"
#include "sub_0044D9E0.c"
#include "sub_004AB5D0.c"
#include "sub_00518F40.c"
#include "sub_0050C9A0.c"
#include "sub_00519200.c"
#include "sub_00526700.c"
#include "sub_0052CCC0.c"
#undef eax
#undef ecx
#undef edx
#undef ebx
#undef esp
#undef ebp
#undef esi
#undef edi
#undef eflags

uint8_t *engine_flat_base;
uint32_t engine_trace_lo, engine_trace_hi;
void engine_pc_trace(EngineCPU *cpu) { (void)cpu; }
void engine_record(uint32_t address) { (void)address; }

/* Guest layout: the game's own globals, plus a stack, a fake device and a
 * fake render-target surface in memory the game does not use here. */
enum { MEMORY = 0x900000 };
static const uint32_t STACK = 0x00020000u, RECORDS = 0x00719B70u, CALLER = 0x004C9515u;
static const uint32_t DEVICE = 0x00890000u, DEVICE_VTABLE = 0x00890100u;
static const uint32_t SURFACE = 0x00890800u, SURFACE_VTABLE = 0x00890900u;
static const uint32_t DEVICE_THUNK = 0xF0000000u, SURFACE_THUNK = 0xF1000000u;
/* IDirect3DDevice9 argument counts, this included (d3d9.c m_dev). */
static const uint8_t device_argc[119] = {
    3,1,1,1,1,1,2,2,3,2,4,4,2,3,3,1,2,5,5,3,2,4,3,9,10,8,7,7,9,9,5,3,3,3,6,4,7,3,3,2,2,1,1,7,3,3,3,2,2,2,
    2,3,3,3,3,3,3,3,3,3,1,2,2,2,3,3,4,4,4,4,2,3,3,2,2,2,2,2,1,2,1,4,7,5,9,7,3,2,2,2,2,3,2,2,4,4,4,4,4,4,
    5,5,3,3,2,2,3,2,2,4,4,4,4,4,4,4,4,2,3 };
enum { SET_RENDER_TARGET = 37, CLEAR = 43, SET_VIEWPORT = 47 };

/* What the frame did, per layer. */
static int active_pass = -1, ui_active;
static unsigned begins, aborts, ui_on;
static unsigned count_seen[HALO_PANORAMA_LAYERS], world_views[HALO_PANORAMA_LAYERS];
static unsigned interface_entries[HALO_PANORAMA_LAYERS], camera_less_entries[HALO_PANORAMA_LAYERS];
static unsigned entries_2d[HALO_PANORAMA_LAYERS], entries_2d_routed[HALO_PANORAMA_LAYERS];
static unsigned camera_less_2d[HALO_PANORAMA_LAYERS];
static unsigned clears[HALO_PANORAMA_LAYERS], clears_in_interface, targets_in_interface, viewports_in_interface;
static uint32_t interface_target;
static unsigned world_reports[HALO_PANORAMA_LAYERS], camera_less_reports[HALO_PANORAMA_LAYERS], other_reports;
static unsigned interface_builds[HALO_PANORAMA_LAYERS];
static int interface_scope, world_seen_in_pass, interface_after_world = 1;
static unsigned composites[HALO_PANORAMA_LAYERS], interface_before_composite = 1;
static unsigned char interface_frustum[0x54], interface_view_record[8];
static uint64_t dispatched;

static void clear_frame(void) {
    active_pass = -1; ui_active = 0; begins = aborts = ui_on = 0;
    memset(count_seen, 0, sizeof count_seen); memset(world_views, 0, sizeof world_views);
    memset(interface_entries, 0, sizeof interface_entries); memset(camera_less_entries, 0, sizeof camera_less_entries);
    memset(entries_2d, 0, sizeof entries_2d); memset(entries_2d_routed, 0, sizeof entries_2d_routed);
    memset(camera_less_2d, 0, sizeof camera_less_2d);
    memset(clears, 0, sizeof clears); clears_in_interface = targets_in_interface = viewports_in_interface = 0;
    interface_target = 0;
    memset(world_reports, 0, sizeof world_reports); memset(camera_less_reports, 0, sizeof camera_less_reports);
    other_reports = 0; memset(interface_builds, 0, sizeof interface_builds);
    interface_scope = world_seen_in_pass = 0; interface_after_world = 1;
    memset(composites, 0, sizeof composites); interface_before_composite = 1;
}

void host_log(const char *format, ...) { (void)format; }
void host_panorama_reset(void) {}
void host_panorama_invalidate(void) {}
int host_panorama_camera_moving(void){return 0;}
void host_panorama_set_camera(const float *pose) { (void)pose; }
void host_panorama_abort(void) { aborts++; }
void host_panorama_begin(int pass) { assert(active_pass == -1); active_pass = pass; begins++; world_seen_in_pass = 0; }
void host_panorama_end(int pass) { assert(active_pass == pass && !ui_active); active_pass = -1; }
void host_panorama_viewmodel(int active) { (void)active; }
void host_panorama_ui(int active) { assert(active != ui_active); ui_active = active; if (active) ui_on++; }
void host_panorama_projection(float m11, float m22, int x, int y, int w, int h, uint32_t caller) {
    (void)m11; (void)m22; (void)x; (void)y;
    assert(active_pass >= 0 && !ui_active && w == 640 && h == 480);
    if (caller == 0x0050BC8Bu) world_reports[active_pass]++;
    else if (caller == 0x0050BE01u || caller == 0x0050BE23u) {
        assert(!interface_scope);   /* never from the interface record */
        camera_less_reports[active_pass]++;
    } else other_reports++;
}
uint64_t host_yield_spin_ns;
int host_panorama_all_layers_ready(void) { return 0; }
void host_panorama_set_stereo(int on) { (void)on; }
int host_pass_profile_enabled(void) { return 0; }
void host_pass_profile_add(uint64_t ns) { (void)ns; }
static void ret_to_caller(EngineCPU *cpu) { cpu->pc = engine_pop(cpu, 4); }
#define OARG(i) G32(cpu->gpr[4] + 4u + 4u * (uint32_t)(i))
static int host_panorama_dispatch(EngineCPU *cpu, uint32_t address);
/* These tests keep the unpaced bearing target. */
float host_frame_pacer_budget_target(float fallback) { return fallback; }
#include "../panorama_hooks.inc"

/* Plain-return recorders for the direct callees the lifted code reaches. */
static void plain_return(EngineCPU *cpu) { cpu->pc = engine_pop(cpu, 4); }
void sub_00511DF0(EngineCPU *cpu) { plain_return(cpu); }                       /* render time */
void sub_00517500(EngineCPU *cpu) { cpu->gpr[0] = 1; plain_return(cpu); }      /* frame may render */
void sub_0049C870(EngineCPU *cpu) { plain_return(cpu); }
void sub_00518130(EngineCPU *cpu) { plain_return(cpu); }
void sub_00517B90(EngineCPU *cpu) {                                            /* RT1 -> RT0 composite */
    assert(active_pass >= 0 && !ui_active); composites[active_pass]++; plain_return(cpu);
}
static void record_2d(EngineCPU *cpu) {
    assert(active_pass >= 0);
    if (interface_scope) { entries_2d[active_pass]++; if (ui_active) entries_2d_routed[active_pass]++; }
    plain_return(cpu);
}
void sub_004499C0(EngineCPU *cpu) { record_2d(cpu); }   /* letterbox and chapter titles */
void sub_00494CA0(EngineCPU *cpu) { record_2d(cpu); }   /* split-screen dividers */
void sub_004ADD10(EngineCPU *cpu) { record_2d(cpu); }   /* game timer */
void sub_00497410(EngineCPU *cpu) { record_2d(cpu); }   /* error and loading modal */
void sub_00496730(EngineCPU *cpu) { record_2d(cpu); }   /* messages */
void sub_00512530(EngineCPU *cpu) { if (interface_scope) record_2d(cpu); else plain_return(cpu); } /* framerate */
void sub_00512E80(EngineCPU *cpu) { if (interface_scope) record_2d(cpu); else plain_return(cpu); } /* framerate */
void sub_00461A80(EngineCPU *cpu) {                     /* the no-camera view's picture */
    assert(!interface_scope && !ui_active); camera_less_2d[active_pass]++; plain_return(cpu);
}
/* The world view: the builder call a world view makes (0050BA80 at
 * 0050BC86, returning to 0050BC8B), through the hook, then a plain return. */
void sub_0050BA80(EngineCPU *cpu) {
    assert(active_pass >= 0 && !ui_active && !interface_scope);
    uint32_t record = G32(cpu->gpr[4] + 4);
    assert(record == RECORDS);   /* the player's view is always record 0 */
    world_views[active_pass]++; world_seen_in_pass = 1;
    EngineCPU call = *cpu;
    call.gpr[4] -= 0x400; engine_push(&call, 1, 4); engine_push(&call, 0x0050BC8Bu, 4);
    call.gpr[1] = record + 4; call.gpr[6] = 0x00030000u; call.gpr[0] = 0;
    engine_dispatch(&call, 0x0050CC40u);
    assert(call.pc == 0x0050BC8Bu);
    plain_return(cpu);
}

void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    dispatched++;
    if (address >= DEVICE_THUNK && address < DEVICE_THUNK + 4u * 119u) {
        unsigned method = (address - DEVICE_THUNK) / 4u;
        uint32_t return_address = engine_pop(cpu, 4), args[10] = {0};
        for (unsigned k = 0; k < device_argc[method]; k++) args[k] = G32(cpu->gpr[4] + 4u * k);
        assert(args[0] == DEVICE);
        cpu->gpr[4] += 4u * device_argc[method];
        if (method == CLEAR) { if (active_pass >= 0) clears[active_pass]++; if (interface_scope) clears_in_interface++; }
        if (method == SET_RENDER_TARGET && interface_scope) { targets_in_interface++; interface_target = args[2]; }
        if (method == SET_VIEWPORT && interface_scope) viewports_in_interface++;
        cpu->gpr[0] = 0; cpu->pc = return_address; return;
    }
    if (address >= SURFACE_THUNK && address < SURFACE_THUNK + 4u * 20u) {
        unsigned method = (address - SURFACE_THUNK) / 4u;
        uint32_t return_address = engine_pop(cpu, 4);
        assert(method == 12 && G32(cpu->gpr[4]) == SURFACE);   /* GetDesc */
        uint32_t desc = G32(cpu->gpr[4] + 4); S32(desc + 24, 640); S32(desc + 28, 480);
        cpu->gpr[4] += 8; cpu->gpr[0] = 0; cpu->pc = return_address; return;
    }
    /* engine_dispatch_override consults the panorama hook first. */
    if (host_panorama_dispatch(cpu, address)) return;
    cpu->pc = address;
    switch (address) {
    case 0x0050BEA0u:
        count_seen[active_pass] = G32(cpu->gpr[4] + 8) & 0xFFFFu;
        sub_0050BEA0(cpu); return;
    case 0x0050BA80u: sub_0050BA80(cpu); return;
    case 0x0050BDC0u: {
        assert(active_pass >= 0);
        uint32_t record = G32(cpu->gpr[4] + 4);
        if (cpu->gpr[0] == 0) {
            /* The interface record: routed, after the world view, once. */
            assert(record == RECORDS + 0xACu && ui_active && panorama_ui_nested);
            if (!world_seen_in_pass) interface_after_world = 0;
            if (composites[active_pass]) interface_before_composite = 0;
            interface_entries[active_pass]++;
            interface_scope = 1; sub_0050BDC0(cpu); interface_scope = 0;
            memcpy(interface_view_record, GPTR(0x007C1220u), sizeof interface_view_record);
        } else {
            assert(cpu->gpr[0] == 1 && record == RECORDS && !ui_active && !panorama_ui_nested);
            camera_less_entries[active_pass]++;
            sub_0050BDC0(cpu);
        }
        return;
    }
    case 0x0050CC40u: {
        uint32_t caller = G32(cpu->gpr[4]);
        if (interface_scope) {
            /* Unhooked and unnarrowed: the rectangle is the game's. */
            assert(caller == 0x0050BE01u || caller == 0x0050BE23u);
            assert(!panorama_projection_nested);
            int16_t rect[4]; memcpy(rect, GPTR(cpu->gpr[1] + 0x2C), sizeof rect);
            assert(rect[0] == 0 && rect[1] == 0 && rect[2] == 480 && rect[3] == 640);
            interface_builds[active_pass]++;
        }
        sub_0050CC40(cpu); return;
    }
    case 0x00518F40u: sub_00518F40(cpu); return;
    }
    fprintf(stderr, "unexpected dispatch %08x from %08x\n", address, G32(cpu->gpr[4]));
    abort();
}

/* One frame as 004C9260 hands it over: count = players + 1, the player's
 * view first, then the interface record; EBX is zero (004C9509). */
static void write_frustum(uint32_t at, const float position[3], const float forward[3], float fov) {
    const float up[3] = {0, 0, 1};
    memcpy(GPTR(at), position, 12); memcpy(GPTR(at + 0x0C), forward, 12); memcpy(GPTR(at + 0x18), up, 12);
    memcpy(GPTR(at + 0x28), &fov, 4);
    int16_t rect[4] = {0, 0, 480, 640}; memcpy(GPTR(at + 0x2C), rect, sizeof rect);
}
static unsigned char records_before[2 * 0xAC], stack_before[24];
static EngineCPU frame(int camera_less, unsigned count) {
    memset(engine_flat_base, 0, MEMORY);
    S32(0x0071D174u, DEVICE); S32(DEVICE, DEVICE_VTABLE);
    for (unsigned k = 0; k < 119; k++) S32(DEVICE_VTABLE + 4 * k, DEVICE_THUNK + 4 * k);
    S32(SURFACE, SURFACE_VTABLE);
    for (unsigned k = 0; k < 20; k++) S32(SURFACE_VTABLE + 4 * k, SURFACE_THUNK + 4 * k);
    S32(0x0069D364u, SURFACE); S32(0x0069D378u, SURFACE + 0x40);   /* RenderTargets[0], [1] */
    S16(0x006893E4u, 1);   /* 005175C0 then passes a clear colour of zero */
    const float player[3] = {10, 20, 30}, ahead[3] = {1, 0, 0};
    const float hud_origin[3] = {-7, 8, 9}, hud_ahead[3] = {0, 1, 0};
    S16(RECORDS, camera_less ? 0xFFFFu : 0); S8(RECORDS + 2, 0);
    write_frustum(RECORDS + 4, player, ahead, 1.2f); write_frustum(RECORDS + 0x58, player, ahead, 1.2f);
    S16(RECORDS + 0xAC, 0xFFFFu); S8(RECORDS + 0xAC + 2, 1);
    write_frustum(RECORDS + 0xAC + 4, hud_origin, hud_ahead, 1.1f);
    write_frustum(RECORDS + 0xAC + 0x58, hud_origin, hud_ahead, 1.1f);
    memcpy(records_before, GPTR(RECORDS), sizeof records_before);
    memcpy(interface_frustum, GPTR(RECORDS + 0xAC + 4), sizeof interface_frustum);
    S32(STACK, CALLER); S32(STACK + 4, RECORDS); S32(STACK + 8, count); S32(STACK + 12, 0);
    float remainder = 0.25f, step = 1.f / 30.f;
    memcpy(GPTR(STACK + 16), &remainder, 4); memcpy(GPTR(STACK + 20), &step, 4);
    memcpy(stack_before, GPTR(STACK), sizeof stack_before);
    EngineCPU cpu; memset(&cpu, 0, sizeof cpu);
    cpu.pc = 0x0050BEA0u; cpu.gpr[4] = STACK; cpu.gpr[3] = 0; cpu.fp_control = 0x027F; cpu.flags = 0x202;
    return cpu;
}
static void run_frame(EngineCPU *cpu) {
    clear_frame();
    assert(host_panorama_dispatch(cpu, 0x0050BEA0u) == 1);
    assert(cpu->pc == CALLER && cpu->gpr[4] == STACK + 4 && aborts == 0 && active_pass == -1 && !ui_active);
    assert(!memcmp(GPTR(RECORDS), records_before, sizeof records_before));
    assert(!memcmp(GPTR(STACK), stack_before, sizeof stack_before));
}
static const int mono_passes[] = {0, 2, 5, 6, 7, 8, 9, 1};
static const int stereo_passes[] = {0, 2, 5, 6, 7, 8, 9, 4, 1};

/* The frame every mode shares: the player's view in every bearing, the
 * interface once, last, in the left-eye centre pass, routed to the HUD. */
static void check_interface_frame(const int *passes, unsigned n, int camera_less, int views_all) {
    assert(begins == n);
    unsigned drawn = 0;
    for (unsigned k = 0; k < n; k++) drawn |= 1u << passes[k];
    for (int layer = 0; layer < HALO_PANORAMA_LAYERS; layer++) {
        unsigned in = (drawn >> layer) & 1u, hud = layer == HALO_PANORAMA_CENTRE_LEFT;
        assert(count_seen[layer] == (in ? (hud || views_all ? 2u : 1u) : 0u));
        assert(interface_entries[layer] == (in && hud));
        assert(entries_2d[layer] == 7u * (in && hud) && entries_2d_routed[layer] == entries_2d[layer]);
        assert(world_views[layer] == (in && !camera_less));
        assert(camera_less_entries[layer] == (in && camera_less));
        assert(camera_less_2d[layer] == (in && camera_less));
        /* A view with no camera clears its world target and reports its
         * projection from 0050BDC0; nothing else clears. */
        assert(clears[layer] == (in && camera_less));
        assert(camera_less_reports[layer] == 2u * (in && camera_less));
        assert(world_reports[layer] == (in && !camera_less));
        assert(interface_builds[layer] == 2u * (in && hud));
        assert(composites[layer] == in);
    }
    assert(!other_reports && interface_before_composite);
    if (!camera_less) assert(interface_after_world);
    /* 005175C0 bound Halo's scene target, set the viewport and cleared
     * nothing: byte +5 of the view record is set, and 005175C0 clears only
     * when it is not. */
    assert(clears_in_interface == 0 && targets_in_interface == 1 && viewports_in_interface == 1);
    assert(interface_target == SURFACE + 0x40);
    assert(G16(0x0069D350u) == 1);
    uint16_t word0, word1; memcpy(&word0, interface_view_record, 2); memcpy(&word1, interface_view_record + 2, 2);
    assert(word0 == 1 && word1 == 0xFFFFu && interface_view_record[5] == 1);
    /* The interface record kept its own camera, and the view globals end
     * the frame holding it, as they do without the panorama. */
    assert(!memcmp(GPTR(0x007C3114u), interface_frustum, sizeof interface_frustum));
}

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }

int main(void) {
    engine_flat_base = calloc(MEMORY, 1); assert(engine_flat_base);
    setenv("HALO_PANORAMA", "1", 1);
    setenv("HALO_PANORAMA_DENSE", "1", 1);
    setenv("HALO_STEREO", "0", 1);
    unsetenv("HALO_PANORAMA_OVERLAY_ALL_VIEWS"); unsetenv("HALO_PANORAMA_WORLD_FP");
    /* HALO_PANORAMA_VIEWS is read once per process: check it in a child. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        setenv("HALO_PANORAMA_VIEWS", "all", 1);
        EngineCPU cpu = frame(0, 2); run_frame(&cpu);
        check_interface_frame(mono_passes, 8, 0, 1);
        _exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    unsetenv("HALO_PANORAMA_VIEWS");

    /* Mono, stereo, and a player view with no camera. */
    EngineCPU cpu = frame(0, 2); run_frame(&cpu);
    check_interface_frame(mono_passes, 8, 0, 0);
    HaloSettings settings; halo_settings_get(&settings);
    /* Stereo, with the budget's shared-centre tiers out of the way so the
     * right-eye centre pass is drawn: it gets one record like the rest. */
    settings.stereo_separation = 0.1f; halo_settings_set(&settings);
    setenv("HALO_PANORAMA_TIGHT_BUDGET", "0", 1);
    cpu = frame(0, 2); run_frame(&cpu);
    check_interface_frame(stereo_passes, 9, 0, 0);
    unsetenv("HALO_PANORAMA_TIGHT_BUDGET");
    settings.stereo_separation = 0.f; halo_settings_set(&settings);
    cpu = frame(1, 2); run_frame(&cpu);
    check_interface_frame(mono_passes, 8, 1, 0);

    /* A frame of one record, which is what every bearing used to get:
     * nothing of the interface runs anywhere. */
    cpu = frame(0, 1); run_frame(&cpu);
    for (int layer = 0; layer < HALO_PANORAMA_LAYERS; layer++)
        assert(interface_entries[layer] == 0 && entries_2d[layer] == 0 && interface_builds[layer] == 0);

    /* Cost of the record: the same mono frame with and without it. The
     * world view and the 2D entries are recorders here, so the difference
     * is the lifted 0050BDC0, its two projection builds, 005175C0's view
     * setup and the hook's routing, which is what the interface adds to a
     * frame before any of its 2D has something to show. */
    enum { FRAMES = 1000, ROUNDS = 9 };
    uint64_t best[2] = {UINT64_MAX, UINT64_MAX}, calls[2] = {0, 0}, instructions[2] = {0, 0};
    for (int round = 0; round < ROUNDS; round++) for (unsigned with = 0; with < 2; with++) {
        EngineCPU start_cpu = frame(0, with ? 2u : 1u);
        uint64_t total = 0, before = dispatched;
        for (unsigned k = 0; k < FRAMES; k++) {
            /* The hook restores the records and the stack after every frame. */
            cpu = start_cpu; clear_frame();
            uint64_t start = now_ns();
            host_panorama_dispatch(&cpu, 0x0050BEA0u);
            total += now_ns() - start;
            assert(cpu.pc == CALLER && begins == 8 && interface_entries[1] == with);
        }
        if (total < best[with]) best[with] = total;
        calls[with] = (dispatched - before) / FRAMES;
        instructions[with] = cpu.instruction_count - start_cpu.instruction_count;
    }
    double without_us = best[0] / 1000.0 / FRAMES, with_us = best[1] / 1000.0 / FRAMES;
    printf("timing (best of %d rounds of %d frames): mono frame of 8 passes with recorded world views %.2f us without the interface record, %.2f us with it: %+.2f us per frame; dispatches per frame %llu -> %llu",
           ROUNDS, FRAMES, without_us, with_us, with_us - without_us, (unsigned long long)calls[0], (unsigned long long)calls[1]);
    if (instructions[1]) printf("; lifted instructions per frame %llu -> %llu", (unsigned long long)instructions[0], (unsigned long long)instructions[1]);
    putchar('\n');
    free(engine_flat_base);
    puts("PASS: lifted 0050BEA0 gets the game's count only in the left-eye centre pass; the interface runs there once, after the world view, routed to the HUD, clearing nothing and reporting no projection; its record keeps the game's camera; a camera-less view stays a world view");
    return 0;
}
#endif
