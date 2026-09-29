/* Direct3D9 pretransformed drawing and surface transfer bridge.
 * Included after the COM object definitions. Unsupported shader/state paths
 * return a real failure and retain a diagnostic; they are never counted as draws.
 */
typedef struct {
    uint32_t rs[256], ts[8][33], sampler[16][14], texture[16];
    uint32_t fvf, vs, ps, decl, stream[16], offset[16], stride[16], indices;
    uint32_t viewport[6];
    float vs_float[256][4], ps_float[224][4];
    int32_t vs_int[16][4], ps_int[16][4];
    uint32_t vs_bool[16], ps_bool[16];
    float world[16], view[16], proj[16], texture_transform[8][16];
} HostD3DDrawState;
static HostD3DDrawState draw_state;
/* Custom block snapshots contain only device state, never panorama/capture
 * targets or surface pixels. Recording setters select logical byte ranges. */
typedef struct {
    HostD3DDrawState draw;
    HostD3DMaterial material;
    HostD3DLightSlot lights[HOST_D3D_MAX_LIGHTS];
} HostD3DStateSnapshot;
static HostStateBlock *stateblock_recording;
static size_t stateblock_resource_offsets[36];
static size_t stateblock_resource_count;
static D3DObj *stateblock_find_resource(uint32_t guest){return obj_find(guest);}
static void stateblock_retain(void *context,uint32_t guest){(void)context;D3DObj *o=stateblock_find_resource(guest);obj_retain(o);}
static void stateblock_release(void *context,uint32_t guest){(void)context;D3DObj *o=stateblock_find_resource(guest);obj_release(o);}
static int stateblock_valid_binding(uint32_t guest,int kind){if(!guest)return 1;D3DObj *o=stateblock_find_resource(guest);return o&&(o->kind==kind||(kind==K_TEXTURE&&(o->kind==K_CUBETEX||o->kind==K_VOLTEX)));}
static void stateblock_snapshot(HostD3DStateSnapshot *s){memset(s,0,sizeof *s);s->draw=draw_state;s->material=current_material;memcpy(s->lights,light_slots,sizeof light_slots);}
static void binding_replace(uint32_t *binding,uint32_t next){
    uint32_t old=*binding;if(old==next)return;
    stateblock_retain(NULL,next);*binding=next;stateblock_release(NULL,old);
}
static void draw_state_restore(const HostD3DDrawState *next){
    /* Retain every new owner before releasing any old owner; a resource may move
     * between stream slots and the old binding may be its last reference. */
    for(unsigned i=0;i<16;i++){stateblock_retain(NULL,next->stream[i]);stateblock_retain(NULL,next->texture[i]);}
    stateblock_retain(NULL,next->indices);stateblock_retain(NULL,next->decl);stateblock_retain(NULL,next->vs);stateblock_retain(NULL,next->ps);
    for(unsigned i=0;i<16;i++){stateblock_release(NULL,draw_state.stream[i]);stateblock_release(NULL,draw_state.texture[i]);}
    stateblock_release(NULL,draw_state.indices);stateblock_release(NULL,draw_state.decl);stateblock_release(NULL,draw_state.vs);stateblock_release(NULL,draw_state.ps);
    draw_state=*next;
}
static void stateblock_restore(const HostD3DStateSnapshot *s){draw_state_restore(&s->draw);current_material=s->material;memcpy(light_slots,s->lights,sizeof light_slots);}
static HostStateBlock *stateblock_begin_payload(void){
    if(!stateblock_resource_count){
        for(size_t i=0;i<16;i++)stateblock_resource_offsets[stateblock_resource_count++]=offsetof(HostD3DStateSnapshot,draw.texture)+4*i;
        for(size_t i=0;i<16;i++)stateblock_resource_offsets[stateblock_resource_count++]=offsetof(HostD3DStateSnapshot,draw.stream)+4*i;
        const size_t fields[]={offsetof(HostD3DStateSnapshot,draw.vs),offsetof(HostD3DStateSnapshot,draw.ps),offsetof(HostD3DStateSnapshot,draw.decl),offsetof(HostD3DStateSnapshot,draw.indices)};
        for(size_t i=0;i<4;i++)stateblock_resource_offsets[stateblock_resource_count++]=fields[i];
    }
    HostD3DStateSnapshot initial;stateblock_snapshot(&initial);
    return host_sb_new(&initial,sizeof initial,stateblock_resource_offsets,stateblock_resource_count,stateblock_retain,stateblock_release,NULL);
}
static void stateblock_capture_payload(HostStateBlock *b){HostD3DStateSnapshot s;stateblock_snapshot(&s);host_sb_capture(b,&s);}
static void stateblock_apply_payload(HostStateBlock *b){HostD3DStateSnapshot s;stateblock_snapshot(&s);host_sb_apply(b,&s);stateblock_restore(&s);}
static void d3d9_set_vertex_input(int fvf,uint32_t value){
    uint32_t declaration=fvf?0:value,format=fvf?value:0;
    if(stateblock_recording){host_sb_write(stateblock_recording,offsetof(HostD3DStateSnapshot,draw.decl),&declaration,4);host_sb_write(stateblock_recording,offsetof(HostD3DStateSnapshot,draw.fvf),&format,4);}
    else{binding_replace(&draw_state.decl,declaration);draw_state.fvf=format;}
}
#define SB_DRAW_OFFSET(field) offsetof(HostD3DStateSnapshot,draw.field)
#define SB_WRITE(field,value,size) host_sb_write(stateblock_recording,SB_DRAW_OFFSET(field),(value),(size))
/* Getters and draws always read live state, even during recording. */
static int stateblock_record_method(EngineCPU *cpu,int i,uint32_t *hr){
    if(!stateblock_recording)return 0;
    HostD3DStateSnapshot *shadow=(HostD3DStateSnapshot *)stateblock_recording->values;
    uint32_t value=ARG(2),index=ARG(1);*hr=D3D_OK;
    switch(i){
    case 44: {
        size_t offset=index==2?SB_DRAW_OFFSET(view):index==3?SB_DRAW_OFFSET(proj):index==256?SB_DRAW_OFFSET(world):index>=16&&index<24?SB_DRAW_OFFSET(texture_transform)+64*(index-16):SIZE_MAX;
        if(!value||offset==SIZE_MAX){*hr=D3DERR_INVALIDCALL;break;}
        host_sb_write(stateblock_recording,offset,GPTR(value),64);break;}
    case 47: if(!index){*hr=D3DERR_INVALIDCALL;break;}SB_WRITE(viewport,GPTR(index),24);break;
    case 49: {
        if(!index){*hr=D3DERR_INVALIDCALL;break;}HostD3DMaterial m;memcpy(&m,GPTR(index),sizeof m);
        if(!finite_floats(&m.diffuse[0],17)||m.power<0.f){*hr=D3DERR_INVALIDCALL;break;}
        host_sb_write(stateblock_recording,offsetof(HostD3DStateSnapshot,material),&m,sizeof m);break;}
    case 51: {
        if(index>=HOST_D3D_MAX_LIGHTS||!value){*hr=D3DERR_INVALIDCALL;break;}HostD3DLight l;memcpy(&l,GPTR(value),sizeof l);
        if(!valid_light(&l)){*hr=D3DERR_INVALIDCALL;break;}
        size_t offset=offsetof(HostD3DStateSnapshot,lights)+index*sizeof(HostD3DLightSlot);uint8_t defined=1;
        host_sb_write(stateblock_recording,offset+offsetof(HostD3DLightSlot,light),&l,sizeof l);
        host_sb_write(stateblock_recording,offset+offsetof(HostD3DLightSlot,defined),&defined,1);break;}
    case 53: {
        if(index>=HOST_D3D_MAX_LIGHTS){*hr=D3DERR_INVALIDCALL;break;}uint8_t enable=value!=0;
        if(enable&&!shadow->lights[index].enabled){unsigned active=0;for(unsigned k=0;k<HOST_D3D_MAX_LIGHTS;k++)active+=shadow->lights[k].enabled!=0;if(active>=HOST_D3D_MAX_ACTIVE_LIGHTS){*hr=D3DERR_INVALIDCALL;break;}}
        size_t offset=offsetof(HostD3DStateSnapshot,lights)+index*sizeof(HostD3DLightSlot);
        if(!shadow->lights[index].defined){HostD3DLight l;default_light(&l);uint8_t defined=1;host_sb_write(stateblock_recording,offset,&l,sizeof l);host_sb_write(stateblock_recording,offset+offsetof(HostD3DLightSlot,defined),&defined,1);}
        host_sb_write(stateblock_recording,offset+offsetof(HostD3DLightSlot,enabled),&enable,1);break;}
    case 57: if(index>=256){*hr=D3DERR_INVALIDCALL;break;}SB_WRITE(rs[index],&value,4);break;
    case 65: if(index>=16||!stateblock_valid_binding(value,K_TEXTURE)){*hr=D3DERR_INVALIDCALL;break;}SB_WRITE(texture[index],&value,4);break;
    case 67: if(index>=8||value>=33){*hr=D3DERR_INVALIDCALL;break;}value=ARG(3);SB_WRITE(ts[index][ARG(2)],&value,4);break;
    case 69: if(index>=16||value>=14){*hr=D3DERR_INVALIDCALL;break;}value=ARG(3);SB_WRITE(sampler[index][ARG(2)],&value,4);break;
    case 87: if(!stateblock_valid_binding(index,K_VDECL)){*hr=D3DERR_INVALIDCALL;break;}d3d9_set_vertex_input(0,index);break;
    case 89: d3d9_set_vertex_input(1,index);break;
    case 92: if(!stateblock_valid_binding(index,K_VSHADER)){*hr=D3DERR_INVALIDCALL;break;}SB_WRITE(vs,&index,4);break;
    case 107: if(!stateblock_valid_binding(index,K_PSHADER)){*hr=D3DERR_INVALIDCALL;break;}SB_WRITE(ps,&index,4);break;
    case 94: case 96: case 98: case 109: case 111: case 113: {
        int pixel=i>=109,kind=(i-(pixel?109:94))/2;uint32_t max=kind?16u:pixel?224u:256u,stride=kind==2?4u:16u,count=ARG(3);
        if(index>max||count>max-index||(!value&&count)){*hr=D3DERR_INVALIDCALL;break;}
        size_t offset=kind==0?(pixel?SB_DRAW_OFFSET(ps_float):SB_DRAW_OFFSET(vs_float)):kind==1?(pixel?SB_DRAW_OFFSET(ps_int):SB_DRAW_OFFSET(vs_int)):(pixel?SB_DRAW_OFFSET(ps_bool):SB_DRAW_OFFSET(vs_bool));
        host_sb_write(stateblock_recording,offset+index*stride,count?GPTR(value):NULL,count*stride);break;}
    case 100: if(index>=16||!stateblock_valid_binding(value,K_VB)){*hr=D3DERR_INVALIDCALL;break;}SB_WRITE(stream[index],&value,4);value=ARG(3);SB_WRITE(offset[index],&value,4);value=ARG(4);SB_WRITE(stride[index],&value,4);break;
    case 104: if(!stateblock_valid_binding(index,K_IB)){*hr=D3DERR_INVALIDCALL;break;}SB_WRITE(indices,&index,4);break;
    case 46: case 55: case 71: case 75: case 77: case 79: case 102: {
        static uint8_t announced[119];if(!announced[i]){announced[i]=1;host_log("[stateblock] legacy setter %s is not stored in custom masks",classes[K_DEVICE].methods[i].name);}
        return 0; /* Preserve these existing no-op paths, without claiming support. */
    }
    default:return 0;
    }
    return 1;
}
#undef SB_WRITE
#undef SB_DRAW_OFFSET
static void ff3d_identity(float *m){ memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1.0f; }

static int capture_panorama_pass(void);
#include "d3d9_capture.inc"
#include "d3d9_process_vertices.inc"
#include "rhw_declaration.h"

