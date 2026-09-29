/* Opt-in local diagnostics. Never read as production state. */
/* Draws before the next Present belong to capture frame frames_presented+1. */
static int render_diag_number(const char *value,uint32_t *out) {
    if(!value||!*value||!out)return 0;
    uint32_t parsed=0;
    for(const unsigned char *p=(const unsigned char *)value;*p;p++) {
        if(*p<'0'||*p>'9'||parsed>(UINT32_MAX-(*p-'0'))/10u)return 0;
        parsed=parsed*10u+(*p-'0');
    }
    *out=parsed;return 1;
}
typedef struct {
    uint32_t primitive,stride,fvf,vs,ps,decl,color,rt,viewport[4];
    int panorama_pass;
    uint32_t depth,fog,stencil,scissor,srcblend,dstblend,result;
    char reason[192];
} RenderTraceKey;
static struct {
    uint32_t frame,attempts,successes,color_attempts,color_successes,omitted;
    unsigned used;
    struct {RenderTraceKey key;uint32_t count;uint64_t vertices,indices;} rows[32];
} render_trace;
static int render_trace_active;
static char render_trace_reason[192];
static int render_trace_in_window(void) {
    static int initialized,enabled;static uint32_t first,last,every=1;
    if(!initialized){initialized=1;last=UINT32_MAX;
        const char *from=HOST_ENV("HALO_RENDER_TRACE_FROM"),*to=HOST_ENV("HALO_RENDER_TRACE_TO");
        uint32_t parsed;const char *interval=HOST_ENV("HALO_RENDER_TRACE_EVERY");
        if(interval&&render_diag_number(interval,&parsed)&&parsed)every=parsed;
        if(from){enabled=render_diag_number(from,&first)&&(!to||render_diag_number(to,&last))&&first<=last;
            if(!enabled)host_log("[render-trace] invalid HALO_RENDER_TRACE_FROM/TO; diagnostics disabled");}}
    uint32_t frame=frames_presented+1;
    return enabled&&frame>=first&&frame<=last&&(frame-first)%every==0;
}
static void render_trace_flush(void) {
    if(!render_trace.attempts)return;
    host_log("[render-trace] f=%u attempts=%u successful=%u colorAttempts=%u colorSuccessful=%u unique=%u omittedAttempts=%u",
        render_trace.frame,render_trace.attempts,render_trace.successes,render_trace.color_attempts,
        render_trace.color_successes,render_trace.used,render_trace.omitted);
    for(unsigned i=0;i<render_trace.used;i++){
        const RenderTraceKey *k=&render_trace.rows[i].key;
        host_log("[render-draw] f=%u count=%u prim=%u stride=%u vertices=%llu indices=%llu fvf=%08x vs=%08x ps=%08x decl=%08x cw=%x rt=%08x vp=%u,%u,%u,%u depth=%u fog=%u stencil=%u scissor=%u blend=%u,%u hr=%08x reason=%s pass=%d",
            render_trace.frame,render_trace.rows[i].count,k->primitive,k->stride,
            (unsigned long long)render_trace.rows[i].vertices,(unsigned long long)render_trace.rows[i].indices,
            k->fvf,k->vs,k->ps,k->decl,k->color,k->rt,k->viewport[0],k->viewport[1],k->viewport[2],k->viewport[3],
            k->depth,k->fog,k->stencil,k->scissor,k->srcblend,k->dstblend,k->result,k->reason,k->panorama_pass);
    }
    memset(&render_trace,0,sizeof render_trace);
}
static void render_trace_begin(void) {
    render_trace_active=render_trace_in_window();render_trace_reason[0]=0;
}
static void render_trace_failure(const char *reason) {
    if(render_trace_active&&reason)snprintf(render_trace_reason,sizeof render_trace_reason,"%s",reason);
}
static void capture_draw_result(uint32_t result);
static void render_trace_record(uint32_t primitive,uint32_t stride,uint32_t vertices,uint32_t indices,uint32_t result) {
    capture_draw_result(result);
    if(!render_trace_active)return;
    render_trace_active=0;
    render_trace.frame=frames_presented+1;render_trace.attempts++;
    if(!result)render_trace.successes++;
    if(draw_state.rs[168]){render_trace.color_attempts++;if(!result)render_trace.color_successes++;}
    RenderTraceKey key={0};key.primitive=primitive;key.stride=stride;
    key.fvf=draw_state.fvf;key.vs=draw_state.vs;key.ps=draw_state.ps;key.decl=draw_state.decl;key.color=draw_state.rs[168];
    key.panorama_pass=capture_panorama_pass();
    D3DObj *rt=rt_surface();key.rt=rt?rt->guest:0;memcpy(key.viewport,draw_state.viewport,sizeof key.viewport);
    key.depth=draw_state.rs[7];key.fog=draw_state.rs[28];key.stencil=draw_state.rs[52];key.scissor=draw_state.rs[174];
    key.srcblend=draw_state.rs[27]?draw_state.rs[19]:0;key.dstblend=draw_state.rs[27]?draw_state.rs[20]:0;key.result=result;
    const char *reason=!result?"ok":render_trace_reason[0]?render_trace_reason:
        result==D3DERR_INVALIDCALL?"invalid draw arguments/buffer/index range":
        result==0x8007000Eu?"allocation failed":"surface/texture/device unavailable";
    snprintf(key.reason,sizeof key.reason,"%s",reason);
    unsigned row=0;while(row<render_trace.used&&memcmp(&render_trace.rows[row].key,&key,sizeof key))row++;
    if(row==render_trace.used){if(row==32){render_trace.omitted++;return;}render_trace.rows[row].key=key;render_trace.used++;}
    render_trace.rows[row].count++;render_trace.rows[row].vertices+=vertices;render_trace.rows[row].indices+=indices;
}
typedef struct {
    int initialized,enabled,has_pass,pending;
    const char *dir;
    uint32_t first,last,limit,wanted_pass,sequence,pending_index,pending_frame;
    int pending_pass;
    uint64_t payload_bytes;
    uint32_t eligible,omitted_budget,omitted_limit,io_errors,results,draw_calls,payload_omissions;
} RenderCaptureSelection;
static RenderCaptureSelection render_capture_selection;
enum { RENDER_CAPTURE_BUDGET=128u*1024u*1024u };
static void capture_selection_init(void);
static unsigned capture_payload_io_errors,capture_payload_omissions;
static D3DObj *capture_object(uint32_t guest) {
    if(!guest || guest>UINT32_MAX-8)return NULL;
    D3DObj *o=obj_from_guest(guest);return o&&o->guest==guest?o:NULL;
}
static void capture_close(FILE *f) {int failed=ferror(f);if(fclose(f))failed=1;if(failed)capture_payload_io_errors++;}
static void capture_binary(const char *dir,unsigned n,const char *suffix,const void *bytes,size_t size) {
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.%s",dir,n,suffix);FILE *f=fopen(path,"wb");
    if(!f){capture_payload_io_errors++;return;}if(fwrite(bytes,1,size,f)!=size)capture_payload_io_errors++;capture_close(f);
}
static void capture_bound_shader(const char *dir,D3DObj *o) {
    if(!o || (o->kind!=K_VSHADER&&o->kind!=K_PSHADER) || !o->data || o->size>262144 || (uint64_t)o->data+o->size>0x100000000ULL)return;
    char path[2048];snprintf(path,sizeof path,"%s/%s-%08X.bin",dir,o->kind==K_VSHADER?"vs":"ps",o->guest);
    FILE *f=fopen(path,"wb");if(!f){capture_payload_io_errors++;return;}if(fwrite(GPTR(o->data),1,o->size,f)!=o->size)capture_payload_io_errors++;capture_close(f);
}
static void capture_summary(void) {
    RenderCaptureSelection *c=&render_capture_selection;char path[2048];snprintf(path,sizeof path,"%s/capture-summary.json",c->dir);FILE *f=fopen(path,"w");if(!f){c->io_errors++;return;}
    fprintf(f,"{\"budgetBytes\":%u,\"reservedPayloadBytes\":%llu,\"eligibleCalls\":%u,\"capturedCalls\":%u,\"omittedBudget\":%u,\"omittedLimit\":%u,\"ioErrors\":%u,\"resultSidecars\":%u,\"drawCalls\":%u,\"omittedPayloadRanges\":%u,\"limit\":%u,\"fromFrame\":%u,\"toFrame\":%u,\"selectedOperationCoverageComplete\":%s,\"fullMaterialReplayComplete\":false,\"uncollected\":[\"lower texture mips\",\"cubemap faces\",\"secondary vertex streams\",\"original shader/material identity and caller\",\"ProcessVertices results\",\"depth/stencil and complete occluding frame\"],\"closureScope\":\"Cumulative observed calls only; no engine publication closure hook\"}\n",
        RENDER_CAPTURE_BUDGET,(unsigned long long)c->payload_bytes,c->eligible,c->sequence,c->omitted_budget,c->omitted_limit,c->io_errors,c->results,c->draw_calls,c->payload_omissions,c->limit,c->first,c->last,(!c->omitted_budget&&!c->omitted_limit&&!c->io_errors&&!c->payload_omissions&&c->results==c->draw_calls)?"true":"false");
    int failed=ferror(f);if(fclose(f))failed=1;if(failed)c->io_errors++;
}
static void capture_shader(D3DObj *o) {
    capture_selection_init();RenderCaptureSelection *c=&render_capture_selection;
    if(!c->enabled || frames_presented+1<c->first || frames_presented+1>c->last ||
       (c->has_pass&&capture_panorama_pass()!=(int)c->wanted_pass))return;
    if(o->size>RENDER_CAPTURE_BUDGET-c->payload_bytes)return;
    c->payload_bytes+=o->size;unsigned before=capture_payload_io_errors;
    capture_bound_shader(c->dir,o);c->io_errors+=capture_payload_io_errors-before;
}
static void capture_draw_payload(const char *dir,unsigned n,int with_textures,uint32_t primitive,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t fmt,int32_t base) {
    char path[2048];
    snprintf(path,sizeof path,"%s/draw-%03u.json",dir,n);FILE *f=fopen(path,"w");if(!f){capture_payload_io_errors++;return;}
    fprintf(f,"{\"testOnly\":true,\"frame\":%u,\"primitive\":%u,\"vertices\":%u,\"stride\":%u,\"vertexCount\":%u,\"indexCount\":%u,\"indexFormat\":%u,\"base\":%d,\"fvf\":%u,\"vs\":%u,\"ps\":%u,\"decl\":%u,\"rs\":[",frames_presented+1,primitive,vertices,stride,nv,ni,fmt,base,draw_state.fvf,draw_state.vs,draw_state.ps,draw_state.decl);
    for(int i=0;i<256;i++)fprintf(f,"%s%u",i?",":"",draw_state.rs[i]);fprintf(f,"],\"textures\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.texture[i]);fprintf(f,"],\"streams\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.stream[i]);fprintf(f,"],\"streamOffsets\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.offset[i]);fprintf(f,"],\"streamStrides\":[");
    for(int i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.stride[i]);fprintf(f,"],\"stages\":[");
    for(int j=0;j<8;j++){fprintf(f,"%s[",j?",":"");for(int i=0;i<33;i++)fprintf(f,"%s%u",i?",":"",draw_state.ts[j][i]);fprintf(f,"]");}fprintf(f,"],\"samplers\":[");
    for(int j=0;j<16;j++){fprintf(f,"%s[",j?",":"");for(int i=0;i<14;i++)fprintf(f,"%s%u",i?",":"",draw_state.sampler[j][i]);fprintf(f,"]");}fprintf(f,"],\"panoramaPass\":%d,\"viewportRaw\":[",capture_panorama_pass());
    for(unsigned j=0;j<6;j++)fprintf(f,"%s%u",j?",":"",draw_state.viewport[j]);
    fprintf(f,"],\"activeBSP\":%d,\"bspPointer\":%u,\"materialCallback\":%u,\"renderMode\":%u,\"originalMaterialIdentityAvailable\":false,\"secondaryStreamsCaptured\":false,\"lowerMipsCaptured\":false,\"cubeFacesCaptured\":false,\"boundTextureIdentity\":[",
        (int16_t)G16(0x0069E8D8u),G32(0x00746F9Cu),G32(0x007C048Cu),G16(0x0069C67Cu));
    for(unsigned j=0;j<16;j++) {
        D3DObj *t=capture_object(draw_state.texture[j]);
        fprintf(f,"%s{\"guest\":%u,\"valid\":%s,\"kind\":%d,\"width\":%u,\"height\":%u,\"format\":%u,\"levels\":%u,\"level0Data\":%u,\"payloadScope\":\"Guest CPU 2D level0 only; lower mips/cube faces/GPU aliases unavailable\"}",
            j?",":"",draw_state.texture[j],t?"true":"false",t?t->kind:0,t?t->width:0,t?t->height:0,t?t->format:0,t?t->levels:0,t?t->level_data[0]:0);
    }
    fprintf(f,"],\"drawResultScope\":\"Generic capture uses .render-result.json when available; shared FP/model diagnostics use their existing result sidecars; ProcessVertices result not supplied here\"}\n");capture_close(f);
    float transforms[3][16];memcpy(transforms[0],draw_state.world,64);memcpy(transforms[1],draw_state.view,64);memcpy(transforms[2],draw_state.proj,64);
    capture_binary(dir,n,"transforms.bin",transforms,sizeof transforms);
    capture_binary(dir,n,"material.bin",&current_material,sizeof current_material);
    capture_binary(dir,n,"light-slots.bin",light_slots,sizeof light_slots);
    capture_binary(dir,n,"vs-constants.bin",draw_state.vs_float,sizeof draw_state.vs_float);
    capture_binary(dir,n,"ps-constants.bin",draw_state.ps_float,sizeof draw_state.ps_float);
    if(with_textures){capture_bound_shader(dir,capture_object(draw_state.vs));capture_bound_shader(dir,capture_object(draw_state.ps));}
    for(int st=0;with_textures&&st<16;st++){
        D3DObj *t=capture_object(draw_state.texture[st]);if(!t||t->kind!=K_TEXTURE||!t->level_data[0])continue;
        uint32_t pitch,size=level_size(t->format,t->width,t->height,&pitch);
        snprintf(path,sizeof path,"%s/draw-%03u.tex-%02d.json",dir,n,st);f=fopen(path,"w");if(f){fprintf(f,"{\"guest\":%u,\"kind\":%d,\"levels\":%u,\"level\":0,\"data\":%u,\"width\":%u,\"height\":%u,\"format\":%u,\"pitch\":%u,\"size\":%u,\"payloadEligible\":%s}\n",t->guest,t->kind,t->levels,t->level_data[0],t->width,t->height,t->format,pitch,size,size&&size<=16777216&&(uint64_t)t->level_data[0]+size<=0x100000000ULL?"true":"false");capture_close(f);}else capture_payload_io_errors++;
        if(!size||size>16777216||(uint64_t)t->level_data[0]+size>0x100000000ULL){capture_payload_omissions++;continue;}
        snprintf(path,sizeof path,"%s/draw-%03u.tex-%02d.bin",dir,n,st);f=fopen(path,"wb");if(f){if(fwrite(GPTR(t->level_data[0]),1,size,f)!=size)capture_payload_io_errors++;capture_close(f);}else capture_payload_io_errors++;
    }
    D3DObj *d=capture_object(draw_state.decl);if(d&&d->kind==K_VDECL&&(with_textures||d->size<=4096)&&(uint64_t)d->data+d->size<=0x100000000ULL){snprintf(path,sizeof path,"%s/decl-%08X.bin",dir,d->guest);f=fopen(path,"wb");if(f){if(fwrite(GPTR(d->data),1,d->size,f)!=d->size)capture_payload_io_errors++;capture_close(f);}else capture_payload_io_errors++;}
    if(vertices&&stride&&nv&&((uint64_t)nv*stride)<16777216&&(uint64_t)vertices+(uint64_t)nv*stride<=0x100000000ULL){snprintf(path,sizeof path,"%s/draw-%03u.vertices.bin",dir,n);f=fopen(path,"wb");if(f){if(fwrite(GPTR(vertices),stride,nv,f)!=nv)capture_payload_io_errors++;capture_close(f);}else capture_payload_io_errors++;}
    /* Secondary streams for the same vertex range (lightmap coordinates live in stream 1). */
    if(vertices&&stride&&nv){D3DObj *stream0=capture_object(draw_state.stream[0]);uint32_t first_vertex=0;
        if(stream0&&stream0->kind==K_VB&&stride==draw_state.stride[0]){uint64_t origin=(uint64_t)stream0->data+draw_state.offset[0];if(vertices>=origin&&((uint64_t)vertices-origin)%stride==0)first_vertex=(uint32_t)(((uint64_t)vertices-origin)/stride);}
        for(int sidx=1;sidx<16;sidx++){D3DObj *vb=capture_object(draw_state.stream[sidx]);uint32_t st=draw_state.stride[sidx];if(!vb||vb->kind!=K_VB||!st)continue;
            uint64_t start=(uint64_t)draw_state.offset[sidx]+(uint64_t)first_vertex*st,length=(uint64_t)nv*st;if(start+length>vb->size||length>=16777216)continue;
            snprintf(path,sizeof path,"%s/draw-%03u.stream-%02d.bin",dir,n,sidx);f=fopen(path,"wb");if(f){if(fwrite(GPTR(vb->data+(uint32_t)start),1,(size_t)length,f)!=(size_t)length)capture_payload_io_errors++;capture_close(f);}else capture_payload_io_errors++;}}
    if(indices&&ni&&ni<196609&&(fmt==101||fmt==102)&&(uint64_t)indices+(uint64_t)ni*(fmt==101?2:4)<=0x100000000ULL){snprintf(path,sizeof path,"%s/draw-%03u.indices.bin",dir,n);f=fopen(path,"wb");if(f){if(fwrite(GPTR(indices),fmt==101?2:4,ni,f)!=ni)capture_payload_io_errors++;capture_close(f);}else capture_payload_io_errors++;}
    if(vertices&&stride&&nv&&((uint64_t)nv*stride>=16777216||(uint64_t)vertices+(uint64_t)nv*stride>0x100000000ULL))capture_payload_omissions++;
    if(indices&&ni&&(ni>=196609||(fmt!=101&&fmt!=102)||(uint64_t)indices+(uint64_t)ni*(fmt==101?2:4)>0x100000000ULL))capture_payload_omissions++;

}

