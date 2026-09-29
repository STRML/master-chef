/* Default-off, one center-pass actor frame. Scope tokens identify submissions
 * only; they never choose render targets or change original guest state. */
#include "process_vertices.h"
#define MODEL_CAPTURE_DRAWS 64u
#define MODEL_CAPTURE_PVS 16u
#define MODEL_CAPTURE_BYTES (1024u*1024u)
#include "model_capture_boundary.h"
#include "model_capture_scope.h"
static struct {
    int initialized,enabled,finished;
    const char *dir; uint32_t first,frame,caller,current,method,wanted; int object_filter;
    unsigned draws,pvs,uploads,omitted,io_errors,escaped; size_t bytes;
    unsigned reasons[8],early_count;uint32_t early_frame,early_objects[32];
    HostModelCaptureScope scope;
    HostModelCaptureBoundary identity,context,overflow_context,previous_context[HOST_MODEL_SCOPE_CAP];
    char backend[24];
} model_capture;
static void model_capture_omit(unsigned reason){
    model_capture.omitted++;if(reason<8)model_capture.reasons[reason]++;
    if(model_capture.omitted<=16)host_log("[model-capture] omission frame=%u reason=%u",model_capture.frame,reason);
}
static int model_capture_number(const char *text,uint32_t *out){
    if(!text||!*text)return 0;for(const char *p=text;*p;p++)if(*p<'0'||*p>'9')return 0;
    return render_diag_number(text,out);
}
static void model_capture_init(void){
    if(model_capture.initialized)return;model_capture.initialized=1;
    model_capture.dir=getenv("HALO_MODEL_CAPTURE");
    const char *from=getenv("HALO_MODEL_CAPTURE_FROM"),*object=getenv("HALO_MODEL_CAPTURE_OBJECT");
    model_capture.object_filter=object!=NULL;
    model_capture.enabled=model_capture.dir&&*model_capture.dir&&(!from||model_capture_number(from,&model_capture.first))&&(!object||model_capture_number(object,&model_capture.wanted));
}
static int model_capture_selected(void){
    return model_capture.enabled&&!model_capture.finished&&model_capture.scope.active&&model_capture.frame==frames_presented+1&&capture_panorama_pass()==1;
}
/* One bounded candidate frame; independent of requested OBJECT matching. */
static struct {
    uint32_t frame;unsigned used,submissions,duplicates,overflow,invalid_roots,io_errors;int finished;
    struct {HostModelCaptureBoundary identity;float root[13],origin[3];int root_valid,origin_valid;} rows[64];
} model_inventory;
static void model_inventory_observe(const HostModelCaptureBoundary *b){
    if(model_inventory.finished||!host_model_capture_eligible(b))return;
    uint32_t frame=frames_presented+1;if(!model_inventory.frame)model_inventory.frame=frame;
    if(frame!=model_inventory.frame)return;model_inventory.submissions++;
    for(unsigned i=0;i<model_inventory.used;i++)if(model_inventory.rows[i].identity.object==b->object){model_inventory.duplicates++;return;}
    if(model_inventory.used==64){model_inventory.overflow++;if(model_inventory.overflow<=4)host_log("[model-inventory] frame=%u candidate cap64 exceeded",frame);return;}
    unsigned n=model_inventory.used++;model_inventory.rows[n].identity=*b;
    if(b->nodes&&(uint64_t)b->nodes+52u<=0x100000000ULL){
        memcpy(model_inventory.rows[n].root,GPTR(b->nodes),52);model_inventory.rows[n].root_valid=1;
        for(unsigned i=0;i<13;i++)if(!isfinite(model_inventory.rows[n].root[i]))model_inventory.rows[n].root_valid=0;
    }
    if(!model_inventory.rows[n].root_valid)model_inventory.invalid_roots++;
    /* Original source object position field also observed by MenuInputProbe. */
    if(b->object_pointer&&(uint64_t)b->object_pointer+0x68u<=0x100000000ULL){
        memcpy(model_inventory.rows[n].origin,GPTR(b->object_pointer+0x5Cu),12);model_inventory.rows[n].origin_valid=1;
        for(unsigned i=0;i<3;i++)if(!isfinite(model_inventory.rows[n].origin[i]))model_inventory.rows[n].origin_valid=0;
    }
}
static void model_inventory_finish(uint32_t status,uint64_t epoch){
    if(!model_inventory.frame||model_inventory.finished)return;model_inventory.finished=1;
    char path[2048];snprintf(path,sizeof path,"%s/model-candidates.json",model_capture.dir);FILE *f=fopen(path,"w");
    if(!f){model_inventory.io_errors++;host_log("[model-inventory] write failed frame=%u ioErrors=%u",model_inventory.frame,model_inventory.io_errors);return;}
    fprintf(f,"{\"testOnly\":true,\"frame\":%u,\"pass\":1,\"sourceEpoch\":%llu,\"panoramaStatus\":%u,\"publicationComplete\":%s,\"candidateCap\":64,\"submissions\":%u,\"uniqueRecorded\":%u,\"duplicates\":%u,\"overflowSubmissions\":%u,\"invalidRoots\":%u,\"candidates\":[",model_inventory.frame,(unsigned long long)epoch,status,status==HALO_PANORAMA_COMPLETE?"true":"false",model_inventory.submissions,model_inventory.used,model_inventory.duplicates,model_inventory.overflow,model_inventory.invalid_roots);
    for(unsigned n=0;n<model_inventory.used;n++){
        const HostModelCaptureBoundary *b=&model_inventory.rows[n].identity;
        fprintf(f,"%s{\"object\":%u,\"model\":%u,\"definition\":%u,\"objectPointer\":%u,\"nodeCount\":%u,\"caller\":%u,\"flags\":%u,\"root\":",n?",":"",b->object,b->model,b->definition,b->object_pointer,b->node_count,b->caller,b->flags);
        if(!model_inventory.rows[n].root_valid)fprintf(f,"null");else{fprintf(f,"[");for(unsigned i=0;i<13;i++)fprintf(f,"%s%.9g",i?",":"",model_inventory.rows[n].root[i]);fprintf(f,"]");}
        fprintf(f,",\"objectOrigin\":");if(!model_inventory.rows[n].origin_valid)fprintf(f,"null");else{fprintf(f,"[");for(unsigned i=0;i<3;i++)fprintf(f,"%s%.9g",i?",":"",model_inventory.rows[n].origin[i]);fprintf(f,"]");}fprintf(f,"}");
    }
    fprintf(f,"]}\n");if(ferror(f))model_inventory.io_errors++;if(fclose(f))model_inventory.io_errors++;
    host_log("[model-inventory] finished frame=%u epoch=%llu candidates=%u submissions=%u duplicates=%u overflow=%u invalidRoots=%u ioErrors=%u",model_inventory.frame,(unsigned long long)epoch,model_inventory.used,model_inventory.submissions,model_inventory.duplicates,model_inventory.overflow,model_inventory.invalid_roots,model_inventory.io_errors);
}
static void model_capture_write(const char *,const void *,size_t);
int host_model_capture_begin(const HostModelCaptureBoundary *b){
    model_capture_init();if(!model_capture.enabled||model_capture.finished||capture_panorama_pass()!=1||frames_presented+1<model_capture.first)return 0;
    model_inventory_observe(b);
    if(!model_capture.frame){
        if(model_capture.early_frame!=frames_presented+1){model_capture.early_frame=frames_presented+1;model_capture.early_count=0;}
        if(b->kind==HOST_MODEL_DEFERRED){if(model_capture.early_count<32)model_capture.early_objects[model_capture.early_count++]=b->object;else model_capture.early_count=33;return 0;}
        if(!host_model_capture_eligible(b)||(model_capture.object_filter&&b->object!=model_capture.wanted))return 0;
        model_capture.frame=frames_presented+1;model_capture.identity=*b;model_capture.scope.object=b->object;
        if(model_capture.early_count>32)model_capture_omit(6);
        for(unsigned i=0;i<model_capture.early_count&&i<32;i++)if(model_capture.early_objects[i]==b->object)model_capture_omit(6);
        model_capture_write("actor-identity.bin",b,sizeof *b);
        host_log("[model-capture] selected frame=%u object=%08x model=%08x normalCaller=%08x",model_capture.frame,b->object,b->model,b->caller);
        if(b->node_count&&b->node_count<=64&&b->nodes&&(uint64_t)b->nodes+b->node_count*52u<=0x100000000ULL)model_capture_write("actor-world-nodes.bin",GPTR(b->nodes),b->node_count*52u);else model_capture_omit(3);
    }
    if(model_capture.frame!=frames_presented+1)return 0;
    if(model_capture.scope.depth==HOST_MODEL_SCOPE_CAP&&!model_capture.scope.overflow)model_capture.overflow_context=model_capture.context;
    if(model_capture.scope.depth<HOST_MODEL_SCOPE_CAP)model_capture.previous_context[model_capture.scope.depth]=model_capture.context;
    int token=host_model_capture_scope_begin(&model_capture.scope,b);
    if(token<0)model_capture_omit(4);
    if(b->kind==HOST_MODEL_DEFERRED&&b->object==model_capture.identity.object&&(b->flags&0x82u))model_capture_omit(7);
    model_capture.context=*b;
    if(model_capture.scope.active&&b->kind==HOST_MODEL_DEFERRED){
        if(b->node_count&&b->node_count<=64&&b->nodes&&(uint64_t)b->nodes+b->node_count*52u<=0x100000000ULL){
            char name[80];snprintf(name,sizeof name,"deferred-%03u.world-nodes.bin",model_capture.scope.deferred_matches-1);model_capture_write(name,GPTR(b->nodes),b->node_count*52u);
        }else model_capture_omit(3);
    }
    return token;
}
void host_model_capture_end(int token,uint32_t escaped_pc){
    if(escaped_pc){model_capture.escaped++;model_capture_omit(5);}
    if(!host_model_capture_scope_end(&model_capture.scope,token))model_capture_omit(4);
    else if(token>0)model_capture.context=model_capture.previous_context[token-1];
    else if(token==-1&&!model_capture.scope.overflow)model_capture.context=model_capture.overflow_context;
}
static void model_capture_write(const char *name,const void *bytes,size_t size){
    if(size>MODEL_CAPTURE_BYTES||model_capture.bytes+size>64u*1024u*1024u){model_capture_omit(0);return;}model_capture.bytes+=size;
    char path[2048];snprintf(path,sizeof path,"%s/%s",model_capture.dir,name);
    FILE *f=fopen(path,"wb");if(!f){model_capture.io_errors++;return;}
    if(fwrite(bytes,1,size,f)!=size)model_capture.io_errors++;if(ferror(f))model_capture.io_errors++;if(fclose(f))model_capture.io_errors++;
}
static void model_capture_shader(uint32_t guest){
    D3DObj *o=obj_from_guest(guest);if(!guest)return;if(!o||(o->kind!=K_VSHADER&&o->kind!=K_PSHADER)||!o->data||o->size>65536){model_capture_omit(2);return;}
    char name[80];snprintf(name,sizeof name,"%s-%08X.bin",o->kind==K_VSHADER?"vs":"ps",guest);model_capture_write(name,GPTR(o->data),o->size);
}
static void model_capture_geometry(unsigned n,const char *kind){
    char name[80];snprintf(name,sizeof name,"draw-%03u.transforms.bin",n);
    float transforms[3][16];memcpy(transforms[0],draw_state.world,64);memcpy(transforms[1],draw_state.view,64);memcpy(transforms[2],draw_state.proj,64);model_capture_write(name,transforms,sizeof transforms);
    snprintf(name,sizeof name,"draw-%03u.source-projection.bin",n);model_capture_write(name,GPTR(0x007C13C0u),64);
    /* Source00518B40 packs at most64 nodes into3 float4 rows per node. The
     * actual upload below records the used count; this snapshot includes tail. */
    snprintf(name,sizeof name,"draw-%03u.source-bones.bin",n);model_capture_write(name,GPTR(0x007C04E0u),64u*3u*16u);
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.geometry.json",model_capture.dir,n);FILE *f=fopen(path,"w");
    if(!f){model_capture.io_errors++;return;}
    fprintf(f,"{\"kind\":\"%s\",\"frame\":%u,\"pass\":1,\"scope\":\"world-biped\",\"caller\":%u,\"method\":%u,\"vertexBlend\":%u,\"indexedVertexBlend\":%u,\"streamGuest\":[",kind,model_capture.frame,model_capture.caller,model_capture.method,draw_state.rs[151],draw_state.rs[167]);
    for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.stream[i]);fprintf(f,"],\"offset\":[");
    for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.offset[i]);fprintf(f,"],\"stride\":[");
    for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.stride[i]);fprintf(f,"],\"omissionReasonNames\":[\"byteBudget\",\"rangeOrOperationCap\",\"unsupportedPayload\",\"nodeSnapshot\",\"scopeBalanceOrOverflow\",\"guestEscape\",\"deferredBeforeSelection\",\"selectedDeferredExcludedFlags\"]}\n");if(ferror(f))model_capture.io_errors++;if(fclose(f))model_capture.io_errors++;
    snprintf(name,sizeof name,"draw-%03u.material.bin",n);model_capture_write(name,&current_material,sizeof current_material);
    snprintf(name,sizeof name,"draw-%03u.light-slots.bin",n);model_capture_write(name,light_slots,sizeof light_slots);
    snprintf(name,sizeof name,"draw-%03u.viewport.bin",n);model_capture_write(name,draw_state.viewport,sizeof draw_state.viewport);
    snprintf(name,sizeof name,"draw-%03u.actor-context.bin",n);model_capture_write(name,&model_capture.context,sizeof model_capture.context);
    model_capture_shader(draw_state.vs);model_capture_shader(draw_state.ps);
}
static int model_capture_payload_budget(uint32_t stride,uint32_t nv,uint32_t ni,uint32_t fmt){
    size_t size=(size_t)stride*nv+(size_t)ni*(fmt==101?2u:4u)+32768u;
    if(model_capture.bytes+size>64u*1024u*1024u){model_capture_omit(0);return 0;}
    model_capture.bytes+=size;return 1;
}
static void model_capture_streams(unsigned n,uint32_t first,uint32_t count){
    D3DObj *d=obj_from_guest(draw_state.decl);if(!d||d->kind!=K_VDECL||!d->data||d->size>4096)return;
    unsigned used=0;const uint8_t *decl=GPTR(d->data);
    for(unsigned offset=0;offset+8<=d->size;offset+=8){
        const uint8_t *e=decl+offset;uint16_t stream;memcpy(&stream,e,2);if(stream==255)break;
        if(stream>=16||!pv_type_bytes(e[4])){model_capture_omit(2);continue;}
        if(!stream||(used&(1u<<stream)))continue;used|=1u<<stream;
        D3DObj *src=obj_from_guest(draw_state.stream[stream]);uint32_t stride=draw_state.stride[stream];
        uint64_t off=(uint64_t)draw_state.offset[stream]+(uint64_t)first*stride,bytes=(uint64_t)count*stride;
        if(!src||src->kind!=K_VB||!stride||off+bytes>src->size||(uint64_t)src->data+off+bytes>0x100000000ULL||bytes>MODEL_CAPTURE_BYTES){model_capture_omit(1);continue;}
        char name[80];snprintf(name,sizeof name,"draw-%03u.stream-%02u.bin",n,stream);model_capture_write(name,GPTR(src->data+(uint32_t)off),(size_t)bytes);
    }
}
static void model_capture_verify(unsigned n,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t fmt){
    const char *suffix[]={"json","vs-constants.bin","ps-constants.bin","vertices.bin","indices.bin"};
    size_t expected[]={1,sizeof draw_state.vs_float,sizeof draw_state.ps_float,(size_t)stride*nv,(size_t)ni*(fmt==101?2u:4u)};
    for(unsigned i=0;i<5;i++){
        if((i==3&&!vertices)||(i==4&&(!indices||!ni)))continue;
        char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.%s",model_capture.dir,n,suffix[i]);FILE *f=fopen(path,"rb");
        if(!f){model_capture.io_errors++;continue;}if(fseek(f,0,SEEK_END)||ftell(f)<(long)expected[i])model_capture.io_errors++;
        if(i==0){char ending[2];if(fseek(f,-2,SEEK_END)||fread(ending,1,2,f)!=2||memcmp(ending,"}\n",2))model_capture.io_errors++;}
        fclose(f);
    }
    D3DObj *d=obj_from_guest(draw_state.decl);if(d&&d->kind==K_VDECL){
        if(d->size>4096){model_capture_omit(2);return;}
        char name[80];snprintf(name,sizeof name,"decl-%08X.bin",d->guest);model_capture_write(name,GPTR(d->data),d->size);
    }
}
static void model_capture_draw(uint32_t prim,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t fmt,int32_t base){
    model_capture.current=0;model_capture.backend[0]=0;if(!model_capture_selected())return;
    if(model_capture.draws>=MODEL_CAPTURE_DRAWS||!vertices||!stride||!nv||(uint64_t)vertices+(uint64_t)nv*stride>0x100000000ULL||(uint64_t)nv*stride>MODEL_CAPTURE_BYTES||(indices&&(fmt!=101&&fmt!=102))||(indices&&ni>=196609)||(indices&&(uint64_t)indices+(uint64_t)ni*(fmt==101?2u:4u)>0x100000000ULL)||(uint64_t)ni*(fmt==101?2u:4u)>MODEL_CAPTURE_BYTES){model_capture_omit(1);return;}
    if(!model_capture_payload_budget(stride,nv,ni,fmt))return;
    unsigned n=model_capture.draws++;model_capture.current=n+1;
    capture_draw_payload(model_capture.dir,n,0,prim,vertices,stride,nv,indices,ni,fmt,base);model_capture_verify(n,vertices,stride,nv,indices,ni,fmt);model_capture_geometry(n,"draw");
    D3DObj *src=obj_from_guest(draw_state.stream[0]);
    if(src&&(uint64_t)vertices>=(uint64_t)src->data+draw_state.offset[0]&&draw_state.stride[0]&&!( (vertices-src->data-draw_state.offset[0])%draw_state.stride[0]))model_capture_streams(n,(vertices-src->data-draw_state.offset[0])/draw_state.stride[0],nv);
    else {D3DObj *d=obj_from_guest(draw_state.decl);if(d&&d->kind==K_VDECL&&d->size<=4096){const uint8_t *e=GPTR(d->data);for(unsigned i=0;i+8<=d->size;i+=8){uint16_t stream;memcpy(&stream,e+i,2);if(stream==255)break;if(stream){model_capture_omit(2);break;}}}}
}
static void model_capture_backend(const char *name){if(model_capture.current)snprintf(model_capture.backend,sizeof model_capture.backend,"%s",name);}
static void model_capture_normalized(const uint16_t *indices,uint32_t count){
    if(!model_capture.current||!indices)return;char name[80];snprintf(name,sizeof name,"draw-%03u.normalized-indices.bin",model_capture.current-1);model_capture_write(name,indices,(size_t)count*2);
}
static void model_capture_clip(const mr_vertex_fixed_clip *v,uint32_t count){
    if(!model_capture.current)return;char name[80];snprintf(name,sizeof name,"draw-%03u.clip-vertices.bin",model_capture.current-1);model_capture_write(name,v,(size_t)count*sizeof *v);
}
static void model_capture_result(uint32_t result){
    if(!model_capture.current)return;char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.result.json",model_capture.dir,model_capture.current-1);FILE *f=fopen(path,"w");
    if(!f)model_capture.io_errors++;else{fprintf(f,"{\"backend\":\"%s\",\"hresult\":%u,\"accepted\":%s}\n",model_capture.backend,result,result==D3D_OK?"true":"false");if(ferror(f))model_capture.io_errors++;if(fclose(f))model_capture.io_errors++;}model_capture.current=0;
}
static void model_capture_upload(uint32_t start,uint32_t source,uint32_t count){
    if(start!=29||!model_capture_selected())return;
    if(!count||count>227||!source||(uint64_t)source+count*16u>0x100000000ULL){model_capture_omit(1);return;}
    if(model_capture.uploads>=32){model_capture_omit(1);return;}
    unsigned n=model_capture.uploads++;char name[80];
    uint32_t metadata[]={start,source,count,model_capture.caller,model_capture.context.object,model_capture.context.kind};
    snprintf(name,sizeof name,"bone-upload-%03u.metadata.bin",n);model_capture_write(name,metadata,sizeof metadata);snprintf(name,sizeof name,"bone-upload-%03u.bin",n);model_capture_write(name,GPTR(source),(size_t)count*16);
    host_log("[model-capture] boneUpload=%u frame=%u caller=%08x source=%08x start=%u count=%u",n,model_capture.frame,model_capture.caller,source,start,count);
}
static void model_capture_pv_dest(unsigned n,const char *suffix,uint32_t dest_index,uint32_t count,D3DObj *dest){
    PVLayout layout;if(!dest||!pv_layout(dest->fvf,&layout)){model_capture_omit(2);return;}
    uint64_t offset=(uint64_t)dest_index*layout.stride,bytes=(uint64_t)count*layout.stride;
    if(offset+bytes>dest->size||(uint64_t)dest->data+offset+bytes>0x100000000ULL||bytes>MODEL_CAPTURE_BYTES){model_capture_omit(1);return;}
    char name[80];snprintf(name,sizeof name,"draw-%03u.%s.bin",n,suffix);model_capture_write(name,GPTR(dest->data+(uint32_t)offset),(size_t)bytes);
}
static unsigned model_capture_pv_begin(uint32_t start,uint32_t dest_index,uint32_t count,uint32_t dest_guest,uint32_t outdecl,uint32_t flags){
    if(!model_capture_selected())return 0;
    D3DObj *src=obj_from_guest(draw_state.stream[0]),*dest=obj_from_guest(dest_guest);
    uint64_t bytes=(uint64_t)count*draw_state.stride[0],offset=(uint64_t)draw_state.offset[0]+(uint64_t)start*draw_state.stride[0];
    if(model_capture.pvs>=MODEL_CAPTURE_PVS||!count||!draw_state.stride[0]||!src||src->kind!=K_VB||!dest||dest->kind!=K_VB||bytes>MODEL_CAPTURE_BYTES||offset+bytes>src->size||(uint64_t)src->data+offset+bytes>0x100000000ULL){model_capture_omit(1);return 0;}
    if(!model_capture_payload_budget(draw_state.stride[0],count,0,0))return 0;
    unsigned n=MODEL_CAPTURE_DRAWS+model_capture.pvs++;model_capture_pv_dest(n,"pv-before",dest_index,count,dest);capture_draw_payload(model_capture.dir,n,0,0,src->data+(uint32_t)offset,draw_state.stride[0],count,0,0,0,0);model_capture_verify(n,src->data+(uint32_t)offset,draw_state.stride[0],count,0,0,0);model_capture_geometry(n,"ProcessVertices");model_capture_streams(n,start,count);
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.pv.json",model_capture.dir,n);FILE *f=fopen(path,"w");
    if(!f)model_capture.io_errors++;else{fprintf(f,"{\"start\":%u,\"destIndex\":%u,\"count\":%u,\"destGuest\":%u,\"destFVF\":%u,\"outdecl\":%u,\"flags\":%u}\n",start,dest_index,count,dest_guest,dest->fvf,outdecl,flags);if(ferror(f))model_capture.io_errors++;if(fclose(f))model_capture.io_errors++;}
    return n+1;
}
static void model_capture_pv_end(unsigned id,uint32_t dest_index,uint32_t count,uint32_t dest_guest,uint32_t result){
    if(!id)return;D3DObj *dest=obj_from_guest(dest_guest);
    /* Capture failed calls too, without modifying destination bytes. */
    model_capture_pv_dest(id-1,"pv-output",dest_index,count,dest);
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.pv-result.json",model_capture.dir,id-1);FILE *f=fopen(path,"w");
    if(!f)model_capture.io_errors++;else{fprintf(f,"{\"hresult\":%u,\"accepted\":%s}\n",result,result==D3D_OK?"true":"false");if(ferror(f))model_capture.io_errors++;if(fclose(f))model_capture.io_errors++;}
    host_log("[model-capture] pv=%u frame=%u hresult=%08x count=%u",id-1,model_capture.frame,result,count);
}
static void model_capture_finish(uint32_t status,uint64_t epoch){
    model_inventory_finish(status,epoch);
    if(!model_capture.frame||model_capture.finished)return;model_capture.finished=1;
    if(model_capture.scope.depth||model_capture.scope.overflow)model_capture_omit(4);
    char path[2048];snprintf(path,sizeof path,"%s/model-frame.json",model_capture.dir);FILE *f=fopen(path,"w");
    if(!f)model_capture.io_errors++;else{
        fprintf(f,"{\"testOnly\":true,\"frame\":%u,\"pass\":1,\"sourceEpoch\":%llu,\"panoramaStatus\":%u,\"publicationComplete\":%s,\"coverageComplete\":%s,\"object\":%u,\"model\":%u,\"definition\":%u,\"nodeCount\":%u,\"normalCaller\":%u,\"draws\":%u,\"processVertices\":%u,\"boneUploads\":%u,\"scopeEntries\":%u,\"scopeExits\":%u,\"deferredMatches\":%u,\"deferredOther\":%u,\"escapedScopes\":%u,\"omitted\":%u,\"ioErrors\":%u,\"omissionReasons\":[",model_capture.frame,(unsigned long long)epoch,status,status==HALO_PANORAMA_COMPLETE?"true":"false",!model_capture.omitted&&!model_capture.io_errors?"true":"false",model_capture.identity.object,model_capture.identity.model,model_capture.identity.definition,model_capture.identity.node_count,model_capture.identity.caller,model_capture.draws,model_capture.pvs,model_capture.uploads,model_capture.scope.entries,model_capture.scope.exits,model_capture.scope.deferred_matches,model_capture.scope.deferred_other,model_capture.escaped,model_capture.omitted,model_capture.io_errors);
        for(unsigned i=0;i<8;i++)fprintf(f,"%s%u",i?",":"",model_capture.reasons[i]);fprintf(f,"],\"omissionReasonNames\":[\"byteBudget\",\"rangeOrOperationCap\",\"unsupportedPayload\",\"nodeSnapshot\",\"scopeBalanceOrOverflow\",\"guestEscape\",\"deferredBeforeSelection\",\"selectedDeferredExcludedFlags\"]}\n");if(ferror(f))model_capture.io_errors++;if(fclose(f))model_capture.io_errors++;
    }
    host_log("[model-capture] finished frame=%u object=%08x draws=%u PV=%u uploads=%u deferred=%u/%u omitted=%u ioErrors=%u",model_capture.frame,model_capture.identity.object,model_capture.draws,model_capture.pvs,model_capture.uploads,model_capture.scope.deferred_matches,model_capture.scope.deferred_other,model_capture.omitted,model_capture.io_errors);
}