static void render_defaults(uint32_t w, uint32_t h) {
    HostD3DDrawState empty={0};draw_state_restore(&empty);
    draw_state.rs[7]=1; draw_state.rs[14]=1; draw_state.rs[19]=2;
    draw_state.rs[20]=1; draw_state.rs[22]=3; draw_state.rs[23]=4;
    draw_state.rs[25]=8;draw_state.rs[53]=draw_state.rs[54]=draw_state.rs[55]=1;draw_state.rs[56]=8;
    draw_state.rs[58]=draw_state.rs[59]=0xFFFFFFFFu;draw_state.rs[60]=0xFFFFFFFFu; draw_state.rs[168]=15; draw_state.rs[171]=1;
    for (int s=0;s<8;s++) {
        draw_state.ts[s][1]=s?1:4; draw_state.ts[s][2]=2; draw_state.ts[s][3]=1;
        draw_state.ts[s][4]=s?1:2; draw_state.ts[s][5]=2; draw_state.ts[s][6]=1;
        draw_state.ts[s][26]=draw_state.ts[s][27]=draw_state.ts[s][28]=1;draw_state.ts[s][11]=(uint32_t)s;
    }
    for(int s=0;s<16;s++) {
        draw_state.sampler[s][1]=draw_state.sampler[s][2]=draw_state.sampler[s][3]=1;
        draw_state.sampler[s][5]=draw_state.sampler[s][6]=1;
    }
    draw_state.viewport[2]=w; draw_state.viewport[3]=h;
    draw_state.viewport[5]=0x3F800000;
    ff3d_identity(draw_state.world); ff3d_identity(draw_state.view); ff3d_identity(draw_state.proj);
    for(int s=0;s<8;s++)ff3d_identity(draw_state.texture_transform[s]);
}
/* Bytes of a surface (and the texture level it belongs to) changed. */
static void content_changed(D3DObj *s) {
    if(!s)return; s->content_generation++;
    D3DObj *owner=obj_from_guest(s->owner); if(owner&&owner!=s)owner->content_generation++;
}
/* Surfaces that own a GPU target; only these can be gpu_dirty. */
static D3DObj *render_surfaces[65536]; static int render_surface_count;
static void surface_destroy_renderer(D3DObj *surface){
    for(int i=0;i<render_surface_count;){
        if(render_surfaces[i]==surface)render_surfaces[i]=render_surfaces[--render_surface_count];else i++;
    }
    if(surface->renderer){mr_destroy(surface->renderer);surface->renderer=NULL;}
}
static uint64_t draw_prof_vertex_ns, draw_prof_submit_ns, draw_prof_draws;
static uint64_t draw_prof_pass_ns, draw_prof_passes, draw_prof_readback_ns;
/* Engine-thread-only lifetime total: HALO_DRAW_PROFILE resets its interval
 * counters for the log, but Present-to-Present telemetry must not lose a pass. */
static uint64_t draw_prof_pass_total_ns;
/* Every synchronous GPU readback the engine's thread waits on. On the Mac
 * the GPU is otherwise idle and each one is cheap; on the headset it waits
 * behind a compositor that now runs at 90 Hz, so the same count can cost far
 * more there. Counted here so the device report can say how many. */
static uint64_t draw_prof_readback_calls;
/* Guest-copy uploads to the GPU (surface_prepare), the other direction. */
static uint64_t draw_prof_upload_ns;
static int surface_flush(D3DObj *s) {
    if (!s || !s->renderer || !s->gpu_dirty) return 0;
    draw_prof_readback_calls++;
    uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    int failed = mr_read_framebuffer(s->renderer,GPTR(s->data),s->size);
    draw_prof_readback_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
    if(failed) return -1;
    s->gpu_dirty=0; content_changed(s); return 0;
}
static int surface_prepare(D3DObj *s) {
    if(!s || (s->format!=21 && s->format!=22) || !s->data) return -1;
    if(!s->renderer){ s->renderer=mr_create((int)s->width,(int)s->height);
        if(s->renderer){
            /* The panorama HUD object persists across resizes. Do not append
             * duplicate pointers when its renderer is replaced. */
            int found=0;for(int i=0;i<render_surface_count;i++)if(render_surfaces[i]==s){found=1;break;}
            if(!found&&render_surface_count<65536)render_surfaces[render_surface_count++]=s;
        } }
    if(!s->renderer) return -1;
    if(!s->gpu_dirty){
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        int failed = mr_write_framebuffer(s->renderer,GPTR(s->data),s->size);
        draw_prof_upload_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start;
        if(failed) return -1;
    }
    return 0;
}
#include "panorama_render.inc"
static int capture_panorama_pass(void){return panorama_pass;}

static int flush_data(uint32_t data) {
    for(int i=0;i<render_surface_count;i++) if(render_surfaces[i]->data==data && surface_flush(render_surfaces[i])) return -1;
    return 0;
}
/* Numeric reject diagnostics are separate from render_trace_reason: enabling
 * these logs must not change fixed3D fallback or draw acceptance. */
typedef struct {uint32_t caller,primitive,vertices,stride,count,indices,index_count,index_format;int32_t base;uint64_t serial;} DrawRejectContext;
typedef struct {char reason[64];uint32_t caller,primitive,stride,fvf,vs,ps,decl,state[16];int pass;uint64_t declaration_hash;} DrawRejectKey;
static struct {
    int initialized,enabled;uint32_t from,to;unsigned details,declarations;
    uint32_t declaration_ids[8];DrawRejectKey keys[32];DrawRejectContext draw;
} draw_reject_trace;
static int draw_reject_enabled(void){
    if(!draw_reject_trace.initialized){
        draw_reject_trace.initialized=1;draw_reject_trace.to=UINT32_MAX;
        const char *flag=HOST_ENV("HALO_DRAW_REJECT_TRACE"),*from=HOST_ENV("HALO_DRAW_REJECT_FROM"),*to=HOST_ENV("HALO_DRAW_REJECT_TO");
        draw_reject_trace.enabled=flag&&!strcmp(flag,"1");
        if((from&&!render_diag_number(from,&draw_reject_trace.from))||(to&&!render_diag_number(to,&draw_reject_trace.to))||draw_reject_trace.from>draw_reject_trace.to)draw_reject_trace.enabled=0;
    }
    return draw_reject_trace.enabled&&frames_presented+1>=draw_reject_trace.from&&frames_presented+1<=draw_reject_trace.to;
}
static void draw_reject_diagnostic(const char *reason){
    if(!draw_reject_enabled()||draw_reject_trace.details>=32)return;
    DrawRejectKey key={0};snprintf(key.reason,sizeof key.reason,"%s",reason);
    key.caller=draw_reject_trace.draw.caller;key.primitive=draw_reject_trace.draw.primitive;key.stride=draw_reject_trace.draw.stride;
    key.fvf=draw_state.fvf;key.vs=draw_state.vs;key.ps=draw_state.ps;key.decl=draw_state.decl;key.pass=panorama_pass;
    const unsigned states[]={27,19,20,171,206,207,208,209,193,15,25,24,168,7,14,23};
    for(unsigned n=0;n<16;n++)key.state[n]=draw_state.rs[states[n]];
    D3DObj *decl=stateblock_find_resource(draw_state.decl);
    const uint8_t *elements=decl&&decl->kind==K_VDECL&&decl->data?(const uint8_t *)GPTR(decl->data):NULL;
    size_t bytes=elements?(decl->size<512?decl->size:512):0;
    key.declaration_hash=UINT64_C(14695981039346656037);
    for(size_t n=0;n<bytes;n++){key.declaration_hash^=elements[n];key.declaration_hash*=UINT64_C(1099511628211);}
    for(unsigned n=0;n<draw_reject_trace.details;n++)if(!memcmp(&key,&draw_reject_trace.keys[n],sizeof key))return;
    unsigned detail=draw_reject_trace.details++;draw_reject_trace.keys[detail]=key;
    const DrawRejectContext *d=&draw_reject_trace.draw;
    host_log("[draw-reject] detail=%u frame=%u draw=%llu reason=%s caller=%08x pass=%d prim=%u vertices=%08x stride=%u nv=%u indices=%08x ni=%u ifmt=%u base=%d fvf=%08x vs=%08x ps=%08x decl=%08x declBytes=%u declHash=%016llx blendEn=%u src=%u dst=%u op=%u separate=%u srcA=%u dstA=%u opA=%u factor=%08x atest=%u acmp=%u aref=%u cw=%x z=%u zw=%u zcmp=%u stream0=%08x offset0=%u stride0=%u",
        detail,frames_presented+1,(unsigned long long)d->serial,reason,d->caller,panorama_pass,d->primitive,d->vertices,d->stride,d->count,d->indices,d->index_count,d->index_format,d->base,
        draw_state.fvf,draw_state.vs,draw_state.ps,draw_state.decl,decl?decl->size:0,(unsigned long long)key.declaration_hash,
        draw_state.rs[27],draw_state.rs[19],draw_state.rs[20],draw_state.rs[171],draw_state.rs[206],draw_state.rs[207],draw_state.rs[208],draw_state.rs[209],draw_state.rs[193],draw_state.rs[15],draw_state.rs[25],draw_state.rs[24],draw_state.rs[168],draw_state.rs[7],draw_state.rs[14],draw_state.rs[23],draw_state.stream[0],draw_state.offset[0],draw_state.stride[0]);
    if(!elements||draw_reject_trace.declarations>=8)return;
    for(unsigned n=0;n<draw_reject_trace.declarations;n++)if(draw_reject_trace.declaration_ids[n]==draw_state.decl)return;
    draw_reject_trace.declaration_ids[draw_reject_trace.declarations++]=draw_state.decl;
    unsigned rows=0;for(size_t offset=0;offset+8<=bytes&&rows<16;offset+=8,rows++){
        const uint8_t *e=elements+offset;unsigned stream=e[0]|(unsigned)e[1]<<8,vertex_offset=e[2]|(unsigned)e[3]<<8;
        host_log("[draw-reject-decl] decl=%08x element=%u stream=%u offset=%u type=%u method=%u usage=%u usageIndex=%u",draw_state.decl,rows,stream,vertex_offset,e[4],e[5],e[6],e[7]);
        if(stream==255)break;
    }
}
/* D3DPT: 1 point list, 2 line list, 3 line strip, 4 triangle list, 5 strip,
 * 6 fan. The fan is expanded into a list before it reaches the renderer, so
 * it maps to a list here. Lines and points only started arriving once the
 * front end began rendering through the panorama path. */
static int halo_mr_primitive(uint32_t prim) {
    switch (prim) {
        case 1: return MR_POINT_LIST;
        case 2: return MR_LINE_LIST;
        case 3: return MR_LINE_STRIP;
        case 5: return MR_TRIANGLE_STRIP;
        default: return MR_TRIANGLE_LIST;
    }
}
static uint32_t unsupported_draw(const char *reason) {
    draw_reject_diagnostic(reason);
    render_trace_failure(reason);
    static unsigned reports;
    if(reports++<24) host_log("d3d9 draw unsupported: %s; FVF=%08X VS=%08X PS=%08X decl=%08X scissor=%u twoSidedStencil=%u pass=%d",reason,draw_state.fvf,draw_state.vs,draw_state.ps,draw_state.decl,draw_state.rs[174],draw_state.rs[185],panorama_pass);
    if(HOST_ENV("HALO_DROPTALLY")){ /* per-reason drop tally, dumped once per ~present via frames_presented change */
        static const char*names[16]; static unsigned cnt[16]; static int n=0; static uint32_t lastf=0; static unsigned okhint=0;
        int idx=-1; for(int i=0;i<n;i++) if(names[i]==reason){idx=i;break;}
        if(idx<0 && n<16){ idx=n++; names[idx]=reason; cnt[idx]=0; }
        if(idx>=0) cnt[idx]++;
        if(frames_presented!=lastf){ if(lastf && (frames_presented%60==0)){ char buf[512]; int p=0; for(int i=0;i<n;i++){ p+=snprintf(buf+p,sizeof buf-p,"%s=%u ",names[i],cnt[i]); } host_log("[droptally] f=%u %s", frames_presented, buf); } lastf=frames_presented; for(int i=0;i<n;i++) cnt[i]=0; }
    }
    return D3DERR_NOTAVAILABLE;
}
static int rect_read(uint32_t guest,D3DObj *s,int r[4]) {
    if(!s) return 0;
    if(guest) for(int i=0;i<4;i++) r[i]=(int32_t)G32(guest+4*i);
    else {r[0]=r[1]=0;r[2]=(int)s->width;r[3]=(int)s->height;}
    return r[0]>=0 && r[1]>=0 && r[2]>r[0] && r[3]>r[1] && (uint32_t)r[2]<=s->width && (uint32_t)r[3]<=s->height;
}
static uint32_t surface_copy(D3DObj *src,uint32_t sr,D3DObj *dst,uint32_t dr,uint32_t filter) {
    int a[4],b[4];
    if(!rect_read(sr,src,a)||!rect_read(dr,dst,b)||!src->data||!dst->data) return D3DERR_INVALIDCALL;
    if((src->format!=21&&src->format!=22)||(dst->format!=21&&dst->format!=22)||filter>1) return D3DERR_NOTAVAILABLE;
    if(surface_flush(src)||surface_flush(dst)) return D3DERR_NOTAVAILABLE;
    int aw=a[2]-a[0],ah=a[3]-a[1],bw=b[2]-b[0],bh=b[3]-b[1];
    uint32_t *copy=malloc((size_t)aw*ah*4); if(!copy) return 0x8007000Eu;
    for(int y=0;y<ah;y++) memcpy(copy+(size_t)y*aw,GPTR(src->data+4*((a[1]+y)*src->width+a[0])),(size_t)aw*4);
    for(int y=0;y<bh;y++) for(int x=0;x<bw;x++) {
        uint32_t v=copy[(size_t)(y*ah/bh)*aw+x*aw/bw];
        if(src->format==22 || dst->format==22) v|=0xFF000000;
        S32(dst->data+4*((b[1]+y)*dst->width+b[0]+x),v);
    }
    free(copy); dst->gpu_dirty=0; content_changed(dst); return D3D_OK;
}
#include "texture_content_hash.h"
/* Map D3D alpha-blend render state to the renderer. Known factor pairs keep
 * their dedicated enum; every other legal pair or non-ADD operation is passed
 * through as raw D3DBLEND/D3DBLENDOP values. Returns 0 for illegal state. */
