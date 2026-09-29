/* Included after the D3D surface helpers. Kept independent of the visionOS UI
 * so the identical multi-view rendering path can be tested on the Mac. */
static int panorama_pass = -1, panorama_ui_active;
void host_panorama_ui(int active){panorama_ui_active=active;}
static D3DObj *panorama_hud;
static HaloPanoramaInfo panorama_info;
static uint8_t *panorama_pixels[HALO_PANORAMA_LAYERS];
static size_t panorama_capacity;
/* Zero-copy sink state: with a sink, each pass end blits into the slot's textures. */
static HaloPanoramaGPUSink panorama_sink; static int panorama_sink_set;
static int panorama_gpu_slot=-1; static void *panorama_gpu_textures[HALO_PANORAMA_LAYERS];
void host_panorama_set_gpu_sink(const HaloPanoramaGPUSink *sink){
    if(sink&&sink->acquire&&sink->release){panorama_sink=*sink;panorama_sink_set=1;}
    else{memset(&panorama_sink,0,sizeof panorama_sink);panorama_sink_set=0;}
}
int host_panorama_gpu_slot(void){return panorama_gpu_slot;}
int host_panorama_gpu_sink_active(void){return panorama_sink_set;}
void host_panorama_gpu_handoff(void){panorama_gpu_slot=-1;}
static void panorama_gpu_drop(void){
    if(panorama_gpu_slot>=0&&panorama_sink_set)panorama_sink.release(panorama_gpu_slot);
    panorama_gpu_slot=-1;
}
static unsigned panorama_mask;
/* Layers that hold a picture, whether or not this frame drew them.
 *
 * The three bearings behind the viewer and the two caps are refreshed in
 * rotation rather than every frame, because they are not where anyone is
 * looking and each one costs a whole pass of the engine's scene traversal.
 * Their pixels persist between frames, so a frame is complete when every
 * layer holds something, not when every layer was drawn this tick. Cleared
 * whenever the layer set itself changes. */
static unsigned panorama_ready_mask;
/* The size the held layers were captured at, so a resolution change can drop
 * them rather than mixing two sizes into one sphere. */
static uint32_t panorama_ready_width, panorama_ready_height;
/* Per-layer projection and source rectangle from the last pass that drew
 * each layer, carried into every frame that holds the layer without
 * redrawing it. Only read for layers in panorama_ready_mask. */
static float panorama_held_px[HALO_PANORAMA_LAYERS], panorama_held_py[HALO_PANORAMA_LAYERS];
static float panorama_held_viewport[HALO_PANORAMA_LAYERS][4];
static uint32_t panorama_stereo_flag;
static uint64_t panorama_source_epoch;
static uint64_t panorama_scene_epoch = 1;
static uint64_t panorama_held_epoch[HALO_PANORAMA_LAYERS];
/* The camera this frame's passes start from, and the one every held layer
 * was drawn with (panorama.h, layer_pose). Recorded for the presenter only;
 * nothing here changes what the engine draws. */
static float panorama_frame_pose[HALO_PANORAMA_POSE_FLOATS];
static float panorama_held_pose[HALO_PANORAMA_LAYERS][HALO_PANORAMA_POSE_FLOATS];
/* Camera cuts. The stick turns the camera at most 2.3 degrees a frame
 * (d3d9.c adds 25 mouse counts per Present, 0.04 rad) and nothing the player
 * drives covers two world units (six metres) between two frames. Cinematic
 * sweeps can, however: a large step is a cut only if it also departs from
 * the previous displacement by that amount. The first frame after one,
 * or after a scene change, becomes cut_epoch, and layers drawn before it are
 * never turned to line up with the new shot. */
