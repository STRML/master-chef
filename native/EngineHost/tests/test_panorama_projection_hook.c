/* Focused ABI test for the opt-in projection-only dense panorama hook. */
#include "../host.h"
#include "../halo_settings.h"
/* Exercise option combinations in one fixture; production caches options for
 * process lifetime. This test intentionally does not test that cache. */
#undef HOST_ENV
#define HOST_ENV(name) getenv(name)
#include <assert.h>
#include <math.h>
#include <stdlib.h>

uint8_t *engine_flat_base;
static unsigned dispatch_count;
static int inspect_dense_rect;
static int recurse_projection;
static int ui_active;
static unsigned ui_changes;
static unsigned projection_reports, interface_builds;
static uint32_t reported_caller;
static float reported_m11, reported_m22;
static int reported_x, reported_y, reported_w, reported_h;
static int host_panorama_dispatch(EngineCPU *cpu, uint32_t address);
static void ret_to_caller(EngineCPU *cpu) { cpu->pc = engine_pop(cpu, 4); }

void host_log(const char *format, ...) { (void)format; }
void host_panorama_reset(void) {}
void host_panorama_invalidate(void) {}
void host_panorama_abort(void) {}
void host_panorama_begin(int pass) { (void)pass; }
int host_panorama_camera_moving(void){return 0;}
void host_panorama_set_camera(const float *pose){(void)pose;}
void host_panorama_end(int pass) { (void)pass; }
void host_panorama_viewmodel(int active){(void)active;}
void host_panorama_ui(int active) {
    ui_active = active;
    ui_changes++;
}
void host_panorama_projection(float m11, float m22, int x, int y, int w, int h, uint32_t caller) {
    reported_caller = caller;
    projection_reports++;
    reported_m11 = m11; reported_m22 = m22; reported_w = w; reported_h = h;
    reported_x = x; reported_y = y;
}

static uint32_t rect_address(EngineCPU *cpu) { return cpu->gpr[1] + 0x2Cu; }
static int rect_width(EngineCPU *cpu) { return (int16_t)G16(rect_address(cpu) + 6) - (int16_t)G16(rect_address(cpu) + 2); }
static int rect_height(EngineCPU *cpu) { return (int16_t)G16(rect_address(cpu) + 4) - (int16_t)G16(rect_address(cpu) + 0); }

void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    if (address == 0x00494730u) {
        assert(ui_active);
        dispatch_count++;
        return;
    }
    if (address == 0x0050BDC0u) {
        /* The interface record's two builds (0050BDFC and 0050BE1E) reach
         * the original unhooked: no narrowing, no report. */
        assert(ui_active && cpu->gpr[0] == 0);
        dispatch_count++;
        for (unsigned k = 0; k < 2; k++) {
            EngineCPU build = *cpu; build.gpr[1] = 0x2000; build.gpr[6] = 0x3000; build.gpr[4] = 0x1800;
            S32(0x1800, k ? 0x0050BE23u : 0x0050BE01u);
            uint8_t rect[8]; memcpy(rect, GPTR(0x2000 + 0x2C), 8);
            assert(host_panorama_dispatch(&build, 0x0050CC40u) == 0);
            assert(!memcmp(rect, GPTR(0x2000 + 0x2C), 8));
            interface_builds++;
        }
        ret_to_caller(cpu);
        return;
    }
    assert(address == 0x0050CC40u);
    dispatch_count++;
    if (inspect_dense_rect) {
        /* 640x480 becomes the 32-degree guarded band: 173x480. */
        assert(rect_width(cpu) == 173);
        assert(rect_height(cpu) == 480);
    }
    /* The original writes its projection from ESI+0x144; m[0] and m[5] are
     * the scale terms the recorder consumes. */
    float m11 = 1.6003f, m22 = 0.5774f;
    memcpy(GPTR(cpu->gpr[6] + 0x144), &m11, 4); memcpy(GPTR(cpu->gpr[6] + 0x158), &m22, 4);
    if (recurse_projection)
        assert(host_panorama_dispatch(cpu, address) == 0);
}

static void setup(EngineCPU *cpu, uint32_t frustum, uint32_t caller) {
    memset(cpu, 0, sizeof *cpu);
    cpu->gpr[1] = frustum; /* ECX: input frustum record. */
    cpu->gpr[4] = 0x1000;  /* ESP: return address and stack argument. */
    cpu->gpr[6] = 0x3000;  /* ESI: output record; projection at +0x144. */
    S32(cpu->gpr[4], caller);
    S16(frustum + 0x2C, 0);
    S16(frustum + 0x2E, 0);
    S16(frustum + 0x30, 480);
    S16(frustum + 0x32, 640);
    float fov = 2.0f * (float)M_PI / 3.0f;
    memcpy(GPTR(frustum + 0x28), &fov, sizeof fov);
}

static void assert_rect_bytes(uint32_t frustum, const uint8_t before[8]) {
    assert(memcmp(GPTR(frustum + 0x2C), before, 8) == 0);
}

#define OARG(i) G32(cpu->gpr[4] + 4u + 4u * (uint32_t)(i))
/* Boundary stubs: no prior captured layer and no profiling overhead. */
uint64_t host_yield_spin_ns;
int host_panorama_all_layers_ready(void) { return 0; }
void host_panorama_set_stereo(int on) { (void)on; }
int host_pass_profile_enabled(void) { return 0; }
void host_pass_profile_add(uint64_t ns) { (void)ns; }
/* This boundary fixture exercises the unpaced budget target. */
float host_frame_pacer_budget_target(float fallback) { return fallback; }
#include "../panorama_hooks.inc"

