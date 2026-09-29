/* Original Halo PC 1.10 renderer ABI, verified against lifted instructions:
 * 0050BEA0(Renderer*,short,short*,float,float), Renderer stride 0xAC.
 * Two 0x54-byte frustums at +4/+0x58; forward +0x0C, up +0x18, fov +0x28.
 * Render three directions at one simulation time; finish on the central view.
 * No camera/player globals, game scripts, or save files are replaced. */
#include "panorama.h"
#include "panorama_overlay_scope.h"
#include "panorama_budget.h"
#include "panorama_lod.h"
#include "frame_pacer.h"
static int panorama_nested, panorama_ui_nested;
static int panorama_viewmodel_scope;
static int panorama_projection_nested;
static int panorama_visibility_nested, panorama_view_pass = -1;
/* How long the engine spends inside one bearing's scene traversal, summed
 * over a frame. This is the cost that multiplies with the number of
 * bearings, so it is the number that decides whether drawing fewer of them
 * can help at all. */
int host_pass_profile_enabled(void);
void host_pass_profile_add(uint64_t nanoseconds);

static uint64_t panorama_render_sequence;
static uint32_t panorama_renderer;
static unsigned panorama_renderer_count;
/* These follow the live settings so the in-game panel can change them while
 * the game runs. Each is sampled once per frame in the entry hook and never
 * between the two eyes, so a change cannot tear a frame. */
static int panorama_keep_all_views(void) {
    static int cached = -1;
    if (cached < 0) { const char *o = HOST_ENV("HALO_PANORAMA_VIEWS"); cached = (o && !strcmp(o, "all")) ? 1 : 0; }
    return cached;
}
static int panorama_stereo_enabled(void) { return halo_settings_stereo_separation() > 0.f; }
static float panorama_eye_separation(void) { return halo_settings_stereo_separation(); }
/* The bearing budget and the frame clock that steers it. */
static HaloPanoramaBudget panorama_budget;
static int panorama_budget_ready;
static uint64_t panorama_previous_entry_ns, panorama_previous_spin_ns;
/* Live, from the settings panel; HALO_PANORAMA_TARGET_FPS seeds it. */
static float panorama_target_fps(void) { return halo_settings_panorama_target_fps(); }
static int panorama_roll_baseline(void) {
    static int cached = -1;
    if (cached < 0) { const char *o = HOST_ENV("HALO_STEREO_ROLL"); cached = !(o && o[0] == '0'); }
    return cached;
}
/* For the device report: extra passes per two frames and the busy time. */
unsigned host_panorama_budget_extra(void) { return panorama_budget.extra_half; }
unsigned host_panorama_budget_tier(void) { return panorama_budget.tier; }
float host_panorama_busy_seconds(void) { return panorama_budget.busy_ema; }
static float panorama_view_vfov(void) { return halo_settings_panorama_vfov(); }
static float panorama_original_fov[4][2], panorama_fp_xy[4][2];
static unsigned panorama_fp_xy_valid;
static int panorama_fp_upload_nested;
/* Experimental, explicitly enabled per outer render. FP nodes are anchored to
 * the saved central frustum, while source view/projection uploads stay yawed. */
static int panorama_world_fp,panorama_fp_cache_nested;
static unsigned panorama_fp_cache_built;
static float panorama_fp_central_pose[4][9];
/* Object pixel sizes for the frame, per view (panorama_lod.h), and how far
 * this frame's eyes sit from the head centre. */
static HaloPanoramaLod panorama_lod[4];
static float panorama_lod_reach;
/* Objects are measured by straight-line distance from the head centre.
 * HALO_PANORAMA_LOD=nearest measures along the nearest bearing's axis instead
 * (PC LOD over each bearing's own sector); =bearing restores the original
 * per-bearing depth for comparison. */
static int panorama_lod_mode(void) {
    const char *o = HOST_ENV("HALO_PANORAMA_LOD");
    if (o && (!strcmp(o, "bearing") || !strcmp(o, "0"))) return HALO_PANORAMA_LOD_BEARING;
    if (o && !strcmp(o, "nearest")) return HALO_PANORAMA_LOD_NEAREST;
    return HALO_PANORAMA_LOD_DISTANCE;
}
/* 0050F740, an object's pixel size, taken from the frame's head pose instead
 * of this bearing's forward axis. Only in the world passes: the mirror pass
 * (0050BA80 renders it through 0050BFB0 as view -1) looks from a reflected
 * camera, so it keeps the original, as does any pass whose camera is not this
 * frame's head or an eye beside it. The original needs three free x87
 * registers; without them it is left to fail as it would. */
static int panorama_lod_dispatch(EngineCPU *cpu) {
    unsigned view = G16(0x007C3108u);
    if (view >= panorama_renderer_count || view >= 4) return 0;
    HaloPanoramaLod *lod = &panorama_lod[view];
    if (lod->mode == HALO_PANORAMA_LOD_BEARING || (cpu->fp_valid & 0xE0u)) return 0;
    float camera[3]; memcpy(camera, GPTR(0x007C3114u), sizeof camera);
    float dx = camera[0]-lod->head[0], dy = camera[1]-lod->head[1], dz = camera[2]-lod->head[2];
    float reach = fabsf(panorama_lod_reach) * 1.01f + 1e-4f;
    if (!(dx*dx + dy*dy + dz*dz <= reach*reach)) return 0;
    halo_panorama_lod_object_pixels(cpu, lod);
    return 1;
}
/* Bounded proof of central-node reuse versus the live sector view. Hash only
 * one cache root and the source view matrix; never dump asset/vertex bytes. */