static int map_blend_state(int *blend,int *src,int *dst,int *op){
    *blend=MR_BLEND_NONE;*src=(int)draw_state.rs[19];*dst=(int)draw_state.rs[20];*op=(int)draw_state.rs[171];
    if(!draw_state.rs[27])return 1;
    uint32_t sb=draw_state.rs[19],db=draw_state.rs[20];
    if(*op!=1)*blend=MR_BLEND_CUSTOM;
    else if(sb==2&&db==1)*blend=MR_BLEND_NONE;
    else if(sb==5&&db==6)*blend=MR_BLEND_SRC_ALPHA;
    else if(sb==2&&db==2)*blend=MR_BLEND_ADD;
    else if((sb==9&&db==1)||(sb==1&&db==3))*blend=MR_BLEND_MODULATE;
    else if(sb==9&&db==3)*blend=MR_BLEND_MODULATE2;
    else if(sb==5&&db==1)*blend=MR_BLEND_SRC_ALPHA_ZERO;
    else if(sb==5&&db==2)*blend=MR_BLEND_SRC_ALPHA_ADD;
    else if(sb==2&&db==6)*blend=MR_BLEND_PREMULTIPLIED;
    else if(sb==7&&db==2)*blend=MR_BLEND_DEST_ALPHA_ADD;
    else *blend=MR_BLEND_CUSTOM;
    if(*blend==MR_BLEND_CUSTOM&&(*op<1||*op>5||sb<1||sb>15||db<1||db>15||sb==12||sb==13||db==12||db==13))return 0;
    return 1;
}
static uint64_t texture_hash_more(uint64_t hash,const void *bytes,size_t size) {
    return halo_texture_hash_more(hash,bytes,size);
}
#include "texture_mod_runtime.inc"
static uint32_t uploaded_texture(mr_context *renderer,D3DObj *t) {
    if(!renderer||!t||t->kind!=K_TEXTURE||!t->level_data[0])return 0;
    /* A render-target texture whose latest contents live on the GPU is sampled
     * directly through the shared texture table; no readback/re-upload. */
    if(t->usage&1u){D3DObj *level=obj_from_guest(t->level_surface[0]);
        if(level&&level->renderer&&level->gpu_dirty&&level->data==t->level_data[0]){uint32_t id=mr_target_texture(level->renderer);if(id)return id;}}
    if(flush_data(t->level_data[0]))return 0;
    size_t bytes=halo_texture_bgra_size(t->width,t->height);if(!bytes)return 0;
    uint32_t pitch,size=level_size(t->format,t->width,t->height,&pitch);if(!size)return 0;
    uint32_t mod=uploaded_texture_mod(renderer,t,size);if(mod)return mod;
    uint64_t key;
    if(t->content_key&&t->hashed_generation==t->content_generation)key=t->content_key;
    else{key=UINT64_C(1469598103934665603);
        key=texture_hash_more(key,&t->guest,sizeof t->guest);key=texture_hash_more(key,&t->format,sizeof t->format);
        key=texture_hash_more(key,&t->width,sizeof t->width);key=texture_hash_more(key,&t->height,sizeof t->height);
        key=texture_hash_more(key,GPTR(t->level_data[0]),size);if(!key)key=1;
        t->content_key=key;t->hashed_generation=t->content_generation;}
    uint32_t id=mr_texture_find_cached(renderer,key);if(id)return id;
    uint8_t *pixels=malloc(bytes);if(!pixels)return 0;
    if(!halo_texture_decode(t->format,t->width,t->height,GPTR(t->level_data[0]),size,pitch,pixels,bytes)){
        /* Render targets (D3DUSAGE_RENDERTARGET) are re-sampled at 1:1 every frame; skip CPU mips. */
        id=(t->usage&1u)?mr_texture_create_cached_nomip(renderer,key,(int)t->width,(int)t->height,pixels,(size_t)t->width*4)
                        :mr_texture_create_cached(renderer,key,(int)t->width,(int)t->height,pixels,(size_t)t->width*4);
        if(id&&HOST_ENV("HALO_FF3D_DUMPTEX")){static uint32_t seen[40];static int ns;int dup=0;for(int q=0;q<ns;q++)if(seen[q]==t->guest)dup=1;
            if(!dup&&ns<40){seen[ns++]=t->guest;char pth[300];snprintf(pth,sizeof pth,"%s/tex_%08x_%ux%u.bgra",HOST_ENV("HALO_FF3D_DUMPTEX"),t->guest,t->width,t->height);
                FILE *fp=fopen(pth,"wb");if(fp){fwrite(pixels,1,bytes,fp);fclose(fp);}}}
    }
    free(pixels);return id;
}
static uint32_t uploaded_cube_texture(mr_context *renderer,D3DObj *t){
    if(!renderer||!t||t->kind!=K_CUBETEX||!t->width||t->width!=t->height)return 0;
    size_t face_bytes=halo_texture_bgra_size(t->width,t->height);if(!face_bytes||face_bytes>SIZE_MAX/6)return 0;
    uint32_t pitch,source_size=level_size(t->format,t->width,t->height,&pitch);if(!source_size)return 0;
    for(int face=0;face<6;face++){uint32_t data=t->face_data[face][0];if(!data||flush_data(data))return 0;}
    uint64_t key;
    if(t->content_key&&t->hashed_generation==t->content_generation)key=t->content_key;
    else{key=UINT64_C(1469598103934665603);key=texture_hash_more(key,&t->guest,sizeof t->guest);key=texture_hash_more(key,&t->format,sizeof t->format);key=texture_hash_more(key,&t->width,sizeof t->width);
        for(int face=0;face<6;face++)key=texture_hash_more(key,GPTR(t->face_data[face][0]),source_size);if(!key)key=1;
        t->content_key=key;t->hashed_generation=t->content_generation;}
    uint32_t id=mr_texture_find_cached(renderer,key);if(id)return id;
    uint8_t *pixels=malloc(face_bytes*6);if(!pixels)return 0;const void *faces[6];
    for(int face=0;face<6;face++){faces[face]=pixels+face_bytes*face;if(halo_texture_decode(t->format,t->width,t->height,GPTR(t->face_data[face][0]),source_size,pitch,pixels+face_bytes*face,face_bytes)){free(pixels);return 0;}}
    id=mr_texture_create_cube_cached(renderer,key,(int)t->width,faces,(size_t)t->width*4);free(pixels);return id;
}
static uint32_t uploaded_volume_texture(mr_context *renderer,D3DObj *t){
    if(!renderer||!t||t->kind!=K_VOLTEX||!t->level_data[0]||!t->width||!t->height||flush_data(t->level_data[0]))return 0;
    uint32_t depth=t->depth?t->depth:1;if(depth>512)return 0;
    size_t slice_bytes=halo_texture_bgra_size(t->width,t->height);if(!slice_bytes||slice_bytes>SIZE_MAX/depth)return 0;
    uint32_t pitch,slice_size=level_size(t->format,t->width,t->height,&pitch);if(!slice_size)return 0;
    if((uint64_t)t->level_data[0]+(uint64_t)slice_size*depth>0x100000000ULL)return 0;
    uint64_t key;
    if(t->content_key&&t->hashed_generation==t->content_generation)key=t->content_key;
    else{key=UINT64_C(1469598103934665603);key=texture_hash_more(key,&t->guest,sizeof t->guest);key=texture_hash_more(key,&t->format,sizeof t->format);
        key=texture_hash_more(key,&t->width,sizeof t->width);key=texture_hash_more(key,&t->height,sizeof t->height);key=texture_hash_more(key,&depth,sizeof depth);
        key=texture_hash_more(key,GPTR(t->level_data[0]),(size_t)slice_size*depth);if(!key)key=1;
        t->content_key=key;t->hashed_generation=t->content_generation;}
    uint32_t id=mr_texture_find_cached(renderer,key);if(id)return id;
    uint8_t *pixels=malloc(slice_bytes*depth);if(!pixels)return 0;
    for(uint32_t z=0;z<depth;z++)if(halo_texture_decode(t->format,t->width,t->height,GPTR(t->level_data[0]+slice_size*z),slice_size,pitch,pixels+slice_bytes*z,slice_bytes)){free(pixels);return 0;}
    id=mr_texture_create_volume_cached(renderer,key,(int)t->width,(int)t->height,(int)depth,pixels,(size_t)t->width*4,slice_bytes);
    free(pixels);return id;
}
/* Scratch buffers for one draw.
 *
 * Every draw used to malloc a vertex array and an index array and free them
 * again, thousands of times a frame, for sizes that barely change. The
 * allocator work and the page faults on fresh memory are pure overhead in
 * the hottest path in the port. These grow to the largest draw seen and are
 * then reused. Single-threaded by construction: only the engine thread
 * submits draws, and a draw is finished before the next begins. */
static void *draw_scratch_grow(void **buffer, size_t *capacity, size_t needed) {
    if (*capacity >= needed) return *buffer;
    size_t want = needed + needed / 2 + 4096;
    void *grown = realloc(*buffer, want);
    if (!grown) return NULL;
    *buffer = grown; *capacity = want; return grown;
}
/* Where a frame's time actually goes.
 *
 * Reported every 120 frames next to the frame timing, so the split between
 * reformatting vertices on the CPU and handing work to the GPU is a
 * measurement rather than an argument. HALO_DRAW_PROFILE=1 turns it on. */

static inline uint64_t draw_prof_now(void) {
    return HOST_ENV("HALO_DRAW_PROFILE") ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
}
/* Always on. The per-pass timer is two clock reads a pass, nine a frame at
 * most, and it is the only measurement that can say whether the headset's
 * engine thread is spending its frame in the scene traversal or waiting on
 * something else. The per-draw timers below stay behind HALO_DRAW_PROFILE. */
int host_pass_profile_enabled(void){ return 1; }
void host_pass_profile_add(uint64_t nanoseconds){ draw_prof_pass_ns += nanoseconds; draw_prof_pass_total_ns += nanoseconds; ++draw_prof_passes; }
uint64_t host_pass_profile_total_ns(void){ return draw_prof_pass_total_ns; }
void host_draw_profile_report(unsigned frame, double seconds) {
    if (!HOST_ENV("HALO_DRAW_PROFILE")) return;
    host_log("[draw-profile] frame=%u wall=%.3fs passes=%llu engine=%.3fs readback=%.3fs draws=%llu vertex=%.3fs submit=%.3fs other=%.3fs",
             frame, seconds, (unsigned long long)draw_prof_passes,
             draw_prof_pass_ns / 1e9, draw_prof_readback_ns / 1e9,
             (unsigned long long)draw_prof_draws,
             draw_prof_vertex_ns / 1e9, draw_prof_submit_ns / 1e9,
             seconds - (draw_prof_pass_ns + draw_prof_readback_ns) / 1e9);
    draw_prof_vertex_ns = draw_prof_submit_ns = draw_prof_draws = 0;
    draw_prof_pass_ns = draw_prof_passes = draw_prof_readback_ns = 0;
}
/* The same counters without the reset or the log line, for the device
 * report. They accumulate for the whole run there; two reports a known
 * time apart give the rate. Order: passes, pass ns, readback ns, readback
 * calls, draws, vertex ns, submit ns, spare. */
void host_draw_profile_snapshot(uint64_t out[12]) {
    out[0] = draw_prof_passes; out[1] = draw_prof_pass_ns; out[2] = draw_prof_readback_ns;
    out[3] = draw_prof_readback_calls; out[4] = draw_prof_draws;
    out[5] = draw_prof_vertex_ns; out[6] = draw_prof_submit_ns; out[7] = draw_prof_upload_ns;
    out[8] = host_sleep_calls; out[9] = host_sleep_ns; out[10] = host_yield_calls; out[11] = host_yield_spin_ns;
}
static void *draw_index_scratch; static size_t draw_index_capacity;
static void *draw_vertex_scratch; static size_t draw_vertex_capacity;

