/* Eight-bearing mono sphere host ABI/state regression, with an injected original-render escape.
 * This is a stub boundary test, not original-game or GPU visibility validation. */
#include "../host.h"
#include "../halo_settings.h"
#include "../panorama.h"
/* Exercise option combinations in one fixture; production caches options for
 * process lifetime. This test intentionally does not test that cache. */
#undef HOST_ENV
#define HOST_ENV(name) getenv(name)
#include <assert.h>
#include <stdarg.h>

uint8_t *engine_flat_base;
static int host_panorama_dispatch(EngineCPU *, uint32_t);
static unsigned calls, begins, ends, resets, aborts, mask;
static unsigned ui_on, ui_off, viewmodel_draws[10], hud_draws[10];
static int ui_active, drawing_hud, boundary_escape;
static int experimental,cache_escape;
static unsigned cache_builds[4];
static float cache_yaw[4],cache_previous_yaw[4];
static int valid, active_pass = -1, escape_after = -1;
static EngineCPU incoming;
static unsigned char original_records[4 * 0xAC], original_stack[24];
static const uint32_t stack_address = 0x10000, renderer_address = 0x20000;
static const uint32_t caller_address = 0x00401234, escape_address = 0x00405678;
static unsigned record_count;
/* The game's record list ends with its interface record (word -1, byte +2
 * set), which 0050BEA0 sends to 0050BDC0 with EAX 0; world views are the
 * records before it. */
static int interface_last;
static unsigned interface_calls[10], interface_draws[10];
static unsigned world_count(void) { return record_count - (unsigned)interface_last; }
static unsigned natural_builds, world_builds, projection_reports, restore_uploads;
static int projection_escape, upload_escape;
static uint64_t helper_instructions;
static float expected_world_xy[2];
static unsigned fp_trace_reports;

void host_log(const char *format, ...) {
    if(!strncmp(format,"[panorama-fp-projection]",24))fp_trace_reports++;
}
void host_panorama_reset(void) { mask = 0; valid = 0; resets++; }
void host_panorama_invalidate(void) { valid = 0; }
void host_panorama_abort(void) { mask = 0; valid = 0; aborts++; }
void host_panorama_begin(int pass) { assert(active_pass == -1); active_pass = pass; begins++; }
/* The frame's camera is recorded once, before the first pass, exactly as the
 * game left it in view 0's first frustum (position, forward, up). */
static unsigned camera_calls; static float camera_pose[HALO_PANORAMA_POSE_FLOATS];
int host_panorama_camera_moving(void){return 0;}
void host_panorama_set_camera(const float pose[HALO_PANORAMA_POSE_FLOATS]) {
    assert(active_pass == -1 && begins == 0);
    memcpy(camera_pose, pose, sizeof camera_pose); camera_calls++;
}
void host_panorama_end(int pass) {
    assert(active_pass == pass); active_pass = -1; ends++;
    mask |= 1u << pass; valid = mask == HALO_PANORAMA_MONO_MASK;
}
static int diagnostic_viewmodel;
void host_panorama_viewmodel(int active){assert(active!=diagnostic_viewmodel);diagnostic_viewmodel=active;}
void host_panorama_ui(int active) {
    assert(active != ui_active);
    ui_active = active;
    if (active) ui_on++; else ui_off++;
}
void host_panorama_projection(float x, float y, int vx, int vy, int w, int h, uint32_t caller) {
    assert(fabsf(x-expected_world_xy[0])<1e-6f && fabsf(y-expected_world_xy[1])<1e-6f);
    assert(vx==0 && vy==0 && w==1280 && h==960 && caller==0x0050BC8Bu);
    projection_reports++;
}
static void ret_to_caller(EngineCPU *cpu) { cpu->pc = engine_pop(cpu, 4); }
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

static void record_overlay_draw(void) {
    assert(active_pass >= 0 && active_pass < HALO_PANORAMA_LAYERS);
    assert(ui_active || (experimental && !drawing_hud));
    /* Mirrors gpu_draw's side-pass rejection while panorama_ui_active. */
    if (active_pass == 1 || (experimental && !drawing_hud)) {
        if (drawing_hud) hud_draws[active_pass]++;
        else viewmodel_draws[active_pass]++;
    }
}