static uint32_t panorama_fp_trace_hash(uint32_t address,unsigned size) {
    uint32_t hash=2166136261u;
    for(unsigned i=0;i<size;i++)hash=(hash^G8(address+i))*16777619u;
    return hash;
}
static void panorama_world_fp_trace(EngineCPU *cpu,unsigned view,const char *stage) {
    static unsigned reports;
    const char *option=HOST_ENV("HALO_PANORAMA_WORLD_FP_TRACE");
    if(!option || strcmp(option,"1") || reports>=96 ||
       !(panorama_render_sequence<=4 || panorama_render_sequence%60==0))return;
    uint32_t base=G32(0x006B2D98u),offset=view*0x1EA0u;
    if(!base || base>UINT32_MAX-offset-0x10C0u)return;
    uint32_t cache=base+offset;float history[4],forward[3];
    memcpy(history,GPTR(cache+0x60),sizeof history);
    memcpy(forward,GPTR(0x007C3120u),sizeof forward);
    ++reports;
    host_log("[panorama-world-fp] render=%llu pass=%d view=%u stage=%s pc=%08x built=%x active=%u yaw=%.7f pitch=%.7f previousYaw=%.7f previousPitch=%.7f sectorForward=%.5f,%.5f,%.5f rootHash=%08x viewHash=%08x",
        (unsigned long long)panorama_render_sequence,panorama_view_pass,view,stage,cpu->pc,
        panorama_fp_cache_built,G8(cache),history[0],history[1],history[2],history[3],
        forward[0],forward[1],forward[2],panorama_fp_trace_hash(cache+0x108C,52),
        panorama_fp_trace_hash(0x007C128Cu,52));
}
/* 00518F53 selects output 007C127C; 0050CC40 stores its matrix at
 * +144/+158. 0050C9A0 modifies depth only. Never replace pose/node data. */
#define PANORAMA_FP_M11 0x007C13C0u
#define PANORAMA_FP_M22 0x007C13D4u
static void panorama_fp_projection_trace(EngineCPU *cpu, const char *stage) {
    static unsigned reports;
    const char *option=HOST_ENV("HALO_PANORAMA_FP_PROJECTION_TRACE");
    if(!option || strcmp(option,"1") || reports>=96 ||
       !(panorama_render_sequence<=4 || panorama_render_sequence%60==0))return;
    float x,y;memcpy(&x,GPTR(PANORAMA_FP_M11),4);memcpy(&y,GPTR(PANORAMA_FP_M22),4);
    ++reports;
    host_log("[panorama-fp-projection] render=%llu pass=%d stage=%s pc=%08x m11=%.7f m22=%.7f queue=%u",
        (unsigned long long)panorama_render_sequence,panorama_view_pass,stage,cpu->pc,x,y,G16(0x0071D154u));
}
/* Re-upload restored XY through the original function, covering both D3D
 * SetTransform and shader constants. Its (0,0) ABI restores saved depth.
 * Borrow only this 12-byte call frame below the returned source stack. */
static int panorama_fp_restore_upload(EngineCPU *cpu) {
    if(cpu->gpr[4]<12)return 0;
    EngineCPU returned=*cpu;
    uint32_t at=cpu->gpr[4]-12,return_address=cpu->pc;
    unsigned char saved[12];memcpy(saved,GPTR(at),sizeof saved);
    engine_push(cpu,0,4);engine_push(cpu,0,4);engine_push(cpu,return_address,4);
    panorama_fp_upload_nested=1;engine_dispatch(cpu,0x00518F40u);panorama_fp_upload_nested=0;
    int ok=cpu->pc==return_address;
    uint64_t instructions=cpu->instruction_count;
    memcpy(GPTR(at),saved,sizeof saved);
    if(ok){*cpu=returned;cpu->instruction_count=instructions;}
    return ok;
}
/* Observe real original-engine boundaries; never alter their dispatch/targets.
 * The offline reentry fixture cannot establish which material paths draw the
 * live weapon. Keep this separately opt-in and bounded across the process. */