static void capture_selection_init(void) {
    RenderCaptureSelection *c=&render_capture_selection;
    if(c->initialized)return;c->initialized=1;c->last=UINT32_MAX;c->limit=40;
    c->dir=HOST_ENV("HALO_RENDER_CAPTURE");if(!c->dir)return;
    const char *from=HOST_ENV("HALO_RENDER_CAPTURE_FROM"),*to=HOST_ENV("HALO_RENDER_CAPTURE_TO"),
        *pass=HOST_ENV("HALO_RENDER_CAPTURE_PASS"),*limit=HOST_ENV("HALO_RENDER_CAPTURE_LIMIT");
    c->enabled=*c->dir&&(!from||render_diag_number(from,&c->first))&&
        (!to||render_diag_number(to,&c->last))&&c->first<=c->last&&
        (!limit||(render_diag_number(limit,&c->limit)&&c->limit>0&&c->limit<=256))&&
        (!pass||(render_diag_number(pass,&c->wanted_pass)&&c->wanted_pass<=2));
    c->has_pass=pass!=NULL;
    if(!c->enabled)host_log("[render-capture] invalid selectors; capture disabled");
}
static void capture_draw_result(uint32_t result) {
    RenderCaptureSelection *c=&render_capture_selection;if(!c->pending)return;c->pending=0;
    if(c->pending_frame!=frames_presented+1 || c->pending_pass!=capture_panorama_pass()) {
        c->payload_omissions++;capture_summary();return;
    }
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.render-result.json",c->dir,c->pending_index);
    FILE *f=fopen(path,"w");if(!f){c->io_errors++;host_log("[render-capture] result write failed draw=%u",c->pending_index);capture_summary();return;}
    fprintf(f,"{\"frame\":%u,\"panoramaPass\":%d,\"resultAvailable\":true,\"HRESULT\":%u,\"success\":%s,\"scope\":\"Bridge draw HRESULT; original material identity/caller and GPU pixel correctness not established\"}\n",c->pending_frame,c->pending_pass,result,result==D3D_OK?"true":"false");
    int failed=ferror(f);if(fclose(f))failed=1;if(failed){c->io_errors++;host_log("[render-capture] result write failed draw=%u",c->pending_index);}else c->results++;capture_summary();
}
/* Conservative reservation includes headers and repeated bound shader files;
 * skipped per-payload ranges still consume their reservation. No disk scan/GPU. */
