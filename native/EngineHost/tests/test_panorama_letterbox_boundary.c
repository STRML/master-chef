/* Focused ABI test for the exact cinematic letterbox dispatch filter. */
#include "../host.h"
#include "../halo_settings.h"
#include <assert.h>

uint8_t *engine_flat_base;
static unsigned dispatched, ui_changes;
static int ui_active;
void host_log(const char *format, ...) { (void)format; }
void host_panorama_reset(void) {}
void host_panorama_invalidate(void) {}
void host_panorama_abort(void) {}
void host_panorama_begin(int pass) { (void)pass; }
int host_panorama_camera_moving(void){return 0;}
void host_panorama_set_camera(const float *pose){(void)pose;}
void host_panorama_end(int pass) { (void)pass; }
void host_panorama_projection(float x,float y,int vx,int vy,int w,int h,uint32_t caller) {
    (void)x;(void)y;(void)vx;(void)vy;(void)w;(void)h;(void)caller;
}
void host_panorama_viewmodel(int active){(void)active;}
void host_panorama_ui(int active) {
    if (active) { assert(!ui_active); ui_active = 1; ui_changes++; }
    else { assert(ui_active); ui_active = 0; }
}
void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    (void)address; dispatched++;
    cpu->pc = engine_pop(cpu, 4);   /* the original returns */
}
static void ret_to_caller(EngineCPU *cpu) { cpu->pc = engine_pop(cpu, 4); }

#define OARG(i) G32(cpu->gpr[4] + 4u + 4u * (uint32_t)(i))
/* This boundary fixture exercises the unpaced budget target. */
float host_frame_pacer_budget_target(float fallback) { return fallback; }
#include "../panorama_hooks.inc"

static void set_return(EngineCPU *cpu, uint32_t returnAddress) {
    S32(cpu->gpr[4], returnAddress);
    cpu->pc = 0xDEADC0DEu;
}

int main(void) {
    engine_flat_base = calloc(0x10000, 1);
    assert(engine_flat_base);
    EngineCPU cpu = {0};
    cpu.gpr[4] = 0x1000;

    /* These are the two call instructions in sub_004499C0: call 449780
     * pushes 449B2A and 449B9E respectively. The shim is a plain ret, so the
     * return address is popped and ESP advances by exactly four bytes. */
    panorama_nested = 1;
    set_return(&cpu, 0x00449B2Au);
    assert(host_panorama_dispatch(&cpu, 0x00449780u) == 1);
    assert(cpu.pc == 0x00449B2Au && cpu.gpr[4] == 0x1004);
    cpu.gpr[4] = 0x1000;
    set_return(&cpu, 0x00449B9Eu);
    assert(host_panorama_dispatch(&cpu, 0x00449780u) == 1);
    assert(cpu.pc == 0x00449B9Eu && cpu.gpr[4] == 0x1004);

    /* Shared rectangle renderer at another return site must run unchanged. */
    cpu.gpr[4] = 0x1000;
    set_return(&cpu, 0x00449B50u);
    assert(host_panorama_dispatch(&cpu, 0x00449780u) == 0);
    assert(cpu.pc == 0xDEADC0DEu && cpu.gpr[4] == 0x1000);

    /* The exact callsites are inert outside panorama rendering. */
    panorama_nested = 0;
    cpu.gpr[4] = 0x1000;
    set_return(&cpu, 0x00449B2Au);
    assert(host_panorama_dispatch(&cpu, 0x00449780u) == 0);
    assert(cpu.pc == 0xDEADC0DEu && cpu.gpr[4] == 0x1000);

    /* Existing HUD interception remains separate and does not broaden the
     * letterbox filter to fades or other screen-space draw paths. */
    panorama_nested = 1; panorama_view_pass = HALO_PANORAMA_CENTRE_LEFT;   /* the HUD pass */
    cpu.gpr[4] = 0x1000;
    set_return(&cpu, 0x0049AAAAu);
    assert(host_panorama_dispatch(&cpu, 0x00494730u) == 1);
    assert(dispatched == 1 && ui_changes == 1 && !ui_active);

    /* The game's interface record (0050BDC0, EAX 0) is where 004499C0 runs:
     * it is routed in the HUD pass and skipped as a plain return elsewhere,
     * and its bars stay omitted inside that scope. EAX 1, a player view with
     * no camera, is left to the original. */
    cpu.gpr[4] = 0x1000; cpu.gpr[0] = 0;
    set_return(&cpu, 0x0050BF86u);
    assert(host_panorama_dispatch(&cpu, 0x0050BDC0u) == 1);
    assert(dispatched == 2 && ui_changes == 2 && !ui_active && cpu.pc == 0x0050BF86u && cpu.gpr[4] == 0x1004);
    cpu.gpr[4] = 0x1000;
    panorama_view_pass = 0;
    set_return(&cpu, 0x0050BF86u);
    assert(host_panorama_dispatch(&cpu, 0x0050BDC0u) == 1);
    assert(dispatched == 2 && ui_changes == 2 && cpu.pc == 0x0050BF86u && cpu.gpr[4] == 0x1004);
    panorama_view_pass = HALO_PANORAMA_CENTRE_LEFT;
    cpu.gpr[4] = 0x1000; cpu.gpr[0] = 1;
    set_return(&cpu, 0x0050BF86u);
    assert(host_panorama_dispatch(&cpu, 0x0050BDC0u) == 0 && ui_changes == 2);
    panorama_ui_nested = 1;
    cpu.gpr[4] = 0x1000;
    set_return(&cpu, 0x00449B2Au);
    assert(host_panorama_dispatch(&cpu, 0x00449780u) == 1);
    assert(cpu.pc == 0x00449B2Au && cpu.gpr[4] == 0x1004);
    panorama_ui_nested = 0;
    panorama_view_pass = -1;
    free(engine_flat_base);
    puts("PASS: cinematic letterbox ABI filter omits only 449B2A/449B9E, preserves stack return, leaves other callers/outside-panorama paths untouched, and still applies inside the interface record, which is routed only in the HUD pass.");
    return 0;
}