#define PANORAMA_CUT_DISTANCE 2.0f
#define PANORAMA_CUT_RADIANS 0.5235988f
static float panorama_previous_pose[HALO_PANORAMA_POSE_FLOATS];
static float panorama_previous_displacement[3];
static int panorama_have_displacement;
static int panorama_previous_pose_set, panorama_cut_pending = 1;
static uint64_t panorama_cut_epoch;
static int panorama_camera_moving;
int host_panorama_camera_moving(void) { return panorama_camera_moving; }
static int panorama_pose_usable(const float *p){
    for(int k=0;k<HALO_PANORAMA_POSE_FLOATS;k++)if(!isfinite(p[k]))return 0;
    return p[3]*p[3]+p[4]*p[4]+p[5]*p[5]>1e-6f && p[6]*p[6]+p[7]*p[7]+p[8]*p[8]>1e-6f;
}
static float panorama_pose_angle(const float *a,const float *b){
    float dot=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
    float length=sqrtf((a[0]*a[0]+a[1]*a[1]+a[2]*a[2])*(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]));
    float c=length>0?dot/length:1.f;
    return acosf(c>1.f?1.f:c<-1.f?-1.f:c);
}
void host_panorama_set_camera(const float pose[HALO_PANORAMA_POSE_FLOATS]){
    panorama_camera_moving=0;
    if(!pose||!panorama_pose_usable(pose)){
        memset(panorama_frame_pose,0,sizeof panorama_frame_pose);
        panorama_previous_pose_set=0;panorama_have_displacement=0;panorama_cut_pending=1;return;
    }
    /* No camera to compare with (the first frame, or the one after an
     * unusable camera): nothing drawn before can be lined up with this one. */
    if(!panorama_previous_pose_set)panorama_cut_pending=1;
    else{
        float dx=pose[0]-panorama_previous_pose[0],dy=pose[1]-panorama_previous_pose[1],dz=pose[2]-panorama_previous_pose[2];
        float distance=sqrtf(dx*dx+dy*dy+dz*dz);
        float ex=dx-panorama_previous_displacement[0];
        float ey=dy-panorama_previous_displacement[1];
        float ez=dz-panorama_previous_displacement[2];
        float departure=panorama_have_displacement?sqrtf(ex*ex+ey*ey+ez*ez):distance;
        panorama_previous_displacement[0]=dx;
        panorama_previous_displacement[1]=dy;
        panorama_previous_displacement[2]=dz;
        panorama_have_displacement=1;
        float forward_angle=panorama_pose_angle(pose+3,panorama_previous_pose+3);
        float up_angle=panorama_pose_angle(pose+6,panorama_previous_pose+6);
        panorama_camera_moving=distance>0.01f || forward_angle>0.003f || up_angle>0.003f;
        if((!(distance<=PANORAMA_CUT_DISTANCE) && !(departure<=PANORAMA_CUT_DISTANCE))||
           !(forward_angle<=PANORAMA_CUT_RADIANS)||
           !(up_angle<=PANORAMA_CUT_RADIANS)) {
            /* A new shot must refresh the whole sphere. Merely stamping
             * cut_epoch left previous-shot pixels eligible for CPU/GPU carry
             * for seconds at heavy tiers. Advance scene_epoch as well so a
             * sink cannot offer old-shot metadata during the first new pass. */
            host_panorama_invalidate();
        }
    }
    memcpy(panorama_previous_pose,pose,sizeof panorama_previous_pose);panorama_previous_pose_set=1;
    memcpy(panorama_frame_pose,pose,sizeof panorama_frame_pose);
}
static float panorama_draw_viewport[4], panorama_builder_viewport[4];
/* A failure belongs to the entire epoch, not just the current bearing. */
static int panorama_frame_failed;
/* panorama_px/py: fallback taken from the D3D projection at the first
 * depth-enabled fixed-function world draw (d3d9_render.inc, ff3d_draw).
 * panorama_builder_px/py: authoritative value recorded from the original
 * projection builder for the world callers, independent of draw type. */
static float panorama_px, panorama_py;
static float panorama_builder_px, panorama_builder_py;
static uint32_t panorama_builder_caller;
/* The backbuffer, or Halo's full-size offscreen scene target (its shader
 * path renders the world into a render-target texture and resolves it with a
 * final quad). HUD/UI draws aimed at either belong to the HUD layer. */
static int panorama_scene_target(const D3DObj *surface) {
    if(!surface||!the_device)return 0;
    if(surface->guest==the_device->data)return 1;
    D3DObj *bb=obj_from_guest(the_device->data);
    return bb && surface->width==bb->width && surface->height==bb->height && surface->guest!=0;
}
/* The pass whose HUD surface is published. Interface draws are executed only
 * during this pass: the HUD is one 2D image, identical for every bearing and
 * both eyes, so drawing it six times cost six times as much and, worse, the
 * five copies that were not routed to the HUD landed in the side bearings'
 * world images, which is what put a seam through the menu. */