static uint64_t capture_payload_estimate(uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t fmt) {
    uint64_t total=65536+sizeof current_material+sizeof light_slots+sizeof draw_state.vs_float+sizeof draw_state.ps_float+192;
    if(vertices&&stride&&nv&&(uint64_t)stride*nv<16777216)total+=(uint64_t)stride*nv;
    if(indices&&ni&&ni<196609&&(fmt==101||fmt==102))total+=(uint64_t)ni*(fmt==101?2:4);
    D3DObj *d=capture_object(draw_state.decl);if(d&&d->kind==K_VDECL)total+=d->size;
    D3DObj *vs=capture_object(draw_state.vs),*ps=capture_object(draw_state.ps);if(vs)total+=vs->size;if(ps)total+=ps->size;
    for(unsigned st=0;st<16;st++){D3DObj *t=capture_object(draw_state.texture[st]);if(!t||t->kind!=K_TEXTURE||!t->level_data[0])continue;
        uint32_t pitch,size=level_size(t->format,t->width,t->height,&pitch);if(size&&size<=16777216)total+=size;}
    return total;
}
static void capture_draw(uint32_t primitive,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t fmt,int32_t base) {
    capture_selection_init();RenderCaptureSelection *c=&render_capture_selection;
    /* Previous draw results arrive synchronously at render_trace_record. PV has
     * no result callback here; never associate it with a later draw's result. */
    c->pending=0;
    uint32_t frame=frames_presented+1;int pass=capture_panorama_pass();
    if(!c->enabled||frame<c->first||frame>c->last||
       (c->has_pass&&pass!=(int)c->wanted_pass))return;
    if(HOST_ENV("HALO_PROCESS_VERTICES_CAPTURE") && primitive!=0)return;
    if(HOST_ENV("HALO_RENDER_CAPTURE_VS") && !draw_state.vs)return;
    const char *fvf_filter=HOST_ENV("HALO_RENDER_CAPTURE_FVF");uint32_t wanted_fvf;
    if(fvf_filter&&(!render_diag_number(fvf_filter,&wanted_fvf)||draw_state.fvf!=wanted_fvf))return;
    c->eligible++;
    if(c->sequence>=c->limit){c->omitted_limit++;capture_summary();return;}
    uint64_t bytes=capture_payload_estimate(vertices,stride,nv,indices,ni,fmt);
    if(bytes>RENDER_CAPTURE_BUDGET-c->payload_bytes){c->omitted_budget++;capture_summary();return;}
    c->payload_bytes+=bytes;unsigned before=capture_payload_io_errors,omissions=capture_payload_omissions;
    unsigned n=c->sequence++;if(primitive)c->draw_calls++;capture_draw_payload(c->dir,n,1,primitive,vertices,stride,nv,indices,ni,fmt,base);
    c->io_errors+=capture_payload_io_errors-before;c->payload_omissions+=capture_payload_omissions-omissions;capture_summary();
    if(primitive){c->pending=1;c->pending_index=n;c->pending_frame=frame;c->pending_pass=pass;}
}

#include "fp_vertex_capture.inc"

#include "model_vertex_capture.inc"