static uint16_t *draw_indices(uint32_t prim,uint32_t nv,uint32_t source,uint32_t count,uint32_t format,int32_t base,uint32_t *out_count) {
    unsigned is=format==101?2:format==102?4:0;uint32_t n=source?count:nv;
    *out_count=prim==6?(n>=3?(n-2)*3:0):source?count:0;
    if(!*out_count)return NULL;
    if(*out_count>196608||n>196608||(source&&(!is||(uint64_t)source+(uint64_t)n*is>0x100000000ULL)))return NULL;
    uint16_t *out=draw_scratch_grow(&draw_index_scratch,&draw_index_capacity,(size_t)*out_count*2);
    if(!out)return NULL;
    if(source&&is==2&&prim!=6){
        /* The common case, 16-bit indices for a list or strip: one pass for
         * the range of the whole draw, which vectorises, then the rebased
         * copy. Accepts and writes exactly what the general loop below does;
         * it only stops writing the scratch early on a rejected draw. */
        const uint8_t *in=GPTR(source);uint32_t lo=65535,hi=0;
        for(uint32_t i=0;i<n;i++){uint16_t v;memcpy(&v,in+2*(size_t)i,2);lo=v<lo?v:lo;hi=v>hi?v:hi;}
        int64_t first=(int64_t)lo+base,last=(int64_t)hi+base;
        if(first<0||last>=(int64_t)nv||last>65535)return NULL;
        for(uint32_t i=0;i<n;i++){uint16_t v;memcpy(&v,in+2*(size_t)i,2);out[i]=(uint16_t)(v+base);}
        return out;
    }
    for(uint32_t i=0;i<*out_count;i++){
        uint32_t k=prim==6?(i%3==0?0:i/3+(i%3)):i;
        int64_t v=source?(int64_t)(is==2?G16(source+k*2):G32(source+k*4))+base:k;
        if(v<0||v>=nv||v>65535)return NULL;   /* scratch, not ours to free */
        out[i]=(uint16_t)v;
    }
    return out;
}
/* Carry D3D sampler intent through to Metal, including minification and mip
 * selection. The old bridge reduced all three filters to a MAG-only boolean. */
static void program_sampler_settings(mr_program_sampler *m,int stage) {
    const uint32_t *state=draw_state.sampler[stage];
    m->address_u=(uint8_t)state[1];m->address_v=(uint8_t)state[2];
    m->border_color=state[4];
    m->linear_filter=state[5]>1;
    m->mag_filter=state[5];m->min_filter=state[6];m->mip_filter=state[7];
    m->max_mip_level=state[9];m->max_anisotropy=state[10];
    if(HOST_ENV("HALO_SAMPLER_TRACE")) {
        static uint64_t seen[32];static unsigned count;
        uint64_t key=(uint64_t)state[5]|((uint64_t)state[6]<<8)|((uint64_t)state[7]<<16)|((uint64_t)state[10]<<24);
        unsigned i=0;for(;i<count;i++)if(seen[i]==key)break;
        if(i==count&&count<32){seen[count++]=key;host_log("[sampler] mag=%u min=%u mip=%u aniso=%u maxMip=%u",state[5],state[6],state[7],state[10],state[9]);}
    }
}
/* Static vertex buffers are drawn from a resident GPU copy.
 *
 * Halo fills every model part's and BSP section's vertex buffer once, at map
 * load (sub_00524980: created without D3DUSAGE_DYNAMIC in the managed pool,
 * then Lock, copy, Unlock), and draws from it in every frame and bearing.
 * Each of those draws copied the part's whole vertex range into the frame
 * arena, the largest single cost of the host draw path. The copy is now
 * taken once per content generation and bound instead.
 *
 * The draw reads the same bytes it would have copied because:
 * - content_generation moves on Lock, Unlock and ProcessVertices, the only
 *   host paths that hand out or write buffer memory, and a buffer is never
 *   copied or bound while it is locked;
 * - dynamic buffers (particles, decals, the streamed index buffer) and
 *   buffers rewritten again and again keep the per-draw copy;
 * - the copy belongs to the D3D object and is released with it, so a reused
 *   object slot or recycled guest pages never meet an old copy;
 * - only DrawPrimitive/DrawIndexedPrimitive pass stream zero's buffer here;
 *   the UP draws take user memory while stream zero still names a buffer.
 * Every reused range is compared with guest bytes before binding, including
 * writes through a retained Lock pointer. A mismatch retires the copy before
 * that draw uses it. This replaces arena writes with reads without assuming
 * that every guest write passed through a host API. HALO_DRAW_FASTPATH=1 is
 * opt-in; the default path keeps the existing arena copies. */
#define VB_RESIDENT_MAX_COPIES 4
static uint64_t vb_resident_draws,vb_resident_copies_taken,vb_resident_mismatches;
static void vb_resident_retire(D3DObj *vb){vb_resident_drop(vb);vb->resident_refused=1;}
/* Equality, not lexicographic order: reduce all words without the early-out
 * dependency of memcmp. memcpy permits unaligned streams and avoids aliasing
 * assumptions; the compiler can vectorize the equal (usual) case. */
static int vb_resident_equal(const void *left,const void *right,size_t length){
    const uint8_t *a=left,*b=right;uint64_t different=0;size_t i=0;
    for(;length-i>=8;i+=8){uint64_t x,y;memcpy(&x,a+i,8);memcpy(&y,b+i,8);different|=x^y;}
    for(;i<length;i++)different|=(uint64_t)(a[i]^b[i]);
    return !different;
}
static const void *vb_resident(D3DObj *vb,uint32_t start,uint64_t length){
    if(!vb||vb->kind!=K_VB||vb->resident_refused||vb->locked||(vb->usage&0x200u)||!vb->data||!mr_fast_paths_enabled())return NULL;
    if((start&3u)||(uint64_t)start+length>vb->size)return NULL;
    if(!vb->resident||vb->resident_generation!=vb->content_generation){
        if(vb->resident_copies>=VB_RESIDENT_MAX_COPIES){vb_resident_retire(vb);return NULL;}
        /* A stale generation cannot be reused. Release its ownership before
         * allocating the replacement; encoded commands still retain it. A
         * full cache falls back to the exact arena path and retries later. */
        vb_resident_drop(vb);
        if(vb->size>VB_RESIDENT_BUDGET_BYTES-vb_resident_live_bytes){vb_resident_budget_fallbacks++;return NULL;}
        void *copy=mr_buffer_create(GPTR(vb->data),vb->size);if(!copy)return NULL;
        vb->resident=copy;vb->resident_generation=vb->content_generation;vb->resident_copies++;vb_resident_copies_taken++;
        vb_resident_live_bytes+=vb->size;
        if(vb_resident_live_bytes>vb_resident_peak_bytes)vb_resident_peak_bytes=vb_resident_live_bytes;
    }
    else {
        if(!vb_resident_equal((const uint8_t *)mr_buffer_contents(vb->resident)+start,GPTR(vb->data+start),(size_t)length)){
            vb_resident_mismatches++;
            static unsigned reports;if(reports++<16)host_log("[vb-resident] vertex buffer %08x (usage %x, %u bytes) changed without a Lock; drawing it from guest memory from now on",vb->guest,vb->usage,vb->size);
            vb_resident_retire(vb);return NULL;
        }
    }
    vb_resident_draws++;
    return vb->resident;
}
/* Every 600 presented frames: what the draw path copied and what it bound. */
static void draw_traffic_report(void){
    if(frames_presented%600)return;
    uint64_t resident=0,arena=0,folded=0;mr_draw_traffic_stats(&resident,&arena,&folded);
    host_log("[draw-traffic] frame=%u residentDraws=%llu residentMB=%.1f arenaVertexMB=%.1f copies=%llu mismatches=%llu foldedClears=%llu fast=%d cacheMB=%.3f peakCacheMB=%.3f budgetFallbacks=%llu",
        frames_presented,(unsigned long long)vb_resident_draws,resident/1048576.0,arena/1048576.0,(unsigned long long)vb_resident_copies_taken,
        (unsigned long long)vb_resident_mismatches,(unsigned long long)folded,mr_fast_paths_enabled(),
        vb_resident_live_bytes/1048576.0,vb_resident_peak_bytes/1048576.0,(unsigned long long)vb_resident_budget_fallbacks);
}
/* Halo fogs by depth along the view's forward vector (vertex constants c6
 * and c8, and c7 while the camera is in a fog region with no plane, planar
 * fog type 007C1424 = 2). Each panorama bearing has its own forward, so a
 * point near the join with the up or down view was fogged differently on
 * either side of it. While a bearing is drawn the vertex shaders with fog
 * terms measure fog from the eye instead (radial_fog.h), which every bearing
 * agrees on. HALO_RADIAL_FOG=1 opts in; default off until headset A/B
 * validation. The flat window keeps PC's fog. Shaders without fog terms never see the flag, so
 * they keep one pipeline whichever way it is set. 005176D0 sets 007C1424 once
 * per view, before any of the view's draws. */
/* Off unless halo_settings.c is linked, as it is in the app and the probe;
 * device tests that include d3d9.c without it keep PC fog. */
#ifndef HALO_TEST_RADIAL_FOG_SETTING
__attribute__((weak)) int halo_settings_radial_fog(void) { return 0; }
#endif
static int radial_fog_mode(const D3DObj *vs) {
    if(!halo_settings_radial_fog()||panorama_pass<0||vs->radial_fog_terms<=0)return 0;
    return vs->radial_fog_view_plane&&G16(0x007C1424u)==2?HALO_RADIAL_FOG_VIEW_PLANE:HALO_RADIAL_FOG_DEPTH;
}
static uint32_t gpu_program_draw(uint32_t prim,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t indexformat,int32_t base,D3DObj *source) {
    if(panorama_skip_ui_draw())return D3D_OK;
    D3DObj *vs=obj_from_guest(draw_state.vs),*ps=obj_from_guest(draw_state.ps),*decl=obj_from_guest(draw_state.decl),*rt=panorama_draw_surface();
    if(!vs||vs->kind!=K_VSHADER||!decl||decl->kind!=K_VDECL)return unsupported_draw("programmable object binding");
    if(!nv||nv>65536||!stride||stride>256||(uint64_t)vertices+(uint64_t)nv*stride>0x100000000ULL)return D3DERR_INVALIDCALL;
    if(prim<4||prim>6)return unsupported_draw("programmable topology");
    if(draw_state.rs[194]||draw_state.rs[206]||draw_state.rs[174])return unsupported_draw("sRGB/separatealpha/scissor");
    if(draw_state.rs[185])return unsupported_draw("two-sided stencil");
    if(surface_prepare(rt))return D3DERR_NOTAVAILABLE;
    if(rt==panorama_hud)mr_set_overlay_target(rt->renderer);
    mr_program_state state={0};
    state.vertex_tokens=GPTR(vs->data);state.vertex_token_bytes=vs->size;state.vertex_key=vs->content_key;
    if(ps){state.pixel_tokens=GPTR(ps->data);state.pixel_token_bytes=ps->size;state.pixel_key=ps->content_key;}
    state.declaration=GPTR(decl->data);state.declaration_bytes=decl->size;
    state.vs_float4=&draw_state.vs_float[0][0];state.vs_float4_count=256;
    state.ps_float4=&draw_state.ps_float[0][0];state.ps_float4_count=224;
    state.texture_factor=draw_state.rs[60];state.color_write_mask=(int)draw_state.rs[168];
    state.fog_enable=draw_state.rs[28]!=0;state.fog_color=draw_state.rs[34];state.radial_fog=radial_fog_mode(vs);
    if(rt==panorama_hud && state.color_write_mask)state.color_write_mask|=8;
    state.depth_enable=draw_state.rs[7]!=0;state.depth_write=draw_state.rs[14]!=0;state.depth_compare=(int)draw_state.rs[23];state.cull_mode=(int)draw_state.rs[22];
    state.stencil_enable=draw_state.rs[52]!=0;state.stencil_fail=(int)draw_state.rs[53];state.stencil_depth_fail=(int)draw_state.rs[54];state.stencil_pass=(int)draw_state.rs[55];state.stencil_compare=(int)draw_state.rs[56];
    state.stencil_ref=draw_state.rs[57];state.stencil_read_mask=draw_state.rs[58];state.stencil_write_mask=draw_state.rs[59];
    state.alpha_test_ref=draw_state.rs[15]?(int)(draw_state.rs[24]&255):-1;
    state.alpha_test_func=(int)draw_state.rs[25];
    if(!map_blend_state(&state.blend,&state.blend_src,&state.blend_dst,&state.blend_op))return unsupported_draw("blend factors");
    /* Secondary streams: the same vertex index addresses every stream. */
    {
        uint32_t first_vertex=0;D3DObj *stream0=obj_from_guest(draw_state.stream[0]);
        if(stream0&&stream0->kind==K_VB&&stride==draw_state.stride[0]){
            uint64_t origin=(uint64_t)stream0->data+draw_state.offset[0];
            if(vertices>=origin&&((uint64_t)vertices-origin)%stride==0)first_vertex=(uint32_t)(((uint64_t)vertices-origin)/stride);
        }
        const uint8_t *el=(const uint8_t*)GPTR(decl->data);
        for(uint32_t n=0;(uint64_t)n*8+8<=decl->size;n++){
            const uint8_t *e=el+(size_t)n*8;uint16_t sidx=(uint16_t)(e[0]|(e[1]<<8));
            if(sidx==0xFF)break;if(!sidx||sidx>=16||state.stream_bytes[sidx])continue;
            D3DObj *vb=obj_from_guest(draw_state.stream[sidx]);uint32_t sstride=draw_state.stride[sidx];
            if(!vb||vb->kind!=K_VB||!sstride||sstride>256)return unsupported_draw("secondary stream binding");
            uint64_t start=(uint64_t)draw_state.offset[sidx]+(uint64_t)first_vertex*sstride,length=(uint64_t)nv*sstride;
            if(start+length>vb->size||(uint64_t)vb->data+start+length>0x100000000ULL)return unsupported_draw("secondary stream range");
            state.stream_bytes[sidx]=GPTR(vb->data+(uint32_t)start);state.stream_stride[sidx]=sstride;state.stream_length[sidx]=(size_t)length;
            state.resident[sidx]=vb_resident(vb,(uint32_t)start,length);state.resident_offset[sidx]=(size_t)start;
        }
    }
    if(source&&vertices>=source->data){state.resident[0]=vb_resident(source,vertices-source->data,(uint64_t)nv*stride);state.resident_offset[0]=vertices-source->data;}
    for(int st=0;st<8;st++){
        uint32_t *t=draw_state.ts[st];mr_fixed_stage *f=&state.fixed_stages[st];
        f->color_op=t[1];f->color_arg1=t[2];f->color_arg2=t[3];f->alpha_op=t[4];f->alpha_arg1=t[5];f->alpha_arg2=t[6];
        f->texcoord_index=t[11];f->texture_transform_flags=t[24];f->color_arg0=t[26];f->alpha_arg0=t[27];f->result_arg=t[28];f->constant=t[32];
    }
    uint32_t result=D3DERR_NOTAVAILABLE,norm_count=0;uint16_t *norm=draw_indices(prim,nv,indices,ni,indexformat,base,&norm_count);
    if((indices||prim==6)&&!norm){result=D3DERR_INVALIDCALL;goto done;}
    fp_capture_normalized(norm,norm_count);model_capture_normalized(norm,norm_count);
    int fixed_end=8;for(int i=0;i<8;i++)if(state.fixed_stages[i].color_op==1){fixed_end=i;break;}
    for(int st=0;st<16;st++){
        if(!ps&&st>=fixed_end)break;
        if(!ps&&!mr_fixed_stage_uses_texture(&state.fixed_stages[st]))continue;
        mr_program_sampler *m=&state.samplers[st];program_sampler_settings(m,st);
        D3DObj *t=obj_from_guest(draw_state.texture[st]);if(!t)continue;
        if(t->kind==K_TEXTURE){m->type=MR_SAMPLER_2D;m->texture=uploaded_texture(rt->renderer,t);}
        else if(t->kind==K_CUBETEX){m->type=MR_SAMPLER_CUBE;m->texture=uploaded_cube_texture(rt->renderer,t);}
        else if(t->kind==K_VOLTEX){m->type=MR_SAMPLER_VOLUME;m->texture=uploaded_volume_texture(rt->renderer,t);}
        else{result=unsupported_draw("texture kind");goto done;}
        if(!m->texture){result=unsupported_draw("texture upload");goto done;}
    }
    mr_set_viewport(rt->renderer,(int)draw_state.viewport[0],(int)draw_state.viewport[1],(int)draw_state.viewport[2],(int)draw_state.viewport[3]);
    int status=mr_draw_program(rt->renderer,&state,halo_mr_primitive(prim),GPTR(vertices),stride,nv,norm,norm_count);
    if(status){render_trace_failure(mr_last_error());static int reports;if(reports++<30)host_log("d3d9 shader draw error %d: %s",status,mr_last_error());}
    else{rt->gpu_dirty=1;draw_calls++;result=D3D_OK;}
done:
    return result;   /* norm is the shared index scratch */
}
static void ff3d_mul(float *r,const float *a,const float *b){
    for(int i=0;i<4;i++)for(int j=0;j<4;j++){float s=0;for(int k=0;k<4;k++)s+=a[i*4+k]*b[k*4+j];r[i*4+j]=s;}
}
static int ff3d_stage_uses_texture(const mr_fixed_stage *s){
    return mr_fixed_stage_uses_texture(s);
}
static int ff3d_read_uv(const uint8_t *v,uint32_t stride,int off,int type,float out[2]){
    out[0]=out[1]=0;if(off<0)return 0;
    uint32_t count=type==0?1:(type==1||type==2||type==3)?2:0;
    if(!count||(uint32_t)off+count*4>stride)return 0;
    memcpy(&out[0],v+off,4);if(count>1)memcpy(&out[1],v+off+4,4);return 1;
}
/* Fixed-function 3D: no vertex shader, position is untransformed float3 in a vertex
   declaration; CPU-transform by world*view*proj and feed the pretransformed path.
   Returns D3D_OK on draw, D3DERR_NOTAVAILABLE to fall through to the 2D path. */