void engine_dispatch(EngineCPU *cpu, uint32_t address) {
    if (address == 0x0050BDC0u) {
        /* Only the interface record gets here: routed, and its draws land
         * only in the HUD pass, as gpu_draw drops them elsewhere. */
        assert(host_panorama_dispatch(cpu, address) == 0);
        assert(cpu->gpr[0] == 0 && ui_active && panorama_ui_nested && !panorama_viewmodel_scope);
        assert(G32(cpu->gpr[4] + 4) == renderer_address + (record_count - 1) * 0xAC);
        interface_calls[active_pass]++;
        if (active_pass == 1) interface_draws[active_pass]++;
        ret_to_caller(cpu);
        return;
    }
    if(address==0x00492430u){
        assert(host_panorama_dispatch(cpu,address)==0);
        unsigned view=G16(0x007C3108u);float f[3];memcpy(f,GPTR(0x007C3120u),sizeof f);
        cache_builds[view]++;cache_previous_yaw[view]=cache_yaw[view];cache_yaw[view]=atan2f(f[1],f[0]);
        if(experimental)assert(f[0]==1 && f[1]==0 && f[2]==0);
        if(cache_escape){cpu->pc=escape_address;return;}
        ret_to_caller(cpu);return;
    }
    if(address==0x0050CC40u){
        assert(host_panorama_dispatch(cpu,address)==0);
        uint32_t frustum=cpu->gpr[1],output=cpu->gpr[6];float fov;
        memcpy(&fov,GPTR(frustum+0x28),4);
        int height=(int16_t)G16(frustum+0x30)-(int16_t)G16(frustum+0x2C);
        int width=(int16_t)G16(frustum+0x32)-(int16_t)G16(frustum+0x2E);
        float y=1/tanf(fov/2),x=y*height/width;
        if(fabsf(fov-2.0943951024f)>1e-6f){
            natural_builds++;assert(width==1280 && height==960);
            if(projection_escape){cpu->pc=escape_address;return;}
        }else{world_builds++;expected_world_xy[0]=x;expected_world_xy[1]=y;}
        memset(GPTR(output),0xB7,0x18C);
        memcpy(GPTR(output+0x144),&x,4);memcpy(GPTR(output+0x158),&y,4);
        cpu->instruction_count+=7;helper_instructions+=7;
        cpu->gpr[0]=0xFEED1234;cpu->flags^=1;cpu->fp_reg[engine_fp_physical(cpu,0)]=123;
        ret_to_caller(cpu);return;
    }
    if(address==0x00518F40u){
        assert(host_panorama_dispatch(cpu,address)==0);
        if(!ui_active && !experimental){restore_uploads++;assert(G32(cpu->gpr[4]+4)==0 && G32(cpu->gpr[4]+8)==0);
            float x,y;memcpy(&x,GPTR(PANORAMA_FP_M11),4);memcpy(&y,GPTR(PANORAMA_FP_M22),4);
            assert(x==expected_world_xy[0] && y==expected_world_xy[1]);
            if(upload_escape){cpu->pc=escape_address;return;}
        }
        cpu->instruction_count+=5;helper_instructions+=5;
        cpu->gpr[0]=0xBAD1234;cpu->flags^=4;cpu->fp_reg[engine_fp_physical(cpu,0)]=456;
        ret_to_caller(cpu);return;
    }
    if (address == 0x005154A0u) {
        assert(host_panorama_dispatch(cpu, address) == 0);
        assert(!ui_active && !panorama_viewmodel_scope && !diagnostic_viewmodel);
        ret_to_caller(cpu);
        return;
    }
    if (address == 0x004924B0u) {
        assert(host_panorama_dispatch(cpu, address) == 0); /* original reentry */
        assert(ui_active==!experimental && panorama_viewmodel_scope && diagnostic_viewmodel);
        if(panorama_fp_xy_valid && !experimental){
            float x,y;memcpy(&x,GPTR(PANORAMA_FP_M11),4);memcpy(&y,GPTR(PANORAMA_FP_M22),4);
            assert(fabsf(x/y-.75f)<1e-6f);
            assert(fabsf(y-1/tanf(panorama_original_fov[G16(0x007C3108u)][0]/2))<1e-6f);
        }
        if (boundary_escape) { cpu->pc = escape_address; return; }
        /* Real FP materials call the original consumer uploader while the
         * production scope is active; the trace hook must not double-route. */
        EngineCPU before_upload=*cpu;unsigned char frame[12];
        uint32_t at=cpu->gpr[4]-12;memcpy(frame,GPTR(at),sizeof frame);
        engine_push(cpu,0x3F800000,4);engine_push(cpu,0x3DCCCCCD,4);engine_push(cpu,0x00526F00u,4);
        assert(host_panorama_dispatch(cpu,0x00518F40u)==1);
        assert(cpu->pc==0x00526F00u);
        uint64_t instructions=cpu->instruction_count;*cpu=before_upload;cpu->instruction_count=instructions;
        memcpy(GPTR(at),frame,sizeof frame);
        record_overlay_draw();
        ret_to_caller(cpu);
        return;
    }
    if (address == 0x00494730u) {
        assert(host_panorama_dispatch(cpu, address) == 0);
        assert(ui_active);
        drawing_hud = 1;
        record_overlay_draw();
        drawing_hud = 0;
        ret_to_caller(cpu);
        return;
    }
    assert(address == 0x0050BEA0u);
    assert(host_panorama_dispatch(cpu, address) == 0); /* original reentry */
    static const int order[] = {0, 2, 5, 6, 7, 8, 9, 1};
    assert(calls < 8 && active_pass == order[calls]);
    assert(cpu->instruction_count == incoming.instruction_count + 100 * calls + helper_instructions);
    EngineCPU comparison = *cpu;
    comparison.instruction_count = incoming.instruction_count;
    assert(!memcmp(&comparison, &incoming, sizeof incoming));
    /* Only the left-eye centre pass (layer 1) carries the render time step;
     * every bearing sees the frame's render-frame counter, and flares are off. */
    unsigned char expected_stack[sizeof original_stack]; memcpy(expected_stack, original_stack, sizeof expected_stack);
    if (active_pass != 1) memset(expected_stack + 20, 0, 4);
    assert(!memcmp(GPTR(stack_address), expected_stack, sizeof expected_stack));
    assert(G32(0x007C3100u) == 41u && G8(0x006893FFu) == 0 && G32(0x0071D134u) == 0);
    for (unsigned view = 0; view < record_count; view++) {
        unsigned char expected[0xAC]; memcpy(expected, original_records + view * 0xAC, sizeof expected);
        /* The interface record keeps the game's camera in every bearing. */
        if (view >= world_count()) { assert(!memcmp(GPTR(renderer_address + view * 0xAC), expected, sizeof expected)); continue; }
        for (unsigned frustum = 0; frustum < 2; frustum++) {
            unsigned at = frustum ? 0x58 : 4;
            const float yaw[10]={-1.0471975512f,0,1.0471975512f,0,0,0,0,2.0943951024f,3.1415926536f,-2.0943951024f};
            float angle=yaw[active_pass];
            float pitch=active_pass==5?1.5707963268f:active_pass==6?-1.5707963268f:0;
            float forward[3] = {cosf(angle)*cosf(pitch), -sinf(angle)*cosf(pitch), sinf(pitch)};
            float up[3]={-cosf(angle)*sinf(pitch),sinf(angle)*sinf(pitch),cosf(pitch)};
            float actual_up[3];memcpy(actual_up,GPTR(renderer_address+view*0xAC+at+0x18),sizeof actual_up);
            for(unsigned k=0;k<3;k++)assert(fabsf(actual_up[k]-up[k])<1e-6f);
            memcpy(expected+at+0x18,actual_up,sizeof actual_up);
            float actual[3]; memcpy(actual, GPTR(renderer_address + view * 0xAC + at + 0x0C), sizeof actual);
            for (unsigned k = 0; k < 3; k++) assert(fabsf(actual[k] - forward[k]) < 1e-6f);
            float fov = 2.0943951024f;
            memcpy(expected + at + 0x0C, actual, sizeof actual);
            memcpy(expected + at + 0x28, &fov, sizeof fov);
        }
        assert(!memcmp(GPTR(renderer_address + view * 0xAC), expected, sizeof expected));
    }
    /* Exercise production builder hook before FP; natural scales must never
     * reach panorama metadata. Distinct view FOVs catch stale/view0 reuse. */
    for(unsigned view=0;view<world_count();view++){
        EngineCPU call=*cpu;call.gpr[4]=stack_address+0x100;
        call.gpr[1]=renderer_address+view*0xAC+4;call.gpr[6]=0x30000+view*0x200;
        call.pc=0x0050CC40u;S32(call.gpr[4],0x0050BC8Bu);
        assert(host_panorama_dispatch(&call,0x0050CC40u)==1);
        assert(call.pc==0x0050BC8Bu);
        cpu->instruction_count=call.instruction_count;
    }
    /* Source cache builder consumes primary pose while the secondary view
     * retains yaw. Verify the production hook's once-per-view central input. */
    for(unsigned view=0;view<world_count();view++){
        memcpy(GPTR(0x007C3114u),GPTR(renderer_address+view*0xACu+4),36);
        unsigned char yawed[36];memcpy(yawed,GPTR(0x007C3114u),36);
        S16(0x007C3108u,(uint16_t)view);EngineCPU cache=*cpu;cache.gpr[4]=stack_address+0x100;S32(cache.gpr[4],0x0050C09Cu);
        if(!host_panorama_dispatch(&cache,0x00492430u))engine_dispatch(&cache,0x00492430u);
        assert(!memcmp(yawed,GPTR(0x007C3114u),36));
        if(cache_escape){assert(cache.pc==escape_address);*cpu=cache;calls++;return;}
        assert(cache.pc==0x0050C09Cu);
    }
    float world_x=expected_world_xy[0],world_y=expected_world_xy[1];
    memcpy(GPTR(PANORAMA_FP_M11),&world_x,4);memcpy(GPTR(PANORAMA_FP_M22),&world_y,4);
    S16(0x007C3108u,(uint16_t)(world_count()-1));
    /* Exercise the actual FP entry; the generic material queue remains
     * unscoped at both original return sites. Source order is checked against
     * compiled original instructions separately; this fixture tests routing. */
    cpu->gpr[4] = stack_address; S32(stack_address, 0x0050E98Du);
    assert(host_panorama_dispatch(cpu, 0x004924B0u) == 1);
    assert(!ui_active && !panorama_viewmodel_scope && !diagnostic_viewmodel);
    assert(!memcmp(GPTR(PANORAMA_FP_M11),&world_x,4) && !memcmp(GPTR(PANORAMA_FP_M22),&world_y,4));
    for (unsigned k = 0; k < 2; ++k) {
        cpu->gpr[4] = stack_address;
        S32(stack_address, k ? 0x0050C4F9u : 0x0050C44Cu);
        assert(host_panorama_dispatch(cpu, 0x005154A0u) == 0);
        engine_dispatch(cpu, 0x005154A0u);
        assert(!ui_active && !panorama_viewmodel_scope && !diagnostic_viewmodel);
    }
    cpu->gpr[4] = stack_address; S32(stack_address, 0x00601234u);
    assert(host_panorama_dispatch(cpu, 0x00494730u) == 1);
    assert(!ui_active && !panorama_ui_nested);
    /* 0050BEA0's record loop: the interface record last, with EAX 0; a
     * player view (EAX 1 for one with no camera) is never routed. */
    if (interface_last && (G32(stack_address + 8) & 0xFFFFu) == record_count) {
        EngineCPU ui = *cpu; ui.gpr[0] = 0; ui.gpr[4] = stack_address + 0x200;
        S32(ui.gpr[4], 0x0050BF86u); S32(ui.gpr[4] + 4, renderer_address + (record_count - 1) * 0xAC);
        assert(host_panorama_dispatch(&ui, 0x0050BDC0u) == 1);
        assert(ui.pc == 0x0050BF86u && ui.gpr[4] == stack_address + 0x204);
        assert(!ui_active && !panorama_ui_nested);
        cpu->instruction_count = ui.instruction_count;
    }
    { EngineCPU camera_less = *cpu; camera_less.gpr[0] = 1; camera_less.gpr[4] = stack_address + 0x200;
      S32(camera_less.gpr[4], 0x0050BF86u);
      assert(host_panorama_dispatch(&camera_less, 0x0050BDC0u) == 0 && !ui_active); }
    cpu->gpr[4] = stack_address; S32(stack_address, caller_address);
    /* Emulate legal caller-volatile CPU state and renderer-local mutations.
     * Neither must contaminate the next view's input. */
    memset(GPTR(renderer_address), 0xCD, record_count * 0xAC);
    S32(stack_address + 8, 0xDEADC0DE);
    cpu->gpr[0] = 0x12340000 + calls;
    cpu->gpr[1] = 0xABC00000 + calls;
    cpu->flags ^= 0x41;
    cpu->fp_reg[engine_fp_physical(cpu, 0)] = 123.0 + calls;
    cpu->instruction_count += 100;
    cpu->gpr[4] = stack_address + 4;
    cpu->pc = (int)calls == escape_after ? escape_address : caller_address;
    calls++;
}