static void panorama_scope_trace(EngineCPU *cpu, uint32_t address, uint32_t caller, const char *stage) {
    static int enabled = -1;
    static unsigned reports;
    if (enabled < 0) {
        const char *option = HOST_ENV("HALO_PANORAMA_SCOPE_TRACE");
        enabled = option && !strcmp(option, "1");
    }
    if (!enabled || reports >= 96 ||
        !(panorama_render_sequence <= 4 || panorama_render_sequence % 60 == 0)) return;
    ++reports;
    host_log("[panorama-scope] render=%llu pass=%d fn=%08x stage=%s pc=%08x caller=%08x ui=%d viewmodel=%d queue=%u cursor=%u",
        (unsigned long long)panorama_render_sequence, panorama_view_pass,
        address, stage, cpu->pc, caller, panorama_ui_nested,
        panorama_viewmodel_scope, G16(0x0071D154u), G32(0x006D9838u));
}
static int host_panorama_dispatch(EngineCPU *cpu, uint32_t address) {
    /* Called several times per object per pass, so it is tested first. */
    if (address == 0x0050F740u) return panorama_nested && panorama_lod_dispatch(cpu);
    /* Original0050BFB0 calls this no-argument, plain-ret builder at0050C097.
     * 00493740 changes current/previous aim history before constructing world
     * nodes from007C3114's origin/forward/up. Build once per source view using
     * central pose; do not feed synthetic sector yaw into next-tick sway. */
    if(panorama_nested && panorama_world_fp && !panorama_fp_cache_nested &&
       address==0x00492430u && G32(cpu->gpr[4])==0x0050C09Cu){
        unsigned view=G16(0x007C3108u);
        if(view<panorama_renderer_count && view<4){
            if(panorama_fp_cache_built&(1u<<view)){
                ret_to_caller(cpu);panorama_world_fp_trace(cpu,view,"reuse");return 1;
            }
            uint32_t caller=G32(cpu->gpr[4]);unsigned char world_pose[36];
            memcpy(world_pose,GPTR(0x007C3114u),sizeof world_pose);
            memcpy(GPTR(0x007C3114u),panorama_fp_central_pose[view],sizeof world_pose);
            panorama_fp_cache_nested=1;engine_dispatch(cpu,address);panorama_fp_cache_nested=0;
            memcpy(GPTR(0x007C3114u),world_pose,sizeof world_pose);
            if(cpu->pc==caller)panorama_fp_cache_built|=1u<<view;
            panorama_world_fp_trace(cpu,view,cpu->pc==caller?"built":"escaped");
            return 1;
        }
    }
    /* Original material submission can immediately dispatch its draw callback
     * or defer it. Observe its live FP matrix without changing queue logic. */
    if(panorama_nested && panorama_viewmodel_scope && address==0x0052B050u)
        panorama_fp_projection_trace(cpu,"material-submit");
    if(panorama_nested && panorama_viewmodel_scope && !panorama_fp_upload_nested && address==0x00518F40u){
        uint32_t caller=G32(cpu->gpr[4]);
        panorama_fp_upload_nested=1;engine_dispatch(cpu,address);panorama_fp_upload_nested=0;
        if(cpu->pc==caller)panorama_fp_projection_trace(cpu,"source-consumer-upload");
        return 1;
    }
    if (panorama_nested && (address == 0x004924B0u || address == 0x005154A0u))
        panorama_scope_trace(cpu, address, G32(cpu->gpr[4]), "entry");
    /* Observe the original BSP visibility result at its producer boundary.
     * 005537C0 clears the cluster/surface bitsets and their counts, traverses
     * portals, then marks visible surfaces. Keep all of that original logic;
     * these counts distinguish an empty PVS from downstream draw rejection.
     * This dispatch is trace-only and the guard permits the original entry. */
    if (panorama_nested && !panorama_visibility_nested &&
        address == 0x005537C0u && HOST_ENV("HALO_PANORAMA_TRACE") &&
        (panorama_render_sequence <= 4 || panorama_render_sequence % 60 == 0)) {
        uint32_t return_address = G32(cpu->gpr[4]);
        panorama_visibility_nested = 1;
        engine_dispatch(cpu, address);
        panorama_visibility_nested = 0;
        if (cpu->pc == return_address) {
            float origin[3]; memcpy(origin, GPTR(0x007C3114u), sizeof origin);
            host_log("[panorama-visibility] render=%llu pass=%d view=%u leaf=%d cluster=%d clusters=%u surfaces=%u camera=%.3f,%.3f,%.3f",
                (unsigned long long)panorama_render_sequence, panorama_view_pass,
                G16(0x007C310Au), (int32_t)G32(0x007C3344u), (int32_t)G32(0x007C3348u),
                G16(0x007D0390u), G16(0x00850394u), origin[0], origin[1], origin[2]);
        }
        return 1;
    }
    /* Original projection builder, world callers only (sub_0050BA80 twice,
     * sub_0050BDC0 twice; both are reached from the wrapped 0050BEA0 entry).
     * 0050BDC0's two count only for a player view with no camera, where
     * they are the pass's sole projection. The interface record's builds
     * run inside the overlay scope (panorama_ui_nested), so they can never
     * narrow a rectangle or replace the world view's report.
     * ECX = frustum (rect at +0x2C, fov at +0x28); ESI = output record whose
     * 4x4 projection starts at +0x144 (sub_0050CC40: lea edx,[esi+0x144]).
     * 1) HALO_PANORAMA_DENSE: projection-only anamorphic camera. Retain the
     *    full-size raster viewport and vertical coverage while spending
     *    source pixels on the displayed 60-degree band plus a two-degree
     *    guard on either side. 50CC40 reads ECX's rectangle solely while
     *    building projection/frustum data; restore it before the caller
     *    builds the D3D viewport and HUD.
     * 2) Always: after the original returns, hand m[0]/m[5] and the restored
     *    raster rectangle to the panorama recorder. This no longer depends on
     *    a fixed-function draw happening in the pass, so frames drawn only
     *    with programmable shaders keep a valid panorama projection. */
    if (panorama_nested && !panorama_ui_nested && !panorama_projection_nested &&
        address == 0x0050CC40u) {
        uint32_t caller=G32(cpu->gpr[4]), frustum=cpu->gpr[1], output=cpu->gpr[6];
        if (caller==0x0050BC8Bu || caller==0x0050BCA3u ||
            caller==0x0050BE01u || caller==0x0050BE23u) {
            int16_t rect[4];memcpy(rect,GPTR(frustum+0x2C),sizeof rect);
            int height=rect[2]-rect[0],width=rect[3]-rect[1];
            /* Derive the central FP scales with the actual original builder,
             * including its crop rectangle/x87 math. Only primary frustums
             * feed 007C127C. Reuse the source-owned 18C-byte output record,
             * restore it and the 8-byte caller frame before the world call. */
            for(unsigned view=0;view<panorama_renderer_count;view++) {
                if(frustum!=panorama_renderer+view*0xACu+4)continue;
                float fov=panorama_original_fov[view][0];
                if(!output || output>UINT32_MAX-0x18Cu || height<=0 || width<=0 || !isfinite(fov) || fov<=0 || fov>=(float)M_PI)break;
                EngineCPU before=*cpu;
                unsigned char record[0x18C],frame[8];
                float world_fov;memcpy(&world_fov,GPTR(frustum+0x28),4);
                memcpy(record,GPTR(output),sizeof record);memcpy(frame,GPTR(cpu->gpr[4]),sizeof frame);
                memcpy(GPTR(frustum+0x28),&fov,4);
                panorama_projection_nested=1;engine_dispatch(cpu,address);panorama_projection_nested=0;
                float x,y;memcpy(&x,GPTR(output+0x144),4);memcpy(&y,GPTR(output+0x158),4);
                memcpy(GPTR(output),record,sizeof record);memcpy(GPTR(frustum+0x28),&world_fov,4);
                memcpy(GPTR(before.gpr[4]),frame,sizeof frame);
                if(cpu->pc!=caller)return 1; /* outer pass invalidates escape */
                uint64_t instructions=cpu->instruction_count;*cpu=before;cpu->instruction_count=instructions;
                if(isfinite(x)&&isfinite(y)&&x>0&&y>0){panorama_fp_xy[view][0]=x;panorama_fp_xy[view][1]=y;panorama_fp_xy_valid|=1u<<view;}
                break;
            }
            const char *dense=HOST_ENV("HALO_PANORAMA_DENSE");
            if(dense && strcmp(dense,"0") && height>0 && width>0) {
                float fov;memcpy(&fov,GPTR(frustum+0x28),4);
                /* The two cap views look straight up and down. At 32 degrees
                 * either side they cannot reach the sides of the hemisphere,
                 * which left four uncovered wedges past 78 degrees of bearing
                 * between 50 and 57 degrees of elevation: the forward bands
                 * have run out of vertical field there and the caps have run
                 * out of horizontal field. The caps also have to reach far
                 * enough under the bands for the bands to dissolve into them
                 * rather than stop at a line, which needs 48 degrees. It
                 * spends sharpness only on the sky and the floor. */
                float half_degrees = (panorama_view_pass == HALO_PANORAMA_UP ||
                                      panorama_view_pass == HALO_PANORAMA_DOWN) ? 48.0f : 32.0f;
                {const char *wide=HOST_ENV("HALO_PANORAMA_CAP_HALF_FOV");
                 if(wide&&*wide&&half_degrees>40.f){double v=atof(wide);if(v>=20.0&&v<=80.0)half_degrees=(float)v;}}
                float proposed=(isfinite(fov)&&fov>0&&fov<(float)M_PI)?height*tanf(half_degrees*(float)M_PI/180.0f)/tanf(fov*0.5f):0;
                int narrow=(isfinite(proposed)&&proposed>=1&&proposed<width)?(int)lroundf(proposed):0;
                if(narrow>0 && narrow<width && rect[1]+narrow<=INT16_MAX) {
                    S16(frustum+0x32,(uint16_t)(rect[1]+narrow));
                    static unsigned reports;if(reports++<8)
                        host_log("[panorama-density] layer=%d half-fov=%.1f deg projection %dx%d -> %dx%d; raster unchanged, caller=%08x",panorama_view_pass,(double)half_degrees,width,height,narrow,height,caller);
                }
            }
            panorama_projection_nested=1;engine_dispatch(cpu,address);panorama_projection_nested=0;
            memcpy(GPTR(frustum+0x2C),rect,sizeof rect);
            if(output){float m11,m22;memcpy(&m11,GPTR(output+0x144),4);memcpy(&m22,GPTR(output+0x158),4);
                host_panorama_projection(m11,m22,rect[1],rect[0],width,height,caller);}
            return 1;
        }
    }
    /* The two solid rectangles called by the original cinematic letterbox
     * renderer. Keep its animation state, subtitles and fade draws intact;
     * only omit these specific draw calls while building panoramic views.
     * Halo PC 1.10 call sites verified in sub_004499C0 (449B25/449B99).
     * Other callers of the shared rectangle renderer must still execute. */
    if (panorama_nested && address == 0x00449780u) {
        uint32_t caller = G32(cpu->gpr[4]);
        if (caller == 0x00449B2Au || caller == 0x00449B9Eu) {
            static unsigned announced;
            if (announced++ < 2)
                host_log("[panorama-letterbox] omitted original bar caller=%08x", caller);
            ret_to_caller(cpu);
            return 1;
        }
    }
    /* 004924B0 resolves and renders the first-person weapon and hands via
     * 004D6FC0(flags=8), before 0050BFB0's generic 005154A0 material queue.
     * Run the original entries in the views their draws can reach, routing
     * only their nested draws. The game's interface record (0050BDC0 with
     * EAX 0) is one more: its draws go to the HUD layer with the HUD's. */
    if(panorama_nested && !panorama_ui_nested &&
       (halo_panorama_overlay_entry(address) || halo_panorama_interface_entry(address, cpu->gpr[0]))) {
        /* Elsewhere every draw they make is dropped: the HUD's and the
         * interface's outside the HUD pass (gpu_draw), the weapon's outside
         * the forward sector (panorama_skip_ui_draw). Running them there only
         * to throw the work away cost every extra bearing the time of the
         * HUD, the menus and the skinned weapon. The weapon still runs in the
         * two side bearings, which a long weapon can reach across the join.
         * Skipped, the entry is the single call a frame Halo makes without
         * the panorama. HALO_PANORAMA_OVERLAY_ALL_VIEWS=1 runs them in every
         * view. */
        int weapon = address == 0x004924B0u;
        int reach = weapon ? (panorama_view_pass == HALO_PANORAMA_CENTRE_LEFT ||
                              panorama_view_pass == HALO_PANORAMA_CENTRE_RIGHT ||
                              panorama_view_pass == 0 || panorama_view_pass == 2) /* -60, +60 */
                           : panorama_view_pass == HALO_PANORAMA_CENTRE_LEFT;
        const char *all_overlay = HOST_ENV("HALO_PANORAMA_OVERLAY_ALL_VIEWS");
        if (!reach && !(all_overlay && all_overlay[0] == '1')) { ret_to_caller(cpu); return 1; }
        uint32_t return_address = G32(cpu->gpr[4]);
        panorama_viewmodel_scope = address == 0x004924B0u;
        float world_xy[2];int corrected=0;
        unsigned view=G16(0x007C3108u);
        if(panorama_viewmodel_scope && !panorama_world_fp && view<4 && (panorama_fp_xy_valid&(1u<<view))){
            memcpy(&world_xy[0],GPTR(PANORAMA_FP_M11),4);memcpy(&world_xy[1],GPTR(PANORAMA_FP_M22),4);
            memcpy(GPTR(PANORAMA_FP_M11),&panorama_fp_xy[view][0],4);memcpy(GPTR(PANORAMA_FP_M22),&panorama_fp_xy[view][1],4);
            corrected=1;panorama_fp_projection_trace(cpu,"scope-XY");
        }
        if(panorama_viewmodel_scope)host_panorama_viewmodel(1);
        int route_ui=!panorama_viewmodel_scope || !panorama_world_fp;
        panorama_ui_nested=1;if(route_ui)host_panorama_ui(1);
        if (panorama_viewmodel_scope)
            panorama_scope_trace(cpu, address, return_address, "scope-begin");
        engine_dispatch(cpu,address);
        if(route_ui)host_panorama_ui(0);panorama_ui_nested=0;
        if(panorama_viewmodel_scope)host_panorama_viewmodel(0);
        if(corrected){
            memcpy(GPTR(PANORAMA_FP_M11),&world_xy[0],4);memcpy(GPTR(PANORAMA_FP_M22),&world_xy[1],4);
            /* Escaped source execution must not execute more guest code. */
            if(cpu->pc==return_address)panorama_fp_restore_upload(cpu);
            panorama_fp_projection_trace(cpu,"restored-world");
        }
        if (panorama_viewmodel_scope)
            panorama_scope_trace(cpu, address, return_address, "scope-return");
        /* Proof for the probe log that the interface runs once a frame, in
         * the HUD pass, and returns. modal is 00497410's message (0 none). */
        if (address == 0x0050BDC0u && HOST_ENV("HALO_PANORAMA_TRACE") &&
            (panorama_render_sequence <= 4 || panorama_render_sequence % 60 == 0)) {
            static unsigned reports;
            if (reports++ < 24)
                host_log("[panorama-interface] render=%llu pass=%d returned=%d modal=%u",
                         (unsigned long long)panorama_render_sequence, panorama_view_pass,
                         cpu->pc == return_address, G32(0x00718F8Cu));
        }
        panorama_viewmodel_scope=0;
        return 1;
    }
    /* Most translated calls cannot emit this trace. Check their address before
     * entering libc's locked environment lookup on every dispatch. */
    if((address==0x0050BEA0u||address==0x0050BFB0u||address==0x0050BA80u) && HOST_ENV("HALO_PANORAMA_TRACE")) {
        static unsigned seen[3]; int k=address==0x0050BEA0u?0:address==0x0050BFB0u?1:2;
        if(seen[k]++<4)host_log("[panorama-entry] fn=%08x shell=%u args=%08x,%08x,%08x",address,G8(0x00718FC9u),OARG(0),OARG(1),OARG(2));
    }
    if (address != 0x0050BEA0u || panorama_nested) return 0;
    const char *option = HOST_ENV("HALO_PANORAMA");
    int in_shell = G8(0x00718FC9u) != 0;
    if (!option || strcmp(option, "1") || (in_shell && !halo_settings_spatial_shell()) ||
        ((OARG(1) & 0xFFFFu) < 1 || (OARG(1) & 0xFFFFu) > 4)) {
        host_panorama_reset(); return 0;
    }
    static unsigned calls;
    if(calls++<3)host_log("[panorama-hook] renderer=%08x count=%08x",OARG(0),OARG(1));
    uint32_t renderer = OARG(0), stack = cpu->gpr[4], return_address = G32(cpu->gpr[4]);
    unsigned count = OARG(1) & 0xFFFFu;
    size_t record_bytes = count * 0xACu;
    if (!renderer || renderer > UINT32_MAX - record_bytes) return 0;
    unsigned char saved[0xAC * 4], saved_stack[24];
    memcpy(saved, GPTR(renderer), record_bytes);
    panorama_renderer=renderer;panorama_renderer_count=count;
    const char *world_fp=HOST_ENV("HALO_PANORAMA_WORLD_FP");
    panorama_world_fp=world_fp&&!strcmp(world_fp,"1");panorama_fp_cache_built=0;
    for(unsigned view=0;view<count;view++)
        memcpy(panorama_fp_central_pose[view],saved+view*0xACu+4,36);
    for(unsigned view=0;view<count;view++)for(unsigned f=0;f<2;f++)
        memcpy(&panorama_original_fov[view][f],saved+view*0xACu+(f?0x58:4)+0x28,4);
    memcpy(saved_stack, GPTR(stack), sizeof saved_stack);
    EngineCPU incoming = *cpu;
    panorama_nested = 1;
    ++panorama_render_sequence;
    host_panorama_reset();
    /* The camera every bearing of this frame is built from, as the game set
     * it: view 0's first frustum, position +0x00, forward +0x0C, up +0x18,
     * before any pass yaws, pitches or offsets it. Each layer drawn below
     * records it, so the presenter knows which camera an older layer came
     * from. Read from the saved copy; nothing is written. */
    { float pose[HALO_PANORAMA_POSE_FLOATS]; memcpy(pose, saved + 4, sizeof pose); host_panorama_set_camera(pose); }
    /* Nine passes: six bearings round the ring, the sky, the floor, and the
     * centre bearing a second time for the right eye. The saved pose is
     * restored before every pass, so the right vector is always the head's,
     * not the previous bearing's, and each eye stays one consistent
     * viewpoint across its three bearings. */
    /* Stereo is decided below, once the budget has seen this frame's busy
     * time: at its heavy tiers the centre is drawn once for both eyes. */
    int stereo = 0; float separation = 0.f;
    /* Each entry is one drawing: which layer it fills, how far to yaw, how far
     * to pitch, and which eye it is drawn from. The centre bearing is drawn
     * twice, once per eye; the rest are drawn once and shown to both, because
     * depth perception off to the side and overhead is worth far less than the
     * engine time it would cost. The centre pass that carries the HUD is drawn
     * last of the forward three, as before. */
    static const HaloPanoramaView schedule[] = {
        { 0,                         -1.0471975512f, 0.f,            0 },  /* left      */
        { 2,                          1.0471975512f, 0.f,            0 },  /* right     */
        { HALO_PANORAMA_UP,           0.f,           1.5707963268f,  0 },  /* zenith    */
        { HALO_PANORAMA_DOWN,         0.f,          -1.5707963268f,  0 },  /* nadir     */
        { HALO_PANORAMA_REAR_RIGHT,   2.0943951024f, 0.f,            0 },  /* +120 */
        { HALO_PANORAMA_REAR,         3.1415926536f, 0.f,            0 },  /* behind */
        { HALO_PANORAMA_REAR_LEFT,   -2.0943951024f, 0.f,            0 },  /* -120 */
        { HALO_PANORAMA_CENTRE_RIGHT, 0.f,           0.f,            1 },  /* centre, right eye */
        { HALO_PANORAMA_CENTRE_LEFT,  0.f,           0.f,            0 },  /* centre, left eye, carries the HUD */
    };
    const int view_count = (int)(sizeof schedule / sizeof *schedule);
    /* Every bearing repeats the engine traversal. Once pixel/projection carry
     * is primed, the budget (panorama_budget.h) draws the predicted gaze
     * bearing and the centre pair every frame and spends what the measured
     * frame time allows on the rest, stalest and nearest first: one extra
     * bearing every other frame when the engine is slow, the whole sphere
     * every frame when it is fast. HALO_PANORAMA_TARGET_FPS sets the frame
     * rate it keeps (30, Halo's own cap). HALO_PANORAMA_TIGHT_BUDGET=0 restores the
     * older five-pass forward schedule; HALO_PANORAMA_ALL_VIEWS=1 still
     * draws every active layer. Until every layer is ready, always draw the
     * complete sphere. */
    static uint64_t panorama_rotation;
    ++panorama_rotation;
    const char *all_env = HOST_ENV("HALO_PANORAMA_ALL_VIEWS");
    int all_views = all_env && all_env[0] == '1';
    float gaze_yaw = halo_settings_head_yaw(), gaze_pitch = halo_settings_head_pitch();
    /* Which of the layers that may rotate gets its turn this frame. */
    static const int rotating[] = { HALO_PANORAMA_REAR_RIGHT, HALO_PANORAMA_REAR,
                                    HALO_PANORAMA_REAR_LEFT, HALO_PANORAMA_UP, HALO_PANORAMA_DOWN };
    const int rotating_count = (int)(sizeof rotating / sizeof *rotating);
    int turn = rotating[panorama_rotation % (uint64_t)rotating_count];
    const char *budget_env = HOST_ENV("HALO_PANORAMA_TIGHT_BUDGET");
    int tight_budget = !budget_env || budget_env[0] != '0';
    if (!panorama_budget_ready) { halo_panorama_budget_init(&panorama_budget); panorama_budget_ready = 1; }
    /* The frame's busy time is its period less what the frame limiter spent
     * spinning in Sleep(0): at Halo's own 30 fps cap the period is fixed and
     * only the spin says how much of it the engine had to spare. */
    uint64_t entry_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), spin_ns = host_yield_spin_ns;
    static int panorama_was_shell = -1;
    if (panorama_previous_entry_ns) {
        uint64_t period_ns = entry_ns - panorama_previous_entry_ns;
        uint64_t idle_ns = spin_ns - panorama_previous_spin_ns;
        float busy = (float)(period_ns > idle_ns ? period_ns - idle_ns : 0) * 1.0e-9f;
        /* A level was entered: the front end closed, or a load held the frame
         * for over a second in play (a level start or a checkpoint revert).
         * Start the budget over, heavy, and redraw the whole sphere: the
         * layers carried until now show the menu or the world before the
         * jump, and at tier three the ones behind would keep them for seconds. */
        if ((panorama_was_shell == 1 && !in_shell) || (!in_shell && period_ns > 1000000000ull)) {
            halo_panorama_budget_scene_entry(&panorama_budget);
            host_panorama_invalidate();
        } else {
            /* While frames are paced (frame_pacer.h) the ceiling is the
             * cadence's period, so what a slower cadence leaves over goes to
             * bearings rather than to waiting for the slot. */
            halo_panorama_budget_observe_timed(&panorama_budget, busy, (float)period_ns * 1.0e-9f,
                                               host_frame_pacer_budget_target(1.f / panorama_target_fps()),
                                               2u * (unsigned)view_count);
        }
    }
    if (!in_shell) halo_panorama_budget_note_level(&panorama_budget);
    /* Mac measurement only: hold a tier (0-3) so two builds compared on the
     * same level draw the same passes, as the headset does at tier 3. */
    const char *force_tier = HOST_ENV("HALO_PANORAMA_FORCE_TIER");
    if (force_tier && force_tier[0] >= '0' && force_tier[0] <= '3' && !in_shell) {
        panorama_budget.tier = (unsigned)(force_tier[0] - '0'); panorama_budget.extra_half = 1;
    }
    panorama_was_shell = in_shell;
    panorama_previous_entry_ns = entry_ns; panorama_previous_spin_ns = spin_ns;
    stereo = panorama_stereo_enabled() && !(tight_budget && halo_panorama_budget_mono(&panorama_budget));
    separation = stereo ? panorama_eye_separation() : 0.f;
    host_panorama_set_stereo(stereo);
    /* Every bearing and both eyes give an object the same pixel size, from
     * the head pose all of this frame's passes start from, whichever of them
     * the budget draws (panorama_lod.h). */
    int lod_mode = panorama_lod_mode();
    for (unsigned view = 0; view < count; view++)
        halo_panorama_lod_begin(&panorama_lod[view], lod_mode, panorama_fp_central_pose[view], schedule, view_count);
    panorama_lod_reach = separation;
    uint32_t budget_mask = halo_panorama_budget_plan(&panorama_budget, schedule, view_count, stereo,
                                                     panorama_rotation, gaze_yaw, gaze_pitch);
    if (host_panorama_camera_moving() || fabsf(panorama_budget.yaw_rate)>.003f ||
        fabsf(panorama_budget.pitch_rate)>.003f)
        budget_mask=halo_panorama_budget_motion_fill(&panorama_budget,schedule,view_count,stereo,
            budget_mask,gaze_yaw,gaze_pitch,host_frame_pacer_budget_target(1.f/panorama_target_fps()));
    uint32_t drawn_mask = 0;
    int aborted = 0;
    uint64_t frame_pass_ns = 0; unsigned frame_passes = 0;
    /* One render frame per engine frame. 0050BEA0 advances the render-frame
     * counter 0x7C3100 and stores the render time step (its fifth argument)
     * at 0x7C3110 on every call, and the panorama calls it once per bearing.
     * So particles unseen for 15 frames were culled after 15 bearing passes,
     * object lighting was recomputed and blended in every bearing (two
     * bearings of one frame could light an object differently), and the sky
     * (00510C50), weather (00458420) and 004FCE80 integrated the time step
     * once per bearing: too fast, and in a different phase in each. Every
     * bearing now sees the same counter, and only the left-eye centre pass,
     * drawn every frame and last, carries the time step. */
    uint32_t frame_counter = G32(0x007C3100u);
    const char *per_bearing = HOST_ENV("HALO_PANORAMA_FRAME_PER_BEARING");   /* 1: the old behaviour, for comparison */
    int frame_coherent = !(per_bearing && per_bearing[0] == '1');
    /* Halo's lens flares: the flare pipeline tests byte 0x6893FF at every
     * entry (00513780, 005138A0, 00513A00, 00513BA0, 00513CF0). It cost
     * 12.5% of each b30 bearing pass and 32% of each a10 one, and on this
     * host it draws next to nothing: occlusion queries report one pixel, so
     * a flare reaches 24% brightness at most and none for boxes of 69 px2 or
     * more. Its reflections follow each bearing's forward axis, so they
     * cannot stitch either. The video-settings apply (00495580) sets the byte
     * again, so clear it every frame, with the pending queue (0x71D134).
     * HALO_LENS_FLARES=1 keeps them. */
    const char *flares = HOST_ENV("HALO_LENS_FLARES");
    if (!(flares && flares[0] == '1') && G8(0x006893FFu)) { S32(0x0071D134u, 0); S8(0x006893FFu, 0); }
    for (int n = 0; n < view_count; ++n) {
        int pass = schedule[n].layer;
        int eye = stereo ? schedule[n].eye : 0;
        if (!stereo && pass == HALO_PANORAMA_CENTRE_RIGHT) continue;
        if (!all_views && host_panorama_all_layers_ready()) {
            if(tight_budget) {
                if(!(budget_mask & (1u<<pass)))continue;
            } else {
                /* A bearing is "near the gaze" when its centre is within ninety
                 * degrees of where the head is pointing, which is everything the
                 * viewer can actually see plus a margin. The caps count as near
                 * once the head tilts a third of the way to vertical. */
                float separation = fabsf(remainderf(schedule[n].yaw - gaze_yaw, 2.f * (float)M_PI));
                int near_gaze = (pass == HALO_PANORAMA_UP)   ? (gaze_pitch >  0.5f)
                              : (pass == HALO_PANORAMA_DOWN) ? (gaze_pitch < -0.5f)
                              : (separation < 1.5707963268f);
                if (!near_gaze && pass != turn) continue;
            }
        }
        panorama_view_pass = pass;
        panorama_fp_xy_valid=0;
        uint64_t instructions = cpu->instruction_count;
        *cpu = incoming; cpu->instruction_count = instructions;
        memcpy(GPTR(stack), saved_stack, sizeof saved_stack);
        memcpy(GPTR(renderer), saved, record_bytes);
        /* The game's second record is not a second camera. 004C9260 builds
         * the player's view and then the interface record (word -1, byte +2
         * set), which 0050BEA0 sends to 0050BDC0: the cinematic letterbox
         * and chapter titles, the error and loading modal, the game timer and
         * the framerate counter, all 2D over the finished frame. Dropping it
         * saved almost nothing, since it draws no world, and it took all of
         * those out of the panorama, along with the modal's timeout that
         * clears its state. So every bearing asks for the player's view
         * alone, and the left-eye centre pass, which carries the HUD, keeps
         * the game's count: the interface runs once a frame, last, as it
         * does without the panorama, and its draws land on the HUD layer.
         * HALO_PANORAMA_VIEWS=all keeps the game's count in every pass. */
        if (count > 1 && pass != HALO_PANORAMA_CENTRE_LEFT && !panorama_keep_all_views())
            S32(stack + 4u + 4u * 1u, (OARG(1) & ~0xFFFFu) | 1u);
        if (frame_coherent) {
            S32(0x007C3100u, frame_counter);
            if (pass != HALO_PANORAMA_CENTRE_LEFT) S32(stack + 4u + 4u * 4u, 0u);
        }
        float angle = schedule[n].yaw, pitch = schedule[n].pitch;
        /* Left eye sits at -separation along the head's right vector; a view
         * drawn once for both eyes sits on the head's centre line. */
        float shift = !stereo || pass == HALO_PANORAMA_CENTRE_LEFT ? (stereo ? -separation : 0.f)
                    : pass == HALO_PANORAMA_CENTRE_RIGHT ? separation : 0.f;
        for (unsigned view = 0; view < count; ++view) for (int frustum = 0; frustum < 2; ++frustum) {
            /* The interface record keeps the camera the game gave it. Its 2D
             * is one flat picture for every bearing and both eyes, and
             * 0050BDC0 copies that frustum into the view globals at 007C3114,
             * which then end the frame holding what they hold without the
             * panorama. */
            if (saved[view * 0xACu + 2u]) continue;
            uint32_t at = renderer + view * 0xAC + (frustum ? 0x58 : 4);
            if (HOST_ENV("HALO_VIEW_DUMP")) {
                static unsigned dumped;
                if (dumped++ < 8) {
                    int16_t r[4]; memcpy(r, GPTR(at + 0x2C), sizeof r);
                    float fv[3], pv[3]; memcpy(pv, GPTR(at), sizeof pv); memcpy(fv, GPTR(at + 0x0C), sizeof fv);
                    host_log("[view-dump] view=%u frustum=%d rect=%d,%d,%d,%d pos=%.2f,%.2f,%.2f fwd=%.3f,%.3f,%.3f",
                             view, frustum, r[0], r[1], r[2], r[3],
                             (double)pv[0], (double)pv[1], (double)pv[2],
                             (double)fv[0], (double)fv[1], (double)fv[2]);
                }
            }
            float f[3], u[3], right[3], position[3];
            memcpy(position, GPTR(at), sizeof position);
            memcpy(f, GPTR(at + 0x0C), sizeof f); memcpy(u, GPTR(at + 0x18), sizeof u);
            right[0] = f[1]*u[2]-f[2]*u[1];
            right[1] = f[2]*u[0]-f[0]*u[2];
            right[2] = f[0]*u[1]-f[1]*u[0];
            float length = sqrtf(right[0]*right[0]+right[1]*right[1]+right[2]*right[2]);
            if (length > 0.0001f && shift != 0.f) {
                /* The eyes sit along the head's own baseline, which rolls with
                 * the head. An offset along the level camera's right vector
                 * gave a tilted head vertical disparity on the weapon, the one
                 * object near enough for the difference to show; the far scene
                 * hardly moves either way. HALO_STEREO_ROLL=0 restores the level
                 * baseline. */
                float roll = panorama_roll_baseline() ? halo_settings_head_roll() : 0.f;
                float c = cosf(roll), s = sinf(roll);
                for (int k=0;k<3;k++) position[k] += (right[k]/length * c + u[k] * s) * shift;
                memcpy(GPTR(at), position, sizeof position);
            }
            if (length > 0.0001f) for (int k=0;k<3;k++) f[k] = f[k]*cosf(angle)+right[k]/length*sinf(angle);
            /* Pitch for the zenith and nadir views. Rotating forward towards
             * up would leave forward and up parallel at ninety degrees, so the
             * basis is turned as a whole: up takes the place forward vacates. */
            if (pitch != 0.f) {
                float c2 = cosf(pitch), s2 = sinf(pitch), nf[3], nu[3];
                for (int k=0;k<3;k++) nf[k] = f[k]*c2 + u[k]*s2;
                for (int k=0;k<3;k++) nu[k] = u[k]*c2 - f[k]*s2;
                memcpy(f, nf, sizeof f); memcpy(u, nu, sizeof u);
                memcpy(GPTR(at + 0x18), u, sizeof u);
            }
            memcpy(GPTR(at + 0x0C), f, sizeof f);
            /* Broad overlapping views; the presenter selects only each
             * middle 60-degree sector using the actual projection matrix.
             * This field is the VERTICAL field of view: the horizontal 64
             * degrees comes from the narrowed projection rectangle, so
             * widening this costs no extra rendering, it only spreads the
             * same raster over more vertical angle. The presenter derives its
             * cylinder height from the reported projection, so it follows
             * automatically. Rectilinear projection starves the centre as
             * this approaches 180 degrees, so keep it well short.
             * HALO_PANORAMA_VFOV overrides it in degrees. */
            float fov = panorama_view_vfov(); memcpy(GPTR(at + 0x28), &fov, sizeof fov);
        }
        host_panorama_begin(pass);
        /* Timed always: the budget prices a lighter tier in passes. */
        uint64_t pass_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        engine_dispatch(cpu, address);
        uint64_t pass_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - pass_start;
        if (host_pass_profile_enabled()) host_pass_profile_add(pass_ns);
        frame_pass_ns += pass_ns; frame_passes++;
        /* Defensive cleanup: never carry a scoped target into another pass. */
        if (panorama_viewmodel_scope) {
            host_panorama_viewmodel(0);
            if(!panorama_world_fp)host_panorama_ui(0);
            panorama_viewmodel_scope = panorama_ui_nested = 0;
        }
        host_panorama_end(pass);
        /* end() also leaves the host draw target's panorama scope. If the
         * original dispatch escaped instead of returning, never publish this
         * partial render (including an escape during the final center view).
         * Preserve the escaped CPU state for the caller to handle normally. */
        if (cpu->pc != return_address) {
            host_panorama_abort();
            aborted = 1;
            break;
        }
        drawn_mask |= 1u << pass;
    }
    if (!aborted) {
        halo_panorama_budget_drawn(&panorama_budget, drawn_mask, panorama_rotation);
        /* Only frames of the steady schedule; the full sphere after a load
         * would price every pass at its texture-upload cost. */
        if (host_panorama_all_layers_ready())
            halo_panorama_budget_note_passes(&panorama_budget, (float)frame_pass_ns * 1.0e-9f, frame_passes);
    }
    memcpy(GPTR(renderer), saved, record_bytes);
    memcpy(GPTR(stack), saved_stack, sizeof saved_stack);
    panorama_nested = 0;
    panorama_viewmodel_scope = panorama_ui_nested = 0;
    panorama_view_pass = -1;
    panorama_renderer=0;panorama_renderer_count=0;panorama_fp_xy_valid=0;
    panorama_world_fp=panorama_fp_cache_nested=0;panorama_fp_cache_built=0;
    return 1;
}