int main(void) {
    /* The HUD entry below is dispatched outside any pass; run overlay entries
     * in every view, the contract this ABI check was written against. */
    setenv("HALO_PANORAMA_OVERLAY_ALL_VIEWS","1",1);
    engine_flat_base = calloc(0x10000, 1);
    assert(engine_flat_base);
    EngineCPU cpu;
    const uint32_t frustum = 0x2000;
    uint8_t before[8];
    setenv("HALO_PANORAMA_DENSE", "1", 1);

    /* Primary 0x50BA80 return sites temporarily narrow only projection input. */
    panorama_nested = 1;
    setup(&cpu, frustum, 0x0050BC8Bu);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    inspect_dense_rect = 1;
    assert(host_panorama_dispatch(&cpu, 0x0050CC40u) == 1);
    assert(dispatch_count == 1);
    assert_rect_bytes(frustum, before);
    /* Recorded after the original ran, with the restored full raster rect. */
    assert(projection_reports == 1 && reported_w == 640 && reported_h == 480);
    assert(fabsf(reported_m11 - 1.6003f) < 1e-6f && fabsf(reported_m22 - 0.5774f) < 1e-6f);
    /* A reduced, off-center viewport keeps its original raster origin. */
    setup(&cpu, frustum, 0x0050BC8Bu);
    S16(frustum+0x2C,60);S16(frustum+0x2E,12);S16(frustum+0x30,420);S16(frustum+0x32,652);
    memcpy(before,GPTR(frustum+0x2C),8);inspect_dense_rect=0;
    assert(host_panorama_dispatch(&cpu,0x0050CC40u)==1);
    assert(reported_x==12&&reported_y==60&&reported_w==640&&reported_h==360);
    assert_rect_bytes(frustum,before);

    /* Guard prevents recursive interception while the original runs. */
    setup(&cpu, frustum, 0x0050BCA3u);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    recurse_projection = 1;
    assert(host_panorama_dispatch(&cpu, 0x0050CC40u) == 1);
    assert(dispatch_count == 3);
    assert_rect_bytes(frustum, before);
    recurse_projection = 0;

    /* Auxiliary culling and arbitrary callers stay completely unchanged. */
    setup(&cpu, frustum, 0x0050C608u);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    assert(host_panorama_dispatch(&cpu, 0x0050CC40u) == 0);
    assert(dispatch_count == 3);
    assert_rect_bytes(frustum, before);
    setup(&cpu, frustum, 0x005545B3u);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    assert(host_panorama_dispatch(&cpu, 0x0050CC40u) == 0);
    assert_rect_bytes(frustum, before);

    /* UI nesting excludes projection interception and HUD interception remains separate. */
    setup(&cpu, frustum, 0x0050BC8Bu);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    panorama_ui_nested = 1;
    assert(host_panorama_dispatch(&cpu, 0x0050CC40u) == 0);
    assert_rect_bytes(frustum, before);
    panorama_ui_nested = 0;
    cpu.gpr[4] = 0x1000;
    assert(host_panorama_dispatch(&cpu, 0x00494730u) == 1);
    assert(ui_changes == 2 && !ui_active);

    /* 0050BDC0's builds are world builds only for a player view with no
     * camera (EAX 1), where they are the pass's only projection. */
    setup(&cpu, frustum, 0x0050BE01u);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    projection_reports = 0;
    assert(host_panorama_dispatch(&cpu, 0x0050CC40u) == 1);
    assert(projection_reports == 1 && reported_caller == 0x0050BE01u && reported_w == 640);
    assert_rect_bytes(frustum, before);
    /* The interface record (EAX 0) runs in the HUD pass inside the overlay
     * scope, so its builds can never narrow a rectangle or replace the
     * world view's report; EAX 1 is not routed at all. */
    setup(&cpu, frustum, 0x0050BF86u);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    projection_reports = 0; unsigned changes = ui_changes, dispatched = dispatch_count;
    panorama_view_pass = HALO_PANORAMA_CENTRE_LEFT; cpu.gpr[0] = 0;
    assert(host_panorama_dispatch(&cpu, 0x0050BDC0u) == 1);
    assert(cpu.pc == 0x0050BF86u && cpu.gpr[4] == 0x1004);
    assert(interface_builds == 2 && projection_reports == 0 && dispatch_count == dispatched + 1);
    assert(ui_changes == changes + 2 && !ui_active && !panorama_ui_nested);
    assert_rect_bytes(frustum, before);
    setup(&cpu, frustum, 0x0050BF86u); cpu.gpr[0] = 1;
    assert(host_panorama_dispatch(&cpu, 0x0050BDC0u) == 0 && ui_changes == changes + 2);
    panorama_view_pass = -1;

    /* Without the density option the original still runs through the hook
     * so the projection is recorded, and the rectangle is untouched. */
    unsetenv("HALO_PANORAMA_DENSE");
    inspect_dense_rect = 0;
    setup(&cpu, frustum, 0x0050BC8Bu);
    memcpy(before, GPTR(frustum + 0x2C), 8);
    projection_reports = 0;
    assert(host_panorama_dispatch(&cpu, 0x0050CC40u) == 1);
    assert(projection_reports == 1 && reported_w == 640 && reported_h == 480);
    assert_rect_bytes(frustum, before);
    free(engine_flat_base);
    puts("PASS: projection hook ABI, builder recording, 32-degree guarded width, restoration, reentry guard, caller filter, UI isolation, and no report from the interface record");
    return 0;
}