static void setup_records(unsigned views, int escape, int interface) {
    record_count = views; escape_after = escape; interface_last = interface && views > 1;
    memset(interface_calls, 0, sizeof interface_calls); memset(interface_draws, 0, sizeof interface_draws);
    calls = begins = ends = resets = aborts = mask = camera_calls = 0;
    natural_builds=world_builds=projection_reports=restore_uploads=0;helper_instructions=0;
    projection_escape=upload_escape=cache_escape=0;
    memset(cache_builds,0,sizeof cache_builds);memset(cache_yaw,0,sizeof cache_yaw);memset(cache_previous_yaw,0,sizeof cache_previous_yaw);
    ui_on = ui_off = 0; memset(viewmodel_draws, 0, sizeof viewmodel_draws);
    memset(hud_draws, 0, sizeof hud_draws);
    valid = 0; active_pass = -1; ui_active = drawing_hud = boundary_escape = 0;
    memset(&incoming, 0, sizeof incoming);
    incoming.pc = 0x0050BEA0u; incoming.gpr[4] = stack_address;
    incoming.gpr[3] = 0x12345678; incoming.flags = 0x202;
    incoming.fp_top = 5; incoming.fp_reg[5] = 7; incoming.fp_valid = 1; incoming.fp_control = 0x037F; /* ST(0) = 7 */
    incoming.instruction_count = 10;
    S32(stack_address, caller_address); S32(stack_address + 4, renderer_address);
    S32(stack_address + 8, views); S32(stack_address + 12, 0x123450);
    S32(stack_address + 16, 0x3F800000); S32(stack_address + 20, 0x40000000);
    memcpy(original_stack, GPTR(stack_address), sizeof original_stack);
    S32(0x007C3100u, 41u); S8(0x006893FFu, 1); S32(0x0071D134u, 3u);
    memset(original_records, 0xA5, sizeof original_records);
    for (unsigned view = 0; view < views; view++) for (unsigned frustum = 0; frustum < 2; frustum++) {
        unsigned at = view * 0xAC + (frustum ? 0x58 : 4);
        const float forward[3] = {1, 0, 0}, up[3] = {0, 0, 1};
        memcpy(original_records + at + 0x0C, forward, sizeof forward);
        memcpy(original_records + at + 0x18, up, sizeof up);
        float fov=1.1f+view*.1f;memcpy(original_records+at+0x28,&fov,4);
        int16_t rect[4]={0,0,960,1280};memcpy(original_records+at+0x2C,rect,sizeof rect);
    }
    for (unsigned view = 0; view < views; view++) {
        int interface_record = interface_last && view == views - 1;
        uint16_t word = interface_record ? 0xFFFFu : (uint16_t)view;
        memcpy(original_records + view * 0xAC, &word, 2);
        original_records[view * 0xAC + 2] = (unsigned char)interface_record;
    }
    memcpy(GPTR(renderer_address), original_records, sizeof original_records);
}
static void setup(unsigned views, int escape) { setup_records(views, escape, 0); }