static uint32_t ff3d_draw(uint32_t prim,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t indexformat,int32_t base) {
    D3DObj *decl=obj_from_guest(draw_state.decl); if(!decl||decl->kind!=K_VDECL) return D3DERR_NOTAVAILABLE;
    const uint8_t *el=(const uint8_t*)GPTR(decl->data);
    int posOff=-1,posType=-1,uvOff[8],uvType[8],uvStream[8],colOff=-1,colType=-1,specOff=-1,specType=-1,nmOff=-1;
    for(int i=0;i<8;i++)uvOff[i]=uvType[i]=uvStream[i]=-1;
    for(uint32_t n=0;(uint64_t)n*8+8<=decl->size;n++){ const uint8_t*e=el+(size_t)n*8;
        uint16_t stream=(uint16_t)(e[0]|(e[1]<<8)); if(stream==0xFF)break; if(stream>=16)continue;
        uint16_t off=(uint16_t)(e[2]|(e[3]<<8)); uint8_t type=e[4],usage=e[6],uidx=e[7];
        if(stream==0&&usage==0&&posOff<0){posOff=off;posType=type;} /* POSITION */
        else if(usage==5&&uidx<8&&uvOff[uidx]<0){uvOff[uidx]=off;uvType[uidx]=type;uvStream[uidx]=stream;} /* TEXCOORD */
        else if(stream==0&&usage==10&&uidx==0&&colOff<0){colOff=off;colType=type;} /* COLOR0 */
        else if(stream==0&&usage==10&&uidx==1&&specOff<0){specOff=off;specType=type;} /* COLOR1/specular */
        else if(stream==0&&usage==3&&nmOff<0&&type==2){nmOff=off;} /* NORMAL float3 */
    }
    if(posOff<0||(posType!=2&&posType!=3)) return D3DERR_NOTAVAILABLE; /* need untransformed float3/4 */
    if((uint32_t)posOff+12>stride) return D3DERR_NOTAVAILABLE;
    { static int logged; if(HOST_ENV("HALO_FF3D_LOG")&&logged<20){ logged++;
        D3DObj *lt=obj_from_guest(draw_state.texture[0]);
        host_log("[ff3d] #%d nv=%u blendEn=%u sb=%u db=%u alphaTest=%u tex0=%08x fmt=%u %ux%u ts0op=%u ts0arg1=%u ts0arg2=%u depth=%u zw=%u",
            logged,nv,draw_state.rs[27],draw_state.rs[19],draw_state.rs[20],draw_state.rs[15],draw_state.texture[0],lt?lt->format:0,lt?lt->width:0,lt?lt->height:0,draw_state.ts[0][1],draw_state.ts[0][2],draw_state.ts[0][3],draw_state.rs[7],draw_state.rs[14]); } }
    if(panorama_skip_ui_draw())return D3D_OK;
    D3DObj *rt=panorama_draw_surface(); if(surface_prepare(rt)) return D3DERR_NOTAVAILABLE;
    if(rt==panorama_hud)mr_set_overlay_target(rt->renderer);
    if(panorama_pass>=0 && panorama_px==0 && the_device && rt->guest==the_device->data && draw_state.rs[7]) {
        panorama_px=fabsf(draw_state.proj[0])*(float)draw_state.viewport[2]/rt->width;
        panorama_py=fabsf(draw_state.proj[5])*(float)draw_state.viewport[3]/rt->height;
        panorama_draw_viewport[0]=(float)draw_state.viewport[0]/rt->width;
        panorama_draw_viewport[1]=(float)draw_state.viewport[1]/rt->height;
        panorama_draw_viewport[2]=((float)draw_state.viewport[0]+draw_state.viewport[2])/rt->width;
        panorama_draw_viewport[3]=((float)draw_state.viewport[1]+draw_state.viewport[3])/rt->height;
    }
    float wv[16],wvp[16]; ff3d_mul(wv,draw_state.world,draw_state.view); ff3d_mul(wvp,wv,draw_state.proj);
    float vpx=(float)draw_state.viewport[0],vpy=(float)draw_state.viewport[1];
    float vpw=(float)draw_state.viewport[2],vph=(float)draw_state.viewport[3];
    if(vpw<=0||vph<=0) return D3DERR_NOTAVAILABLE;
    uint32_t first_vertex=0;D3DObj *stream0=obj_from_guest(draw_state.stream[0]);
    if(stream0&&stream0->kind==K_VB&&stride==draw_state.stride[0]){
        uint64_t origin=(uint64_t)stream0->data+draw_state.offset[0];
        if(vertices>=origin&&((uint64_t)vertices-origin)%stride==0)first_vertex=(uint32_t)(((uint64_t)vertices-origin)/stride);
    }
    int stage_source_coord[8]={0},stage_transform[8]={0};
    mr_program_state material={0};material.color_write_mask=(int)draw_state.rs[168];material.alpha_test_ref=-1;material.cull_mode=(int)draw_state.rs[22];material.texture_factor=draw_state.rs[60];
    /* No-VS low-capability path: vertex fog was previously omitted here.
     * Opt-in range fog uses the same D3D render-state formula in all bearings;
     * the full-capability fog-plane passes use the rewritten VS above. */
    material.fog_enable=halo_settings_radial_fog()&&panorama_pass>=0&&draw_state.rs[28]&&draw_state.rs[35]==0;
    material.fog_color=draw_state.rs[34];material.radial_fog=material.fog_enable?HALO_RADIAL_FOG_DEPTH:0;
    float fog_start=0,fog_end=0,fog_density=0;
    if(material.fog_enable){memcpy(&fog_start,&draw_state.rs[36],4);memcpy(&fog_end,&draw_state.rs[37],4);memcpy(&fog_density,&draw_state.rs[38],4);}
    mr_draw_state simple={.color_write_mask=(int)draw_state.rs[168],.alpha_test_ref=-1,.cull_mode=(int)draw_state.rs[22]};
    if(rt==panorama_hud && material.color_write_mask){material.color_write_mask|=8;simple.color_write_mask|=8;}
    if(!map_blend_state(&material.blend,&material.blend_src,&material.blend_dst,&material.blend_op))return unsupported_draw("fixed 3D blend factors");
    simple.blend=material.blend;simple.blend_src=material.blend_src;simple.blend_dst=material.blend_dst;simple.blend_op=material.blend_op;
    if(draw_state.rs[15]&&!HOST_ENV("HALO_FF3D_NOALPHA")){material.alpha_test_ref=(int)(draw_state.rs[24]&255);material.alpha_test_func=(int)draw_state.rs[25];simple.alpha_test_ref=material.alpha_test_func==7?material.alpha_test_ref:-1;}
    material.depth_enable=simple.depth_enable=draw_state.rs[7]!=0;material.depth_write=simple.depth_write=draw_state.rs[14]!=0;material.depth_compare=simple.depth_compare=(int)draw_state.rs[23];
    material.stencil_enable=simple.stencil_enable=draw_state.rs[52]!=0;material.stencil_fail=simple.stencil_fail=(int)draw_state.rs[53];material.stencil_depth_fail=simple.stencil_depth_fail=(int)draw_state.rs[54];material.stencil_pass=simple.stencil_pass=(int)draw_state.rs[55];material.stencil_compare=simple.stencil_compare=(int)draw_state.rs[56];
    material.stencil_ref=simple.stencil_ref=draw_state.rs[57];material.stencil_read_mask=simple.stencil_read_mask=draw_state.rs[58];material.stencil_write_mask=simple.stencil_write_mask=draw_state.rs[59];
    /* base texture: stage 0 */
    int ff3d_flat=HOST_ENV("HALO_FF3D_FLAT")!=NULL;
    int ff3d_shade=HOST_ENV("HALO_FF3D_SHADE")!=NULL; /* shade geometry by vertex normal (clean lit look) */
    int ff3d_white=HOST_ENV("HALO_FF3D_WHITE")!=NULL;
    D3DObj *t=(ff3d_flat||ff3d_shade)?NULL:obj_from_guest(draw_state.texture[0]);
    if(ff3d_flat||ff3d_shade){if(t){simple.texture=uploaded_texture(rt->renderer,t);if(!simple.texture)return unsupported_draw("fixed 3D texture upload");
        simple.linear_filter=draw_state.sampler[0][5]>1;simple.address_clamp=draw_state.sampler[0][1]==3;}}
    else for(int st=0;st<8;st++){
        uint32_t *ts=draw_state.ts[st];mr_fixed_stage *f=&material.fixed_stages[st];
        f->color_op=ts[1];f->color_arg1=ts[2];f->color_arg2=ts[3];f->alpha_op=ts[4];f->alpha_arg1=ts[5];f->alpha_arg2=ts[6];
        f->texcoord_index=ts[11];f->texture_transform_flags=ts[24];f->color_arg0=ts[26];f->alpha_arg0=ts[27];f->result_arg=ts[28];f->constant=ts[32];
        if(f->color_op==1)break;
        program_sampler_settings(&material.samplers[st],st);
        if(ff3d_stage_uses_texture(f)){
            int coord=(int)(f->texcoord_index&0xffffu),flags=(int)f->texture_transform_flags;
            if((f->texcoord_index&0xffff0000u)||(flags!=0&&flags!=2)||coord>=8)return unsupported_draw("fixed 3D texture coordinates");
            if(uvOff[coord]>=0){int us=uvStream[coord];D3DObj *uvb=us>0?obj_from_guest(draw_state.stream[us]):NULL;
                uint32_t elems=uvType[coord]==0?1:(uvType[coord]>=1&&uvType[coord]<=3)?2:0;
                if(!elems||(us>0&&(!uvb||uvb->kind!=K_VB||!draw_state.stride[us]||(uint64_t)draw_state.offset[us]+((uint64_t)first_vertex+nv-1)*draw_state.stride[us]+uvOff[coord]+elems*4>uvb->size)))return unsupported_draw("fixed 3D texture stream");
                stage_source_coord[st]=coord;
            }else stage_source_coord[st]=-1; /* D3D defaults an absent declaration value to zero. */
            stage_transform[st]=flags;f->texcoord_index=(uint32_t)st;f->texture_transform_flags=0;
            D3DObj *stage_tex=obj_from_guest(draw_state.texture[st]);if(!stage_tex||stage_tex->kind!=K_TEXTURE)return unsupported_draw("fixed 3D stage texture");
            material.samplers[st].texture=uploaded_texture(rt->renderer,stage_tex);if(!material.samplers[st].texture)return unsupported_draw("fixed 3D texture upload");
        }
    }
    if(HOST_ENV("HALO_FF3D_PASSES")){ /* one log per distinct (tex0dims,op0,op1,blend,atest) pass signature */
        D3DObj *t0=obj_from_guest(draw_state.texture[0]),*t1=obj_from_guest(draw_state.texture[1]);
        uint32_t sig=(t0?(t0->width*8191u+t0->height):0)*131u + draw_state.ts[0][1]*17u + draw_state.ts[1][1]*7u + (draw_state.rs[27]?(draw_state.rs[19]*13u+draw_state.rs[20]):0)*3u + draw_state.rs[15];
        static uint32_t seen[64]; static int ns2=0; int dup=0; for(int q=0;q<ns2;q++)if(seen[q]==sig)dup=1;
        if(!dup&&ns2<64){seen[ns2++]=sig;
            host_log("[ff3d-pass] tex0=%08x %ux%u op%u | tex1=%08x %ux%u op%u | blend=%u/%u,%u atest=%u zw=%u cw=%u",
                draw_state.texture[0],t0?t0->width:0,t0?t0->height:0,draw_state.ts[0][1],
                draw_state.texture[1],t1?t1->width:0,t1?t1->height:0,draw_state.ts[1][1],
                draw_state.rs[27],draw_state.rs[19],draw_state.rs[20],draw_state.rs[15],draw_state.rs[14],draw_state.rs[168]); } }
    { static int tl; if(HOST_ENV("HALO_FF3D_LOG")&&tl<8){tl++; host_log("[ff3d-tex] tex0guest=%08x created=%u err=%s",draw_state.texture[0],material.samplers[0].texture,material.samplers[0].texture?"ok":mr_last_error()); } }
    uint32_t norm_count=0; uint16_t *ix=draw_indices(prim,nv,indices,ni,indexformat,base,&norm_count);
    if((indices||prim==6)&&!ix)return D3DERR_INVALIDCALL;
    fp_capture_normalized(ix,norm_count);model_capture_normalized(ix,norm_count);
    mr_vertex_fixed_clip *v=draw_scratch_grow(&draw_vertex_scratch,&draw_vertex_capacity,(size_t)nv*sizeof *v);
    if(!v)return 0x8007000Eu;
    memset(v,0,(size_t)nv*sizeof *v);
    HostD3DPreparedLighting prepared_lighting;
    if(draw_state.rs[137])d3d9_prepare_lighting(draw_state.world,&prepared_lighting);
    uint64_t prof_vertex_start=draw_prof_now();
    for(uint32_t j=0;j<nv;j++){
        const uint8_t *vp=(const uint8_t*)GPTR(vertices+(size_t)j*stride);
        float px,py,pz; memcpy(&px,vp+posOff,4);memcpy(&py,vp+posOff+4,4);memcpy(&pz,vp+posOff+8,4);
        if(j==nv/2){ D3DObj*t0=obj_from_guest(draw_state.texture[0]); if(HOST_ENV("HALO_FF3D_RAW")&&t0&&t0->width>=1024){ static int rl; if(rl<4){rl++;
            const uint8_t*b=vp; host_log("[ff3d-raw] BASE tex0=%08x nv=%u off=%u stride=%u posOff=%d bytes=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",draw_state.texture[0],nv,draw_state.offset[0],stride,posOff,b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]); } } }
        float cx=px*wvp[0]+py*wvp[4]+pz*wvp[8]+wvp[12];
        float cy=px*wvp[1]+py*wvp[5]+pz*wvp[9]+wvp[13];
        float cz=px*wvp[2]+py*wvp[6]+pz*wvp[10]+wvp[14];
        float cw=px*wvp[3]+py*wvp[7]+pz*wvp[11]+wvp[15];
        /* Preserve homogeneous position through the camera plane.  Dividing
         * by cw here and reconstructing it in Metal produces inf/NaN at cw=0
         * and can turn a clipped primitive into a screen-sized triangle. */
        v[j].x=cx; v[j].y=cy; v[j].z=cz; v[j].w=cw;
        float nx=0,ny=0,nz=1;if(nmOff>=0&&(uint32_t)nmOff+12<=stride){memcpy(&nx,vp+nmOff,4);memcpy(&ny,vp+nmOff+4,4);memcpy(&nz,vp+nmOff+8,4);}
        uint32_t col=ff3d_flat?0xFF20C020u:0xFFFFFFFFu,specular=0;
        if(ff3d_shade){
            float nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl>1e-6f){nx/=nl;ny/=nl;nz/=nl;}
            /* two-tone key+fill lambert on a neutral surface, world up = +Z */
            float d1=nx*0.40f+ny*0.20f+nz*0.90f; if(d1<0)d1=-0.3f*d1; float sh=0.28f+0.62f*(d1>1?1:d1);
            if(sh<0)sh=0; if(sh>1)sh=1; unsigned g=(unsigned)(sh*205.0f+16.0f); if(g>255)g=255;
            col=0xFF000000u|(g<<16)|(g<<8)|((g*13/12>255?255:g*13/12)); /* slight cool tint */
        }
        else if(!ff3d_flat){if(colOff>=0&&colType==4&&(uint32_t)colOff+4<=stride&&!ff3d_white)memcpy(&col,vp+colOff,4);
            if(specOff>=0&&specType==4&&(uint32_t)specOff+4<=stride)memcpy(&specular,vp+specOff,4);
            if(draw_state.rs[137]){float position[3]={px,py,pz},normal[3]={nx,ny,nz};col=d3d9_light_vertex_prepared(&prepared_lighting,position,normal,col,specular);}}
        v[j].color=col;v[j].specular=specular;
        if(material.fog_enable){
            float view_position[3]={px*wv[0]+py*wv[4]+pz*wv[8]+wv[12],
                                   px*wv[1]+py*wv[5]+pz*wv[9]+wv[13],
                                   px*wv[2]+py*wv[6]+pz*wv[10]+wv[14]};
            v[j].fog=halo_fixed_radial_fog(view_position,draw_state.rs[140],fog_start,fog_end,fog_density,(specular>>24)/255.f);
        }
        for(int st=0;st<8&&material.fixed_stages[st].color_op!=1;st++)if(ff3d_stage_uses_texture(&material.fixed_stages[st])){
            int tc=stage_source_coord[st];if(tc<0)continue;int us=uvStream[tc];const uint8_t *tv=vp;uint32_t ts_stride=stride;
            if(us>0){D3DObj *uvb=obj_from_guest(draw_state.stream[us]);tv=(const uint8_t*)GPTR(uvb->data+draw_state.offset[us]+(first_vertex+j)*draw_state.stride[us]);ts_stride=draw_state.stride[us];}
            float uv[2];ff3d_read_uv(tv,ts_stride,uvOff[tc],uvType[tc],uv);
            if(stage_transform[st]==2){const float*m=draw_state.texture_transform[st];v[j].uv[st][0]=uv[0]*m[0]+uv[1]*m[4]+m[12];v[j].uv[st][1]=uv[0]*m[1]+uv[1]*m[5]+m[13];}
            else{v[j].uv[st][0]=uv[0];v[j].uv[st][1]=uv[1];}
        }
        if(j==nv/2 && (px!=0.0f||py!=0.0f||pz!=0.0f)){ static int vl; if(HOST_ENV("HALO_FF3D_LOG")&&vl<10){vl++;
            host_log("[ff3d-vtx] nv=%u tex0=%08x/op%u tex1=%08x/op%u tex2=%08x/op%u tex3=%08x/op%u blend=%u/%u,%u atest=%u",nv,
                draw_state.texture[0],draw_state.ts[0][1],draw_state.texture[1],draw_state.ts[1][1],draw_state.texture[2],draw_state.ts[2][1],draw_state.texture[3],draw_state.ts[3][1],
                draw_state.rs[27],draw_state.rs[19],draw_state.rs[20],draw_state.rs[15]);} }
    }
    if(prof_vertex_start){draw_prof_vertex_ns+=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-prof_vertex_start;++draw_prof_draws;}
    mr_set_viewport(rt->renderer,(int)vpx,(int)vpy,(int)vpw,(int)vph);
    fp_capture_clip(v,nv);model_capture_clip(v,nv);
    uint64_t prof_submit_start=draw_prof_now();
    uint32_t result=D3D_OK;
    int status;
    if(ff3d_flat||ff3d_shade){
        /* Diagnostic flat/shade modes use the normal material shader too so
         * CPU-transformed geometry remains in homogeneous clip space. */
        mr_program_state diagnostic=material;diagnostic.fixed_stages[0].color_op=2;diagnostic.fixed_stages[0].color_arg1=0;
        diagnostic.fixed_stages[0].alpha_op=2;diagnostic.fixed_stages[0].alpha_arg1=0;diagnostic.fixed_stages[0].result_arg=1;diagnostic.fixed_stages[1].color_op=1;
        diagnostic.blend=simple.blend;diagnostic.color_write_mask=simple.color_write_mask;diagnostic.alpha_test_ref=simple.alpha_test_ref;
        status=mr_draw_fixed_clip(rt->renderer,&diagnostic,halo_mr_primitive(prim),v,sizeof *v,nv,ix,norm_count);
    }else status=mr_draw_fixed_clip(rt->renderer,&material,halo_mr_primitive(prim),v,sizeof *v,nv,ix,norm_count);
    if(prof_submit_start)draw_prof_submit_ns+=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)-prof_submit_start;
    if(status){render_trace_failure(mr_last_error());result=D3DERR_NOTAVAILABLE;}
    else{rt->gpu_dirty=1;draw_calls++;}
    return result;
}
/* `source` is stream zero's vertex buffer for DrawPrimitive and
 * DrawIndexedPrimitive, NULL for the UP draws. */
