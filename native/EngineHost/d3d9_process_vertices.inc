#include "process_vertices_differential.h"
static struct { int initialized,enabled,from_valid;uint32_t first; } pv_differential_config;
static int pv_differential_enabled(void) {
    if(!pv_differential_config.initialized) {
        pv_differential_config.initialized=1;const char *v=getenv("HALO_PV_DIFFERENTIAL");
        pv_differential_config.enabled=v&&!strcmp(v,"1");pv_differential_config.from_valid=1;
        if(pv_differential_config.enabled) {
            const char *from=getenv("HALO_PV_DIFFERENTIAL_FROM_FRAME");
            if(from&&!pv_differential_parse_frame(from,&pv_differential_config.first)) {
                pv_differential_config.from_valid=0;
                host_log("[pv-differential] invalid HALO_PV_DIFFERENTIAL_FROM_FRAME; comparison disabled, authoritative=generic");
            }
        }
    }
    return pv_differential_config.enabled;
}

static int pv_skin_fast_enabled(void) {
    static int enabled=-1;if(enabled<0){const char *v=getenv("HALO_PV_SKIN_FAST");enabled=v&&!strcmp(v,"1");}
    return enabled;
}

static uint32_t process_vertices(uint32_t start,uint32_t dest_index,uint32_t count,
                                  uint32_t dest_guest,uint32_t outdecl,uint32_t flags) {
    D3DObj *vs=obj_from_guest(draw_state.vs),*decl=obj_from_guest(draw_state.decl),*dest=obj_from_guest(dest_guest);
    if(!vs||vs->kind!=K_VSHADER||!decl||decl->kind!=K_VDECL||!dest||dest->kind!=K_VB||outdecl)return D3DERR_INVALIDCALL;
    PVProgram program;PVStream streams[16]={{0}};
    if(!pv_compile(&program,GPTR(vs->data),vs->size,draw_state.vs_float))return D3DERR_NOTAVAILABLE;
    for(unsigned i=0;i<16;i++){
        D3DObj *src=obj_from_guest(draw_state.stream[i]);
        if(src&&src->kind==K_VB)streams[i]=(PVStream){GPTR(src->data),src->size,draw_state.offset[i],draw_state.stride[i]};
    }
    /* This API writes destination bytes without Lock/Unlock. Invalidate at
     * the writer itself, including callers outside the COM method router. */
    if(count)dest->content_generation++;
    int ok,differential=pv_differential_enabled();
    static unsigned checked,matched,skipped,mismatched,restore_failed,logs;
    if(differential&&pv_differential_frame_allowed(frames_presented+1,pv_differential_config.first,pv_differential_config.from_valid)&&checked<128&&pv_skin_shader_matches(GPTR(vs->data),vs->size)) {
        PVDifferentialResult result;checked++;
        ok=pv_process_differential(&program,GPTR(decl->data),decl->size,streams,start,count,GPTR(dest->data),dest->size,dest_index,dest->fvf,flags,&result);
        if(result.skipped)skipped++;else if(result.matched)matched++;else mismatched++;
        restore_failed+=result.environment_restore_failed;
        if(logs<32&&(checked<=4||checked%32==0||(!result.skipped&&!result.matched)||result.environment_restore_failed)) {
            logs++;
            host_log("[pv-differential] frame=%u pass=%d check=%u count=%u vs=%08x decl=%08x generic=%u candidate=%u skip=%u matched=%u firstByte=%llu genericWord=%08x candidateWord=%08x bytes=%llu totals=%u/%u/%u restoreFailures=%u authoritative=generic",
                frames_presented+1,capture_panorama_pass(),checked,count,draw_state.vs,draw_state.decl,result.generic_ok,result.candidate_ok,result.skipped,result.matched,
                (unsigned long long)result.first_byte,result.generic_word,result.candidate_word,(unsigned long long)result.bytes,matched,mismatched,skipped,restore_failed);
        }
    } else ok=pv_process_skin_dispatch(&program,GPTR(vs->data),vs->size,GPTR(decl->data),decl->size,streams,start,count,GPTR(dest->data),dest->size,dest_index,dest->fvf,flags,
        pv_skin_fast_enabled(),differential); /* Differential stays generic after its cap too. */
    if(!ok)return D3DERR_INVALIDCALL;
    return D3D_OK;
}