static int panorama_hud_pass(void) { return 1; }
/* True when an interface draw should be skipped rather than rendered.
 *
 * Only in the front end. There the interface is a flat 2D image that belongs
 * on the HUD layer alone, and drawing it in the other bearings put the logo
 * and menu text into the side views at different positions, which is the seam
 * that was visible through the menu.
 *
 * The first-person weapon needs the same treatment for a different reason.
 * It is real geometry, but it is attached to the camera, so drawing it once
 * per bearing puts a copy of the weapon sixty degrees to the left, sixty to
 * the right, and three more around the back. It belongs in the forward view
 * and nowhere else, which is the bearing the two centre layers carry. */
static int panorama_skip_ui_draw(void) {
    if (!panorama_ui_active || panorama_pass < 0) return 0;
    int forward = HALO_PANORAMA_SECTOR_OF(panorama_pass) == 1;
    if (fp_capture.scope) return !forward;
    if (panorama_pass == panorama_hud_pass()) return 0;
    return G8(0x00718FC9u) != 0;   /* shell flag: the front end is showing */
}
static D3DObj *panorama_draw_surface(void) {
    D3DObj *surface=rt_surface();
    if(panorama_ui_active && panorama_pass==panorama_hud_pass() && panorama_hud && the_device && surface && panorama_scene_target(surface))
        return panorama_hud;
    return surface;
}
void host_panorama_projection(float m11,float m22,int viewport_x,int viewport_y,int viewport_w,int viewport_h,uint32_t caller) {
    D3DObj *bb=the_device?obj_from_guest(the_device->data):NULL;
    if(panorama_pass<0||panorama_ui_active||!bb||!bb->width||!bb->height||viewport_w<=0||viewport_h<=0)return;
    /* Same normalisation as ff3d_draw: |scale| * viewport extent / back buffer extent,
     * so cinematic letterboxing (a shorter raster rectangle) is preserved. */
    float px=fabsf(m11)*(float)viewport_w/(float)bb->width, py=fabsf(m22)*(float)viewport_h/(float)bb->height;
    if(!isfinite(px)||!isfinite(py)||px<=0||py<=0)return;
    if(panorama_builder_px>0){ /* keep the first world caller; report later ones that disagree */
        if(HOST_ENV("HALO_PANORAMA_TRACE")&&(fabsf(px-panorama_builder_px)>0.01f*panorama_builder_px||fabsf(py-panorama_builder_py)>0.01f*panorama_builder_py)){
            static unsigned reports;if(reports++<8)host_log("[panorama-projection] pass=%d caller=%08x %.4f,%.4f disagrees with first caller %08x %.4f,%.4f",panorama_pass,caller,px,py,panorama_builder_caller,panorama_builder_px,panorama_builder_py);
        }
        return;
    }
    panorama_builder_px=px;panorama_builder_py=py;panorama_builder_caller=caller;
    panorama_builder_viewport[0]=(float)viewport_x/bb->width;
    panorama_builder_viewport[1]=(float)viewport_y/bb->height;
    panorama_builder_viewport[2]=((float)viewport_x+viewport_w)/bb->width;
    panorama_builder_viewport[3]=((float)viewport_y+viewport_h)/bb->height;
    if(HOST_ENV("HALO_PANORAMA_TRACE")){static unsigned reports;if(reports++<12)
        host_log("[panorama-projection] pass=%d caller=%08x m11=%.4f m22=%.4f raster=%dx%d -> %.4f,%.4f",panorama_pass,caller,m11,m22,viewport_w,viewport_h,px,py);}
}
void host_panorama_set_stereo(int on){panorama_stereo_flag=on?1u:0u;panorama_info.stereo=panorama_stereo_flag;}
int host_panorama_all_layers_ready(void){
    unsigned want=panorama_stereo_flag?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
    return (panorama_ready_mask&want)==want;
}
void host_panorama_reset(void) {
    panorama_mask=0;memset(&panorama_info,0,sizeof panorama_info);
    panorama_info.stereo=panorama_stereo_flag;
    panorama_info.scene_epoch=panorama_scene_epoch;
    panorama_info.cut_epoch=panorama_cut_epoch;
    panorama_pass=-1;panorama_ui_active=0;panorama_frame_failed=0;
    panorama_gpu_drop();
}
void host_panorama_invalidate(void) {
    panorama_scene_epoch++;
    panorama_cut_pending=1;
    host_panorama_reset();
    panorama_ready_mask=0;panorama_ready_width=panorama_ready_height=0;
}
void host_panorama_present_complete(void) {
    /* A flat/loading frame breaks scene continuity. The next sphere must
     * refresh every bearing even if resolution and stereo mode are unchanged. */
    if(!panorama_info.source_epoch)host_panorama_invalidate();
    else host_panorama_reset();
}
void host_panorama_abort(void) {
    panorama_mask=0;panorama_ready_mask=0;panorama_ready_width=panorama_ready_height=0;panorama_info.valid=0;
    panorama_info.status=HALO_PANORAMA_WORLD_INCOMPLETE;
    panorama_info.failure_reason=HALO_PANORAMA_RENDER_ABORTED;
    panorama_frame_failed=1;
    panorama_pass=-1;panorama_ui_active=0;
}
static void panorama_fail(unsigned reason){
    panorama_info.valid=0;panorama_info.status=HALO_PANORAMA_WORLD_INCOMPLETE;
    if(!panorama_frame_failed)panorama_info.failure_reason=reason;
    panorama_frame_failed=1;
    /* CPU captures may already contain part of this failed epoch; force every
     * bearing to refresh before a later epoch becomes eligible to publish. */
    panorama_ready_mask=0;
    panorama_gpu_drop();
}
void host_panorama_begin(int pass) {
    /* Rotation may skip layer zero. Start the epoch on the first actual
     * bearing, and never reset a frame merely because layer zero runs later. */
    if(!panorama_info.source_epoch){
        host_panorama_reset();panorama_info.source_epoch=++panorama_source_epoch;
        if(panorama_cut_pending){panorama_cut_epoch=panorama_info.source_epoch;panorama_cut_pending=0;}
        panorama_info.cut_epoch=panorama_cut_epoch;
        panorama_info.status=HALO_PANORAMA_WORLD_INCOMPLETE;
        panorama_info.failure_reason=HALO_PANORAMA_MISSING_PASS;
    }
    panorama_pass=pass; panorama_ui_active=0; panorama_px=panorama_py=0;
    if(panorama_frame_failed)return;
    memset(panorama_draw_viewport,0,sizeof panorama_draw_viewport);
    memset(panorama_builder_viewport,0,sizeof panorama_builder_viewport);
    panorama_builder_px=panorama_builder_py=0; panorama_builder_caller=0;
    D3DObj *bb=the_device?obj_from_guest(the_device->data):NULL;
    if(!bb){panorama_fail(HALO_PANORAMA_SIZE_MISMATCH);return;}
    /* The pass clears the target right away, so the guest copy of the back
     * buffer (fresh from the previous pass's readback on the CPU path) must
     * not be uploaded first: mark the GPU copy authoritative before prepare. */
    if(bb->renderer)bb->gpu_dirty=1;
    if(!surface_prepare(bb)) {
        mr_clear(bb->renderer,0xFF000000u);mr_clear_depth(bb->renderer,1.0f);bb->gpu_dirty=1;
    }else{panorama_fail(HALO_PANORAMA_READBACK_FAILED);return;}
    if(panorama_gpu_slot<0&&panorama_sink_set){
        /* Zero-copy frame: lend a pool slot for every layer. A busy pool drops
         * this frame; the compositor keeps its last complete one.
         *
         * Taken on the first pass of the frame, whichever that is, not on
         * layer zero: the bearings behind the viewer are refreshed in
         * rotation, so layer zero is not always drawn. */
        if(!panorama_sink.acquire(bb->width,bb->height,panorama_gpu_textures,&panorama_gpu_slot)){
            panorama_gpu_slot=-1;panorama_fail(HALO_PANORAMA_ALLOCATION_FAILED);return;
        }
    }
    if(HALO_PANORAMA_SECTOR_OF(pass)==1){
        if(!panorama_hud || panorama_hud->width!=bb->width || panorama_hud->height!=bb->height) {
            if(!panorama_hud)panorama_hud=calloc(1,sizeof *panorama_hud);
            if(!panorama_hud){panorama_fail(HALO_PANORAMA_ALLOCATION_FAILED);return;}
            uint32_t old_data=panorama_hud->data,new_data=guest_page_alloc(bb->size);
            if(!new_data){panorama_fail(HALO_PANORAMA_ALLOCATION_FAILED);return;}
            /* Destroy drains any GPU work before releasing this owned CPU copy. */
            if(panorama_hud->renderer)mr_destroy(panorama_hud->renderer);
            memset(panorama_hud,0,sizeof *panorama_hud);
            panorama_hud->width=bb->width;panorama_hud->height=bb->height;
            panorama_hud->format=21;panorama_hud->size=bb->size;
            panorama_hud->data=new_data;
            if(old_data)guest_page_free(old_data);
        }
        /* Create the target without uploading guest pixels: both attachments
         * are cleared before use, including the first HUD frame. CPU consumers
         * go through surface_flush and receive the actual cleared/drawn HUD. */
        panorama_hud->gpu_dirty=1;
        if(surface_prepare(panorama_hud)){
            panorama_fail(HALO_PANORAMA_ALLOCATION_FAILED);return;
        }
        mr_clear(panorama_hud->renderer,0x00000000u);
        mr_clear_depth(panorama_hud->renderer,1.0f);
    }
}
void host_panorama_end(int pass) {
    D3DObj *bb=the_device?obj_from_guest(the_device->data):NULL;
    panorama_pass=-1;
    int builder=panorama_builder_px>0&&panorama_builder_py>0;
    float px=builder?panorama_builder_px:panorama_px, py=builder?panorama_builder_py:panorama_py;
    float *viewport=builder?panorama_builder_viewport:panorama_draw_viewport;
    if(builder&&panorama_px>0&&HOST_ENV("HALO_PANORAMA_TRACE")&&(fabsf(px-panorama_px)>0.01f*px||fabsf(py-panorama_py)>0.01f*py)){
        static unsigned reports;if(reports++<8)host_log("[panorama-projection] pass=%d builder %.4f,%.4f differs from draw-time %.4f,%.4f",pass,px,py,panorama_px,panorama_py);
    }
    if(panorama_frame_failed)return;
    if(pass<0||pass>=HALO_PANORAMA_LAYERS||pass==HALO_PANORAMA_HUD_LAYER||!bb){panorama_fail(HALO_PANORAMA_SIZE_MISMATCH);return;}
    if(!isfinite(px)||!isfinite(py)||px<=0||py<=0){
        /* This is the path that turns a presented frame into the flat fallback. */
        static unsigned reports;if(reports++<16)
            host_log("[panorama] frame=%u pass=%d dropped: no world projection (builder=%.4f draw=%.4f)",frames_presented+1,pass,panorama_builder_px,panorama_px);
        panorama_fail(HALO_PANORAMA_NO_PROJECTION);return;
    }
    if(viewport[0]<0||viewport[1]<0||viewport[2]>1||viewport[3]>1||
       viewport[2]<=viewport[0]||viewport[3]<=viewport[1]){
        panorama_fail(HALO_PANORAMA_INVALID_VIEWPORT);return;
    }
    /* Every pass within one frame must agree on size. This asks
     * panorama_mask, which is per frame, and not panorama_ready_mask, which
     * persists: panorama_info is zeroed at the start of every frame, so
     * comparing a persistent mask against a zeroed width failed on the first
     * pass of every frame after the first and blacked the whole view out. */
    if(panorama_mask&&(panorama_info.width!=bb->width||panorama_info.height!=bb->height)){
        panorama_fail(HALO_PANORAMA_SIZE_MISMATCH);return;
    }
    /* A resolution change does invalidate layers held from earlier frames. */
    if(panorama_ready_mask&&panorama_ready_width&&
       (panorama_ready_width!=bb->width||panorama_ready_height!=bb->height))
        panorama_ready_mask=0;
    panorama_ready_width=bb->width; panorama_ready_height=bb->height;
    size_t bytes=bb->size;
    if(panorama_sink_set){
        /* Zero-copy: blit this view (and the HUD after the centre pass) into the
         * lent pool textures; the GPU orders the blit behind the pass's draws. */
        if(panorama_gpu_slot<0){panorama_fail(HALO_PANORAMA_ALLOCATION_FAILED);return;}
        /* World layers go through FXAA on the way (mr_fxaa_target_to); the HUD
         * below is copied as drawn. HALO_FXAA=0 copies world layers plainly. */
        const char *fxaa=HOST_ENV("HALO_FXAA");
        int filtered=!(fxaa&&fxaa[0]=='0');
        if(!bb->renderer||(filtered?mr_fxaa_target_to(bb->renderer,panorama_gpu_textures[pass],0.5f)
                                   :mr_blit_target_to(bb->renderer,panorama_gpu_textures[pass]))){panorama_fail(HALO_PANORAMA_READBACK_FAILED);return;}
        if(pass==1){
            if(!panorama_hud||surface_prepare(panorama_hud)||mr_blit_target_to(panorama_hud->renderer,panorama_gpu_textures[HALO_PANORAMA_HUD_LAYER])){panorama_fail(HALO_PANORAMA_READBACK_FAILED);return;}
        }
    }else{
    if(surface_flush(bb)){panorama_fail(HALO_PANORAMA_READBACK_FAILED);return;}
    if(panorama_capacity!=bytes){
        panorama_mask=0;panorama_ready_mask=0;panorama_ready_width=panorama_ready_height=0;panorama_info.valid=0;
        for(int k=0;k<HALO_PANORAMA_LAYERS;k++){free(panorama_pixels[k]);panorama_pixels[k]=calloc(1,bytes);}
        panorama_capacity=bytes;
    }
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if(!panorama_pixels[k]){panorama_fail(HALO_PANORAMA_ALLOCATION_FAILED);return;}
    memcpy(panorama_pixels[pass],GPTR(bb->data),bytes);
    if(pass==1&&panorama_hud){
        if(surface_flush(panorama_hud)){panorama_fail(HALO_PANORAMA_READBACK_FAILED);return;}
        memcpy(panorama_pixels[HALO_PANORAMA_HUD_LAYER],GPTR(panorama_hud->data),bytes);
    }else if(pass==1){panorama_fail(HALO_PANORAMA_ALLOCATION_FAILED);return;}
    }
    panorama_info.width=bb->width;panorama_info.height=bb->height;
    panorama_info.projection_x[pass]=px;panorama_info.projection_y[pass]=py;
    panorama_info.viewport_u_min[pass]=viewport[0];panorama_info.viewport_v_min[pass]=viewport[1];
    panorama_info.viewport_u_max[pass]=viewport[2];panorama_info.viewport_v_max[pass]=viewport[3];
    panorama_mask|=1u<<pass; panorama_ready_mask|=1u<<pass;
    panorama_info.layer_epoch[pass]=panorama_info.source_epoch;
    panorama_held_epoch[pass]=panorama_info.source_epoch;
    memcpy(panorama_info.layer_pose[pass],panorama_frame_pose,sizeof panorama_frame_pose);
    memcpy(panorama_held_pose[pass],panorama_frame_pose,sizeof panorama_frame_pose);
    if(pass==HALO_PANORAMA_CENTRE_LEFT){
        panorama_info.layer_epoch[HALO_PANORAMA_HUD_LAYER]=panorama_info.source_epoch;
        memcpy(panorama_info.layer_pose[HALO_PANORAMA_HUD_LAYER],panorama_frame_pose,sizeof panorama_frame_pose);
    }
    /* A layer's picture is carried between frames, so its projection and
     * source rectangle must be carried with it. panorama_info is wiped at
     * every frame start and only the passes that ran this frame write their
     * layer back, so under the rotating schedule a skipped layer published a
     * projection of zero. The compositor rejects a frame whose ring or caps
     * carry an impossible projection, and it rejected every one: rotation on
     * the headset was a black screen with the host reporting COMPLETE. The
     * pixels alone were never enough to carry. */
    panorama_held_px[pass]=px;panorama_held_py[pass]=py;
    for(int j=0;j<4;j++)panorama_held_viewport[pass][j]=viewport[j];
    /* Only layers this frame's mode shows are carried. In mono the right
     * eye's centre is not drawn or sampled, and carrying its last stereo
     * epoch reported a picture thousands of frames stale that nobody was
     * looking at (Build73 layer 4, 6342 frames behind at the end). */
    unsigned shown=panorama_info.stereo?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++){
        if(!(shown&(1u<<k))||!(panorama_ready_mask&(1u<<k))||(panorama_mask&(1u<<k)))continue;
        panorama_info.layer_epoch[k]=panorama_held_epoch[k];
        memcpy(panorama_info.layer_pose[k],panorama_held_pose[k],sizeof panorama_held_pose[k]);
        panorama_info.projection_x[k]=panorama_held_px[k];panorama_info.projection_y[k]=panorama_held_py[k];
        panorama_info.viewport_u_min[k]=panorama_held_viewport[k][0];panorama_info.viewport_v_min[k]=panorama_held_viewport[k][1];
        panorama_info.viewport_u_max[k]=panorama_held_viewport[k][2];panorama_info.viewport_v_max[k]=panorama_held_viewport[k][3];
    }
    /* Layers 0,1,2 and 7,8,9 are the ring of six bearings, 5 and 6 the zenith
     * and nadir, 3 the HUD, and 4 the centre bearing seen by the right eye,
     * which only stereo produces. */
    {unsigned want=panorama_info.stereo?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
     panorama_info.valid=(panorama_ready_mask&want)==want;}
    if(panorama_info.valid){panorama_info.status=HALO_PANORAMA_COMPLETE;panorama_info.failure_reason=HALO_PANORAMA_OK;}
    if(panorama_info.valid && (frames_presented<5 || frames_presented%60==0))
        host_log("[panorama] frame=%u %ux%u three views + HUD projection=%.4f,%.4f (%s)",frames_presented+1,bb->width,bb->height,px,py,builder?"builder":"draw");
    const char *folder=HOST_ENV("HALO_FRAME_CAPTURE");
    if(folder&&panorama_info.valid&&HOST_ENV("HALO_PANORAMA_CAPTURE")&&frames_presented%60==0){
        for(int k=0;k<HALO_PANORAMA_LAYERS;k++){char path[2048];snprintf(path,sizeof path,"%s/panorama-%04u-%d.bgra",folder,frames_presented+1,k);FILE*f=panorama_pixels[k]?fopen(path,"wb"):NULL;if(f){fwrite(panorama_pixels[k],1,bytes,f);fclose(f);}}
    }
}
void host_panorama_gpu_carry_missing(void){
    if(panorama_gpu_slot<0||!panorama_sink_set||!panorama_info.valid)return;
    unsigned want=panorama_info.stereo?HALO_PANORAMA_STEREO_MASK:HALO_PANORAMA_MONO_MASK;
    unsigned missing=want&~panorama_mask;
    /* A full first frame needs no source. Ignore the inactive right eye when
     * switching to mono; only layers the presenter will sample must carry. */
    if(!missing)return;
    HaloPanoramaInfo source={0};
    unsigned copied=panorama_sink.carry?panorama_sink.carry(panorama_gpu_slot,missing,&source):0;
    if(source.scene_epoch!=panorama_info.scene_epoch||source.width!=panorama_info.width||
       source.height!=panorama_info.height)copied=0;
    for(int k=0;k<HALO_PANORAMA_LAYERS;k++)if((copied&missing)&(1u<<k)){
        if(!source.layer_epoch[k]||source.layer_epoch[k]>panorama_info.source_epoch||
           !isfinite(source.projection_x[k])||source.projection_x[k]<=0||
           !isfinite(source.projection_y[k])||source.projection_y[k]<=0){copied&=~(1u<<k);continue;}
        panorama_info.layer_epoch[k]=source.layer_epoch[k];
        memcpy(panorama_info.layer_pose[k],source.layer_pose[k],sizeof source.layer_pose[k]);
        panorama_info.projection_x[k]=source.projection_x[k];panorama_info.projection_y[k]=source.projection_y[k];
        panorama_info.viewport_u_min[k]=source.viewport_u_min[k];panorama_info.viewport_v_min[k]=source.viewport_v_min[k];
        panorama_info.viewport_u_max[k]=source.viewport_u_max[k];panorama_info.viewport_v_max[k]=source.viewport_v_max[k];
    }
    if((copied&missing)!=missing){
        /* Held readiness describes CPU history, not necessarily the latest
         * successfully published GPU slot. Force a full refresh after failure. */
        panorama_ready_mask=0;
        panorama_info.failure_reason=HALO_PANORAMA_READBACK_FAILED;
        panorama_fail(HALO_PANORAMA_READBACK_FAILED);
    }
}
int host_panorama_frame(HaloPanoramaInfo *info,const void *layers[HALO_PANORAMA_LAYERS]){
    *info=panorama_info;for(int k=0;k<HALO_PANORAMA_LAYERS;k++)layers[k]=panorama_pixels[k];return panorama_info.valid;
}
/* Platforms without a zero-copy presenter (the macOS probe) never set a sink,
 * so this is never reached there; the visionOS runtime overrides it. */
__attribute__((weak)) void metalwin_present_gpu(int slot,int width,int height){(void)slot;(void)width;(void)height;}
__attribute__((weak)) void metalwin_present_dropped(int width,int height){(void)width;(void)height;}