static uint32_t gpu_draw_impl(uint32_t prim,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t indexformat,int32_t base,D3DObj *source) {
    if(panorama_ui_active && panorama_pass>=0 && panorama_pass!=1)return D3D_OK;
    if(draw_reject_enabled()){
        DrawRejectContext *d=&draw_reject_trace.draw;d->primitive=prim;d->vertices=vertices;d->stride=stride;d->count=nv;d->indices=indices;d->index_count=ni;d->index_format=indexformat;d->base=base;++d->serial;
    }
    capture_draw(prim,vertices,stride,nv,indices,ni,indexformat,base);
    fp_capture_draw(prim,vertices,stride,nv,indices,ni,indexformat,base);model_capture_draw(prim,vertices,stride,nv,indices,ni,indexformat,base);
    if(draw_state.vs){fp_capture_backend("programmable");model_capture_backend("programmable");return gpu_program_draw(prim,vertices,stride,nv,indices,ni,indexformat,base,source);}
    if(HOST_ENV("HALO_FF3D") && !draw_state.ps && !draw_state.fvf && draw_state.decl && !draw_state.rs[174] && !draw_state.rs[185]){
        fp_capture_backend("fixed3D");model_capture_backend("fixed3D");
        uint32_t r=ff3d_draw(prim,vertices,stride,nv,indices,ni,indexformat,base);
        if(r!=D3DERR_NOTAVAILABLE||render_trace_reason[0]) return r; }
    fp_capture_backend("RHW");model_capture_backend("RHW");
    uint32_t rhw_fvf=draw_state.fvf;
    if(!rhw_fvf){D3DObj *decl=obj_from_guest(draw_state.decl);if(decl&&decl->kind==K_VDECL)rhw_fvf=halo_rhw_declaration_fvf(GPTR(decl->data),decl->size);}
    if(rhw_fvf!=0x144 && rhw_fvf!=0x1c4 && rhw_fvf!=0x244) return unsupported_draw("vertex layout");
    if(prim<1 || prim>6) return unsupported_draw("primitive topology");
    if(draw_state.rs[28]) return unsupported_draw("fog");
    if(draw_state.rs[185])return unsupported_draw("two-sided stencil");
    if(draw_state.rs[206] || draw_state.rs[194] || draw_state.rs[174]) return unsupported_draw("separate alpha, sRGB, or scissor");
    uint32_t minimum_stride=rhw_fvf==0x244?36u:rhw_fvf==0x1c4?32u:28u;
    if(!vertices || stride<minimum_stride||nv>65536||!nv||ni>196608) return D3DERR_INVALIDCALL;
    if((uint64_t)vertices+(uint64_t)stride*(nv-1)+minimum_stride>0x100000000ULL) return D3DERR_INVALIDCALL;
    mr_draw_state s={.color_write_mask=(int)draw_state.rs[168],.alpha_test_ref=-1,.cull_mode=(int)draw_state.rs[22]};
    s.depth_enable=draw_state.rs[7]!=0;s.depth_write=draw_state.rs[14]!=0;s.depth_compare=(int)draw_state.rs[23];
    s.stencil_enable=draw_state.rs[52]!=0;s.stencil_fail=(int)draw_state.rs[53];s.stencil_depth_fail=(int)draw_state.rs[54];s.stencil_pass=(int)draw_state.rs[55];s.stencil_compare=(int)draw_state.rs[56];
    s.stencil_ref=draw_state.rs[57];s.stencil_read_mask=draw_state.rs[58];s.stencil_write_mask=draw_state.rs[59];
    if(!map_blend_state(&s.blend,&s.blend_src,&s.blend_dst,&s.blend_op)) return unsupported_draw("blend factors");
    if(draw_state.ps){ /* pretransformed 2D draw shaded by a bound pixel shader (HUD/screen effects) */
        D3DObj *ps=obj_from_guest(draw_state.ps);if(!ps||ps->kind!=K_PSHADER)return unsupported_draw("pixel shader binding");
        D3DObj *rt=rt_surface();
        if(panorama_ui_active && panorama_pass>=0 && the_device && rt && panorama_scene_target(rt)){if(panorama_pass!=1)return D3D_OK;if(panorama_hud)rt=panorama_hud;}
        if(surface_prepare(rt))return D3DERR_NOTAVAILABLE;
        mr_program_state pstate={0};pstate.pixel_tokens=GPTR(ps->data);pstate.pixel_token_bytes=ps->size;pstate.pixel_key=ps->content_key;
        pstate.ps_float4=&draw_state.ps_float[0][0];pstate.ps_float4_count=224;pstate.texture_factor=draw_state.rs[60];
        pstate.fog_enable=draw_state.rs[28]!=0;pstate.fog_color=draw_state.rs[34];
        pstate.color_write_mask=s.color_write_mask;if(rt==panorama_hud){mr_set_overlay_target(rt->renderer);if(pstate.color_write_mask)pstate.color_write_mask|=8;}
        pstate.blend=s.blend;pstate.blend_src=s.blend_src;pstate.blend_dst=s.blend_dst;pstate.blend_op=s.blend_op;
        pstate.alpha_test_ref=draw_state.rs[15]?(int)(draw_state.rs[24]&255):-1;pstate.alpha_test_func=(int)draw_state.rs[25];
        pstate.depth_enable=s.depth_enable;pstate.depth_write=s.depth_write;pstate.depth_compare=s.depth_compare;pstate.cull_mode=s.cull_mode?s.cull_mode:1;
        pstate.stencil_enable=s.stencil_enable;pstate.stencil_fail=s.stencil_fail;pstate.stencil_depth_fail=s.stencil_depth_fail;pstate.stencil_pass=s.stencil_pass;pstate.stencil_compare=s.stencil_compare;
        pstate.stencil_ref=s.stencil_ref;pstate.stencil_read_mask=s.stencil_read_mask;pstate.stencil_write_mask=s.stencil_write_mask;
        for(int st=0;st<16;st++){mr_program_sampler *m=&pstate.samplers[st];program_sampler_settings(m,st);D3DObj *t=obj_from_guest(draw_state.texture[st]);if(!t)continue;
            if(t->kind==K_TEXTURE){m->type=MR_SAMPLER_2D;m->texture=uploaded_texture(rt->renderer,t);}
            else if(t->kind==K_CUBETEX){m->type=MR_SAMPLER_CUBE;m->texture=uploaded_cube_texture(rt->renderer,t);}
            else if(t->kind==K_VOLTEX){m->type=MR_SAMPLER_VOLUME;m->texture=uploaded_volume_texture(rt->renderer,t);}
            else continue;
            if(!m->texture)return unsupported_draw("texture upload");}
        uint32_t norm_count=0;uint16_t *ix=draw_indices(prim,nv,indices,ni,indexformat,base,&norm_count);
        if((indices||prim==6)&&!ix)return D3DERR_INVALIDCALL;
        fp_capture_normalized(ix,norm_count);model_capture_normalized(ix,norm_count);
        mr_vertex_fixed_rhw *v=draw_scratch_grow(&draw_vertex_scratch,&draw_vertex_capacity,(size_t)nv*sizeof *v);
        if(!v)return 0x8007000Eu;
        memset(v,0,(size_t)nv*sizeof *v);
        for(uint32_t j=0;j<nv;j++){const uint8_t *src=(const uint8_t*)GPTR(vertices+j*stride);
            memcpy(&v[j].x,src,16);memcpy(&v[j].color,src+16,4);unsigned uvoff=20;
            if(rhw_fvf==0x1c4){memcpy(&v[j].specular,src+20,4);uvoff=24;}
            memcpy(v[j].uv[0],src+uvoff,8);if(rhw_fvf==0x244)memcpy(v[j].uv[1],src+uvoff+8,8);
            v[j].x+=0.5f;v[j].y+=0.5f;}
        mr_set_viewport(rt->renderer,(int)draw_state.viewport[0],(int)draw_state.viewport[1],(int)draw_state.viewport[2],(int)draw_state.viewport[3]);
        int status=mr_draw_program_rhw(rt->renderer,&pstate,halo_mr_primitive(prim),v,sizeof *v,nv,ix,norm_count);
        if(status){render_trace_failure(mr_last_error());static int reports;if(reports++<30)host_log("d3d9 2D shader draw error %d: %s",status,mr_last_error());return D3DERR_NOTAVAILABLE;}
        rt->gpu_dirty=1;draw_calls++;return D3D_OK;
    }
    if(draw_state.rs[15]) {
        if(draw_state.rs[25]!=7) return unsupported_draw("alpha comparison");
        s.alpha_test_ref=(int)(draw_state.rs[24]&255);
    }
    int need_fragment=s.color_write_mask||s.alpha_test_ref>=0;
    if(need_fragment&&draw_state.texture[1] && draw_state.ts[1][1]!=1) return unsupported_draw("multiple texture stages");
    D3DObj *rt=rt_surface();
    if(panorama_ui_active && panorama_pass>=0 && the_device && rt && panorama_scene_target(rt)) {
        if(panorama_pass!=1)return D3D_OK;
        if(panorama_hud)rt=panorama_hud;
    }
    if(surface_prepare(rt)) return D3DERR_NOTAVAILABLE;
    if(rt==panorama_hud){mr_set_overlay_target(rt->renderer); if(s.color_write_mask)s.color_write_mask|=8;}
    mr_program_sampler texture_sampler={0};
    D3DObj *t=need_fragment?obj_from_guest(draw_state.texture[0]):NULL; /* depth-only prepass needs no fragment texture state */
    if(t && draw_state.ts[0][1]!=1) {
        if(t->kind!=K_TEXTURE || !t->level_data[0]) return unsupported_draw("texture kind or empty level");
        /* At stage zero CURRENT is the input diffuse color. Halo's additive
         * particles modulate texture RGB by DIFFUSE but select DIFFUSE alpha;
         * requiring alpha from the texture discarded those entire draws. */
        const uint32_t *stage=draw_state.ts[0];
        int color_ok=(stage[1]==2&&stage[2]==2)||
                     (stage[1]==4&&stage[2]==2&&(stage[3]==0||stage[3]==1));
        int alpha_diffuse=stage[4]==2&&(stage[5]==0||stage[5]==1);
        int alpha_ok=alpha_diffuse||(stage[4]==2&&stage[5]==2)||
                     (stage[4]==4&&stage[5]==2&&(stage[6]==0||stage[6]==1));
        if(!color_ok||!alpha_ok)return unsupported_draw("texture combiner");
        s.texture_color_only=alpha_diffuse;
        if(draw_state.sampler[0][8] || draw_state.sampler[0][11]) return unsupported_draw("texture sampling LOD bias or sRGB");
        program_sampler_settings(&texture_sampler,0);s.sampler=&texture_sampler;
        s.texture=uploaded_texture(rt->renderer,t);
        if(!s.texture) return unsupported_draw("texture upload");
        s.linear_filter=draw_state.sampler[0][5]==2; s.address_clamp=draw_state.sampler[0][1]==3;
    }
    mr_vertex_rhw *v=draw_scratch_grow(&draw_vertex_scratch,&draw_vertex_capacity,(size_t)nv*sizeof *v);
    uint32_t norm_count=0; uint16_t *ix=draw_indices(prim,nv,indices,ni,indexformat,base,&norm_count);
    uint32_t result=D3DERR_INVALIDCALL;
    if(!v){result=0x8007000Eu;goto finish;}
    if((indices||prim==6)&&!ix)goto finish;
    fp_capture_normalized(ix,norm_count);model_capture_normalized(ix,norm_count);
    for(uint32_t j=0;j<nv;j++) {
        const uint8_t *source=GPTR(vertices+j*stride);
        if(rhw_fvf==0x1c4){memcpy(&v[j],source,20);memcpy(&v[j].u,source+24,8);} /* skip SPECULAR at byte 20 */
        else memcpy(&v[j],source,sizeof *v);
        /* D3D9's integer pixel centers correspond to Metal's half-integer centers. */
        v[j].x+=0.5f;v[j].y+=0.5f;
        if(s.texture && draw_state.ts[0][1]==2) v[j].color|=0xFFFFFF;
        if(s.texture && draw_state.ts[0][4]==2 && !s.texture_color_only) v[j].color|=0xFF000000;
    }
    mr_set_viewport(rt->renderer,(int)draw_state.viewport[0],(int)draw_state.viewport[1],(int)draw_state.viewport[2],(int)draw_state.viewport[3]);
    if(mr_draw_rhw(rt->renderer,&s,halo_mr_primitive(prim),v,sizeof *v,nv,ix,norm_count)) {render_trace_failure(mr_last_error());result=D3DERR_NOTAVAILABLE;}
    else {rt->gpu_dirty=1;draw_calls++;result=D3D_OK;}
finish:
    return result;
}
static uint32_t gpu_draw(uint32_t prim,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t indexformat,int32_t base,D3DObj *source) {
    mr_texture_bindings_begin();
    uint32_t result=gpu_draw_impl(prim,vertices,stride,nv,indices,ni,indexformat,base,source);
    mr_texture_bindings_end();
    return result;
}
static uint32_t primitive_vertices(uint32_t prim,uint32_t count) {
    if(count>65534) return 0;
    return prim==4?count*3:(prim==5||prim==6)?count+2:0;
}
/* Shared by the ordinary device path and focused production lifetime fixtures. */
static int resource_binding_method(EngineCPU *cpu,int i,uint32_t *hr){
    uint32_t slot=ARG(1),value=ARG(2);*hr=D3D_OK;
    switch(i){
    case 64: case 65:
        if(slot>=16||(i==65?!stateblock_valid_binding(value,K_TEXTURE):!value)){*hr=D3DERR_INVALIDCALL;break;}
        if(i==65)binding_replace(&draw_state.texture[slot],value);
        else {stateblock_retain(NULL,draw_state.texture[slot]);S32(value,draw_state.texture[slot]);}break;
    case 87: if(!stateblock_valid_binding(slot,K_VDECL)){*hr=D3DERR_INVALIDCALL;break;}d3d9_set_vertex_input(0,slot);break;
    case 88: if(!slot){*hr=D3DERR_INVALIDCALL;break;}stateblock_retain(NULL,draw_state.decl);S32(slot,draw_state.decl);break;
    case 89: d3d9_set_vertex_input(1,slot);break;
    case 92: case 107:
        if(!stateblock_valid_binding(slot,i==92?K_VSHADER:K_PSHADER)){*hr=D3DERR_INVALIDCALL;break;}
        binding_replace(i==92?&draw_state.vs:&draw_state.ps,slot);break;
    case 93: case 108:
        if(!slot){*hr=D3DERR_INVALIDCALL;break;}value=i==93?draw_state.vs:draw_state.ps;stateblock_retain(NULL,value);S32(slot,value);break;
    case 100:
        if(slot>=16||!stateblock_valid_binding(value,K_VB)){*hr=D3DERR_INVALIDCALL;break;}
        binding_replace(&draw_state.stream[slot],value);draw_state.offset[slot]=ARG(3);draw_state.stride[slot]=ARG(4);break;
    case 101:
        if(slot>=16||!value||!ARG(3)||!ARG(4)){*hr=D3DERR_INVALIDCALL;break;}
        stateblock_retain(NULL,draw_state.stream[slot]);S32(value,draw_state.stream[slot]);S32(ARG(3),draw_state.offset[slot]);S32(ARG(4),draw_state.stride[slot]);break;
    case 104: if(!stateblock_valid_binding(slot,K_IB)){*hr=D3DERR_INVALIDCALL;break;}binding_replace(&draw_state.indices,slot);break;
    case 105: if(!slot){*hr=D3DERR_INVALIDCALL;break;}stateblock_retain(NULL,draw_state.indices);S32(slot,draw_state.indices);break;
    default:return 0;
    }return 1;
}
/* Render/depth targets are live device state and are not captured in D3D9
 * state blocks. Their strong owners are separate from the cached implicit
 * back/depth buffers, which retain their initial device-owned reference. */