int main(void) {
    /* The routing checks below run every overlay entry in every view, the
     * contract before overlay entries were limited to the views their draws
     * reach; that limit is checked on its own further down. */
    setenv("HALO_PANORAMA_OVERLAY_ALL_VIEWS","1",1);
    engine_flat_base = calloc(0x900000, 1); assert(engine_flat_base);
    unsetenv("HALO_PANORAMA_WORLD_FP");
    setenv("HALO_PANORAMA", "1", 1);
    setenv("HALO_STEREO","0",1);
    setenv("HALO_PANORAMA_VIEWS","all",1);
    setenv("HALO_PANORAMA_VFOV","120",1);
    setenv("HALO_PANORAMA_DENSE", "1", 1);
    /* An escape from the actual FP renderer must close its target before
     * the outer per-view escape cleanup runs. */
    setup(1, -1); panorama_nested = 1; panorama_view_pass = active_pass = 0;
    boundary_escape = 1; EngineCPU escaped = incoming;
    S32(stack_address, 0x0050E98Du);
    assert(host_panorama_dispatch(&escaped, 0x004924B0u) == 1);
    assert(escaped.pc == escape_address && !ui_active);
    assert(!panorama_ui_nested && !panorama_viewmodel_scope);
    panorama_nested = 0; panorama_view_pass = active_pass = -1;
    /* Exact borrowed-frame and CPU restoration, plus consumer escape cleanup. */
    setup(1,-1);expected_world_xy[0]=1.5f;expected_world_xy[1]=.5f;
    memcpy(GPTR(PANORAMA_FP_M11),&expected_world_xy[0],4);memcpy(GPTR(PANORAMA_FP_M22),&expected_world_xy[1],4);
    EngineCPU upload=incoming;upload.pc=caller_address;
    unsigned char borrowed[12];memset(GPTR(stack_address-12),0x73,12);memcpy(borrowed,GPTR(stack_address-12),12);
    assert(panorama_fp_restore_upload(&upload));
    assert(!memcmp(GPTR(stack_address-12),borrowed,12));
    EngineCPU exact=upload;exact.instruction_count=incoming.instruction_count;exact.pc=incoming.pc;
    assert(!memcmp(&exact,&incoming,sizeof exact));
    upload_escape=1;assert(!panorama_fp_restore_upload(&upload));
    assert(upload.pc==escape_address && !panorama_fp_upload_nested);
    assert(!memcmp(GPTR(stack_address-12),borrowed,12));
    /* Escaped FP restores XY but must never re-enter the world uploader. */
    setup(1,-1);panorama_nested=1;panorama_view_pass=active_pass=0;
    panorama_original_fov[0][0]=1.1f;panorama_fp_xy[0][1]=1/tanf(.55f);
    panorama_fp_xy[0][0]=panorama_fp_xy[0][1]*.75f;panorama_fp_xy_valid=1;
    S16(0x007C3108u,0);expected_world_xy[0]=1.5f;expected_world_xy[1]=.5f;
    memcpy(GPTR(PANORAMA_FP_M11),&expected_world_xy[0],4);memcpy(GPTR(PANORAMA_FP_M22),&expected_world_xy[1],4);
    boundary_escape=1;escaped=incoming;S32(stack_address,0x0050E98Du);
    assert(host_panorama_dispatch(&escaped,0x004924B0u)==1);
    assert(escaped.pc==escape_address && !ui_active && !panorama_viewmodel_scope && !panorama_ui_nested);
    assert(restore_uploads==0);
    assert(!memcmp(GPTR(PANORAMA_FP_M11),&expected_world_xy[0],4) && !memcmp(GPTR(PANORAMA_FP_M22),&expected_world_xy[1],4));
    panorama_nested=0;panorama_view_pass=active_pass=-1;panorama_fp_xy_valid=0;
    /* Natural-builder escape restores source-owned output/FOV/caller frame
     * and cannot publish natural scales as world metadata. */
    setup(1,-1);panorama_nested=1;panorama_renderer=renderer_address;panorama_renderer_count=1;
    panorama_original_fov[0][0]=1.1f;panorama_fp_xy_valid=0;projection_escape=1;
    EngineCPU builder=incoming;builder.gpr[1]=renderer_address+4;builder.gpr[6]=0x30000;
    float wide=2.0943951024f;memcpy(GPTR(renderer_address+4+0x28),&wide,4);
    memset(GPTR(0x30000),0x91,0x18C);unsigned char output_before[0x18C];memcpy(output_before,GPTR(0x30000),sizeof output_before);
    S32(stack_address,0x0050BC8Bu);
    assert(host_panorama_dispatch(&builder,0x0050CC40u)==1);
    assert(builder.pc==escape_address && !panorama_projection_nested && !panorama_fp_xy_valid);
    assert(!memcmp(GPTR(0x30000),output_before,sizeof output_before));
    assert(!memcmp(GPTR(renderer_address+4+0x28),&wide,4));
    assert(G32(stack_address)==0x0050BC8Bu && projection_reports==0 && world_builds==0);
    panorama_nested=0;panorama_renderer_count=0;
    /* A cache-builder escape must propagate through outer cleanup without
     * drawing/publishing or leaking the temporary central input/scope/mask. */
    experimental=1;setenv("HALO_PANORAMA_WORLD_FP","1",1);
    for(unsigned views=1;views<=4;views++){
        setup(views,-1);cache_escape=1;EngineCPU cache=incoming;
        assert(host_panorama_dispatch(&cache,0x0050BEA0u)==1);
        assert(cache.pc==escape_address && calls==1 && begins==1 && ends==1);
        assert(aborts==1 && !valid && !mask && !ui_on && !ui_off);
        assert(!memcmp(GPTR(renderer_address),original_records,sizeof original_records));
        assert(!memcmp(GPTR(stack_address),original_stack,sizeof original_stack));
        assert(!panorama_nested && !panorama_world_fp && !panorama_fp_cache_nested && !panorama_fp_cache_built);
        assert(!panorama_ui_nested && !panorama_viewmodel_scope && !diagnostic_viewmodel && active_pass==-1);
    }
    for(experimental=0;experimental<2;experimental++)
    for (unsigned views = 1; views <= 4; views++) for (int interface = 0; interface < 2; interface++)
    for (int escape = -1; escape < 8; escape++) {
        if (interface && views < 2) continue;
        if(experimental)setenv("HALO_PANORAMA_WORLD_FP","1",1);else unsetenv("HALO_PANORAMA_WORLD_FP");
        setup_records(views, escape, interface);
        EngineCPU cpu = incoming;
        assert(host_panorama_dispatch(&cpu, 0x0050BEA0u) == 1);
        unsigned expected_calls = escape < 0 ? 8 : (unsigned)escape + 1;
        assert(calls == expected_calls && begins == calls && ends == calls);
        assert(cpu.instruction_count == incoming.instruction_count + 100 * calls + helper_instructions);
        assert(cpu.gpr[4] == stack_address + 4);
        assert(cpu.gpr[0] == 0x12340000 + calls - 1);
        assert(cpu.pc == (escape < 0 ? caller_address : escape_address));
        assert(valid == (escape < 0));
        assert(resets == 1 && aborts == (escape < 0 ? 0 : 1));
        assert(camera_calls == 1 && !memcmp(camera_pose, original_records + 4, sizeof camera_pose));
        assert(active_pass == -1 && panorama_view_pass == -1 && !panorama_nested);
        assert(!ui_active && !panorama_ui_nested && !panorama_viewmodel_scope);
        assert(ui_on == ((experimental?1:2) + (unsigned)interface) * calls && ui_off == ui_on);
        assert(viewmodel_draws[0] == (unsigned)experimental && viewmodel_draws[2] == (unsigned)(experimental&&calls>1));
        assert(hud_draws[0] == 0 && hud_draws[2] == 0);
        assert(viewmodel_draws[1] == (escape < 0 || escape >= 7));
        assert(hud_draws[1] == (escape < 0 || escape >= 7));
        /* Every rendered cap/rear bearing gets the same routing contract;
         * no draw can leak into an unvisited or inactive layer. */
        const int sequence[]={0,2,5,6,7,8,9,1};
        unsigned visited=0;
        for(unsigned n=0;n<expected_calls;n++)visited|=1u<<sequence[n];
        for(unsigned layer=0;layer<HALO_PANORAMA_LAYERS;layer++){
            unsigned present=(visited>>layer)&1u;
            assert(viewmodel_draws[layer]==(present&&(experimental||layer==1)));
            assert(hud_draws[layer]==(present&&layer==1));
            /* HALO_PANORAMA_VIEWS=all keeps the game's count, so the
             * interface record runs in every pass, its draws in one. */
            assert(interface_calls[layer]==(present&&interface));
            assert(interface_draws[layer]==(present&&interface&&layer==1));
        }
        assert(!memcmp(GPTR(renderer_address), original_records, sizeof original_records));
        assert(!memcmp(GPTR(stack_address), original_stack, sizeof original_stack));
        assert(natural_builds==world_count()*calls && world_builds==natural_builds && projection_reports==world_builds);
        assert(restore_uploads==(experimental?0:calls) && !panorama_fp_xy_valid);
        for(unsigned view=0;view<world_count();view++){
            assert(cache_builds[view]==(experimental?1:calls));
            if(experimental)assert(cache_yaw[view]==0 && cache_previous_yaw[view]==0);
            else if(calls==8)assert(fabsf(cache_previous_yaw[view]-2.0943951024f)<1e-6f);
        }
        assert(!panorama_world_fp && !panorama_fp_cache_nested && !panorama_fp_cache_built);
    }
    /* By default an overlay entry runs only where its draws can land: the
     * weapon in the forward sector and the two side bearings, the HUD in the
     * HUD pass. Elsewhere it returns at once, as a skipped call. */
    unsetenv("HALO_PANORAMA_OVERLAY_ALL_VIEWS");
    for(experimental=0;experimental<2;experimental++){
        if(experimental)setenv("HALO_PANORAMA_WORLD_FP","1",1);else unsetenv("HALO_PANORAMA_WORLD_FP");
        setup(1,-1); EngineCPU cpu=incoming;
        assert(host_panorama_dispatch(&cpu,0x0050BEA0u)==1);
        assert(calls==8 && valid && cpu.pc==caller_address && !panorama_nested);
        assert(ui_on==(experimental?1u:4u) && ui_off==ui_on);
        assert(restore_uploads==(experimental?0u:3u));
        for(unsigned layer=0;layer<HALO_PANORAMA_LAYERS;layer++){
            assert(viewmodel_draws[layer]==(experimental?(layer==0||layer==1||layer==2):layer==1));
            assert(hud_draws[layer]==(layer==1));
        }
        assert(!memcmp(GPTR(renderer_address),original_records,sizeof original_records));
        assert(!memcmp(GPTR(stack_address),original_stack,sizeof original_stack));
        /* With the game's interface record it runs in the HUD pass alone. */
        setup_records(2,-1,1); cpu=incoming;
        assert(host_panorama_dispatch(&cpu,0x0050BEA0u)==1);
        assert(calls==8 && valid && cpu.pc==caller_address && !panorama_nested);
        assert(ui_on==(experimental?2u:5u) && ui_off==ui_on);
        for(unsigned layer=0;layer<HALO_PANORAMA_LAYERS;layer++)
            assert(interface_calls[layer]==(layer==1) && interface_draws[layer]==(layer==1));
        assert(!memcmp(GPTR(renderer_address),original_records,sizeof original_records));
        assert(!memcmp(GPTR(stack_address),original_stack,sizeof original_stack));
    }
    setenv("HALO_PANORAMA_OVERLAY_ALL_VIEWS","1",1);
    experimental=0;unsetenv("HALO_PANORAMA_WORLD_FP");
    setenv("HALO_PANORAMA_FP_PROJECTION_TRACE","1",1);
    panorama_nested=panorama_viewmodel_scope=1;panorama_render_sequence=5;
    EngineCPU trace=incoming;
    assert(host_panorama_dispatch(&trace,0x0052B050u)==0 && fp_trace_reports==0);
    panorama_render_sequence=1;
    for(unsigned k=0;k<200;k++)assert(host_panorama_dispatch(&trace,0x0052B050u)==0);
    assert(fp_trace_reports==96);
    panorama_nested=panorama_viewmodel_scope=0;
    free(engine_flat_base);
    puts("PASS: default routing plus opt-in central FP cache once per view, yawed sectors, central HUD and interface record, the interface record's own camera, restored pose/projection, and escape invalidation");
}