static int target_binding_method(EngineCPU *cpu,D3DObj *device,int i,uint32_t *hr){
    *hr=D3D_OK;
    switch(i){
    case 18:
        if(ARG(1)||ARG(2)||ARG(3)||!ARG(4)){*hr=D3DERR_INVALIDCALL;break;}
        obj_retain(obj_find(device->data));S32(ARG(4),device->data);break;
    case 37: {
        D3DObj *surface=obj_find(ARG(2));
        if(ARG(1)||!surface||surface->kind!=K_SURFACE||!(surface->usage&1)){*hr=D3DERR_INVALIDCALL;break;}
        binding_replace(&current_rt_guest,surface->guest);
        draw_state.viewport[0]=draw_state.viewport[1]=0;draw_state.viewport[2]=surface->width;draw_state.viewport[3]=surface->height;
        break;}
    case 38:
        if(ARG(1)||!ARG(2)){*hr=D3DERR_INVALIDCALL;break;}
        obj_retain(obj_find(current_rt_guest));S32(ARG(2),current_rt_guest);break;
    case 39: {
        D3DObj *surface=obj_find(ARG(1));
        if(ARG(1)&&(!surface||surface->kind!=K_SURFACE||!(surface->usage&2))){*hr=D3DERR_INVALIDCALL;break;}
        binding_replace(&current_depth_guest,ARG(1));break;}
    case 40:
        if(!ARG(1)){*hr=D3DERR_INVALIDCALL;break;}
        obj_retain(obj_find(current_depth_guest));S32(ARG(1),current_depth_guest);
        if(!current_depth_guest)*hr=0x88760866u; /* D3DERR_NOTFOUND */
        break;
    default:return 0;
    }
    return 1;
}
/* Returns 1 if handled and writes HRESULT; the outer COM method pops its exact argument count. */
static int render_method(EngineCPU *cpu,D3DObj *o,int i,uint32_t *hr) {
    *hr=D3D_OK;
    if(fp_capture.scope){fp_capture.caller=G32(cpu->gpr[4]);fp_capture.method=(uint32_t)i;}
    if(model_capture.scope.active){model_capture.caller=G32(cpu->gpr[4]);model_capture.method=(uint32_t)i;}
    if(stateblock_record_method(cpu,i,hr))return 1;
    if(resource_binding_method(cpu,i,hr))return 1;
    if(target_binding_method(cpu,o,i,hr))return 1;
    if(i==17)render_trace_flush(); /* Present flushes even the final captured frame. */
    if(i>=81&&i<=84){render_trace_begin();if(draw_reject_enabled())draw_reject_trace.draw.caller=G32(cpu->gpr[4]);}
    switch(i) {
    case 32: case 34: *hr=surface_copy(obj_from_guest(ARG(1)),i==34?ARG(2):0,obj_from_guest(i==34?ARG(3):ARG(2)),i==34?ARG(4):0,i==34?ARG(5):0);break;
    case 35: {D3DObj *s=obj_from_guest(ARG(1));int r[4]; if(!rect_read(ARG(2),s,r)||!s->data){*hr=D3DERR_INVALIDCALL;break;}
        if((s->format!=21&&s->format!=22)||surface_flush(s)){*hr=D3DERR_NOTAVAILABLE;break;}
        for(int y=r[1];y<r[3];y++)for(int x=r[0];x<r[2];x++)S32(s->data+4*(y*s->width+x),ARG(3));s->gpu_dirty=0;content_changed(s);break;}
    case 44: { uint32_t ts=ARG(1),mp=ARG(2); if(mp){ if(ts==2)memcpy(draw_state.view,GPTR(mp),64); else if(ts==3)memcpy(draw_state.proj,GPTR(mp),64); else if(ts==256)memcpy(draw_state.world,GPTR(mp),64); else if(ts>=16&&ts<24)memcpy(draw_state.texture_transform[ts-16],GPTR(mp),64); } break; }
    case 45: {uint32_t ts=ARG(1),mp=ARG(2);const float*m=ts==2?draw_state.view:ts==3?draw_state.proj:ts==256?draw_state.world:(ts>=16&&ts<24)?draw_state.texture_transform[ts-16]:NULL;if(!mp||!m)*hr=D3DERR_INVALIDCALL;else memcpy(GPTR(mp),m,64);break;}
    case 47: memcpy(draw_state.viewport,GPTR(ARG(1)),24);break;
    case 48: memcpy(GPTR(ARG(1)),draw_state.viewport,24);break;
    case 57: case 58: if(ARG(1)>=256){*hr=D3DERR_INVALIDCALL;break;}if(i==57)draw_state.rs[ARG(1)]=ARG(2);else S32(ARG(2),draw_state.rs[ARG(1)]);break;
    case 66: case 67: if(ARG(1)>=8||ARG(2)>=33){*hr=D3DERR_INVALIDCALL;break;}if(i==67)draw_state.ts[ARG(1)][ARG(2)]=ARG(3);else S32(ARG(3),draw_state.ts[ARG(1)][ARG(2)]);break;
    case 68: case 69: if(ARG(1)>=16||ARG(2)>=14){*hr=D3DERR_INVALIDCALL;break;}if(i==69)draw_state.sampler[ARG(1)][ARG(2)]=ARG(3);else S32(ARG(3),draw_state.sampler[ARG(1)][ARG(2)]);break;
    case 90: S32(ARG(1),draw_state.fvf);break;
    case 94: case 95: case 96: case 97: case 98: case 99:
    case 109: case 110: case 111: case 112: case 113: case 114: {
        int pixel=i>=109, k=i-(pixel?109:94), get=k&1, kind=k/2;
        uint32_t max=kind?16u:pixel?224u:256u, stride=kind==2?4u:16u;
        void *registers=kind==0?(void *)(pixel?draw_state.ps_float:draw_state.vs_float):kind==1?(void *)(pixel?draw_state.ps_int:draw_state.vs_int):(void *)(pixel?draw_state.ps_bool:draw_state.vs_bool);
        if(ARG(1)>max || ARG(3)>max-ARG(1) || (!ARG(2)&&ARG(3))){*hr=D3DERR_INVALIDCALL;break;}
        uint8_t *reg=(uint8_t *)registers+ARG(1)*stride;
        if(get)memcpy(GPTR(ARG(2)),reg,ARG(3)*stride);else{memcpy(reg,GPTR(ARG(2)),ARG(3)*stride);if(i==94){fp_capture_upload(ARG(1),ARG(2),ARG(3));model_capture_upload(ARG(1),ARG(2),ARG(3));}}
        break; }
    case 81: case 82: {
        D3DObj *vb=obj_from_guest(draw_state.stream[0]),*ib=obj_from_guest(draw_state.indices);
        uint32_t n=primitive_vertices(ARG(1),ARG(i==81?3:6));
        if(!vb||vb->kind!=K_VB||!n||!draw_state.stride[0]||draw_state.offset[0]>=vb->size){*hr=D3DERR_INVALIDCALL;break;}
        if(i==81){uint64_t end=(uint64_t)draw_state.offset[0]+((uint64_t)ARG(2)+n)*draw_state.stride[0];
            *hr=end>vb->size?D3DERR_INVALIDCALL:gpu_draw(ARG(1),vb->data+draw_state.offset[0]+ARG(2)*draw_state.stride[0],draw_state.stride[0],n,0,0,0,0,vb);}
        else {unsigned is=ib&&ib->format==101?2:ib&&ib->format==102?4:0;
            if(!is||ib->kind!=K_IB||((uint64_t)ARG(5)+n)*is>ib->size){*hr=D3DERR_INVALIDCALL;break;}
            int64_t first=(int64_t)(int32_t)ARG(2)+ARG(3);
            uint32_t nv=ARG(4);
            if(first<0 || ARG(3)>INT32_MAX || (uint64_t)draw_state.offset[0]+((uint64_t)first+nv)*draw_state.stride[0]>vb->size){*hr=D3DERR_INVALIDCALL;break;}
            *hr=gpu_draw(ARG(1),vb->data+draw_state.offset[0]+(uint32_t)first*draw_state.stride[0],draw_state.stride[0],nv,ib->data+ARG(5)*is,n,ib->format,-(int32_t)ARG(3),vb);}break;}
    case 83: *hr=gpu_draw(ARG(1),ARG(3),ARG(4),primitive_vertices(ARG(1),ARG(2)),0,0,0,0,NULL);binding_replace(&draw_state.stream[0],0);break;
    case 84: {
        uint64_t start=(uint64_t)ARG(7)+(uint64_t)ARG(2)*ARG(8);
        *hr=start>UINT32_MAX||ARG(2)>INT32_MAX?D3DERR_INVALIDCALL:gpu_draw(ARG(1),(uint32_t)start,ARG(8),ARG(3),ARG(5),primitive_vertices(ARG(1),ARG(4)),ARG(6),-(int32_t)ARG(2),NULL);
        binding_replace(&draw_state.stream[0],0);binding_replace(&draw_state.indices,0);break; }
    case 85: {
        if(HOST_ENV("HALO_PROCESS_VERTICES_TRACE")) {
            D3DObj *src=obj_from_guest(draw_state.stream[0]),*dst=obj_from_guest(ARG(4));
            static unsigned logs;
            if(logs++<20)host_log("[process-vertices] frame=%u src=%08x start=%u stride=%u count=%u dest=%08x index=%u fvf=%08x outdecl=%08x flags=%x vs=%08x decl=%08x",
                frames_presented+1,draw_state.stream[0],ARG(1),draw_state.stride[0],ARG(3),ARG(4),ARG(2),dst?dst->fvf:0,ARG(5),ARG(6),draw_state.vs,draw_state.decl);
            if(src && HOST_ENV("HALO_PROCESS_VERTICES_CAPTURE") && (uint64_t)draw_state.offset[0]+((uint64_t)ARG(1)+ARG(3))*draw_state.stride[0]<=src->size)
                capture_draw(0,src->data+draw_state.offset[0]+ARG(1)*draw_state.stride[0],draw_state.stride[0],ARG(3),0,0,0,0);
        }
        unsigned fp_pv=fp_capture_pv_begin(ARG(1),ARG(2),ARG(3),ARG(4),ARG(5),ARG(6));
        unsigned model_pv=model_capture_pv_begin(ARG(1),ARG(2),ARG(3),ARG(4),ARG(5),ARG(6));
        *hr=process_vertices(ARG(1),ARG(2),ARG(3),ARG(4),ARG(5),ARG(6));
        fp_capture_pv_end(fp_pv,ARG(2),ARG(3),ARG(4),*hr);model_capture_pv_end(model_pv,ARG(2),ARG(3),ARG(4),*hr);
        if(*hr!=D3D_OK){static unsigned errors;if(errors++<20)host_log("[process-vertices] failed hr=%08x vs=%08x decl=%08x flags=%x",*hr,draw_state.vs,draw_state.decl,ARG(6));}
        else if(HOST_ENV("HALO_PROCESS_VERTICES_TRACE")){static unsigned done;if(done++<8)host_log("[process-vertices] wrote %u vertices",ARG(3));}
        break;
    }
    default:return 0;
    }
    if(i>=81&&i<=84){
        uint32_t prim=ARG(1),stride=i==83?ARG(4):i==84?ARG(8):draw_state.stride[0];
        uint32_t vertices=i==81?primitive_vertices(prim,ARG(3)):i==82?ARG(4):i==83?primitive_vertices(prim,ARG(2)):ARG(3);
        uint32_t indices=i==82?primitive_vertices(prim,ARG(6)):i==84?primitive_vertices(prim,ARG(4)):0;
        render_trace_record(prim,stride,vertices,indices,*hr);
        fp_capture_result(*hr);model_capture_result(*hr);
    }
    return 1;
}
