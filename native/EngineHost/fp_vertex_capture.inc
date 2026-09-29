/* Opt-in, one selected center004924B0 frame. Diagnostic state is never used
 * to choose a draw, change guest state, or infer a successful render. */
#include "process_vertices.h"
#define FP_CAPTURE_DRAWS 16u
#define FP_CAPTURE_PVS 8u
#define FP_CAPTURE_BYTES (1024u*1024u)
static struct {
    int initialized,enabled,scope,finished;
    const char *dir; uint32_t first,frame,caller,current,method;
    unsigned draws,pvs,uploads,entries,exits,omitted,io_errors;
    char backend[24];
} fp_capture;
void host_panorama_viewmodel(int active){
    fp_capture.scope=active!=0;
    if(fp_capture.frame&&!fp_capture.finished&&frames_presented+1==fp_capture.frame&&capture_panorama_pass()==1){
        if(active)fp_capture.entries++;else fp_capture.exits++;
    }
}
static int fp_capture_selected(void){
    if(!fp_capture.initialized){
        fp_capture.initialized=1;fp_capture.dir=getenv("HALO_FP_CAPTURE");
        const char *from=getenv("HALO_FP_CAPTURE_FROM");
        fp_capture.enabled=fp_capture.dir&&*fp_capture.dir&&(!from||render_diag_number(from,&fp_capture.first));
    }
    uint32_t frame=frames_presented+1;
    if(!fp_capture.enabled||fp_capture.finished||!fp_capture.scope||capture_panorama_pass()!=1||frame<fp_capture.first)return 0;
    if(!fp_capture.frame){fp_capture.frame=frame;fp_capture.entries=1;host_log("[fp-capture] selected frame=%u pass=1 scope=004924b0 capDraws=%u capPV=%u capBytes=%u",frame,FP_CAPTURE_DRAWS,FP_CAPTURE_PVS,FP_CAPTURE_BYTES);}
    return frame==fp_capture.frame;
}
static void fp_capture_write(const char *name,const void *bytes,size_t size){
    if(size>FP_CAPTURE_BYTES){fp_capture.omitted++;return;}
    char path[2048];snprintf(path,sizeof path,"%s/%s",fp_capture.dir,name);
    FILE *f=fopen(path,"wb");if(!f){fp_capture.io_errors++;return;}
    if(fwrite(bytes,1,size,f)!=size)fp_capture.io_errors++;if(fclose(f))fp_capture.io_errors++;
}
static void fp_capture_shader(uint32_t guest){
    D3DObj *o=obj_from_guest(guest);if(!o||(o->kind!=K_VSHADER&&o->kind!=K_PSHADER)||!o->data||o->size>65536)return;
    char name[80];snprintf(name,sizeof name,"%s-%08X.bin",o->kind==K_VSHADER?"vs":"ps",guest);fp_capture_write(name,GPTR(o->data),o->size);
}
static void fp_capture_geometry(unsigned n,const char *kind){
    char name[80];snprintf(name,sizeof name,"draw-%03u.transforms.bin",n);
    float transforms[3][16];memcpy(transforms[0],draw_state.world,64);memcpy(transforms[1],draw_state.view,64);memcpy(transforms[2],draw_state.proj,64);fp_capture_write(name,transforms,sizeof transforms);
    snprintf(name,sizeof name,"draw-%03u.source-projection.bin",n);fp_capture_write(name,GPTR(0x007C13C0u),64);
    /* Source00518B40 packs at most64 nodes into3 float4 rows per node. The
     * actual upload below records the used count; this snapshot includes tail. */
    snprintf(name,sizeof name,"draw-%03u.source-bones.bin",n);fp_capture_write(name,GPTR(0x007C04E0u),64u*3u*16u);
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.geometry.json",fp_capture.dir,n);FILE *f=fopen(path,"w");
    if(!f){fp_capture.io_errors++;return;}
    fprintf(f,"{\"kind\":\"%s\",\"frame\":%u,\"pass\":1,\"scope\":\"004924b0\",\"caller\":%u,\"method\":%u,\"vertexBlend\":%u,\"indexedVertexBlend\":%u,\"streamGuest\":[",kind,fp_capture.frame,fp_capture.caller,fp_capture.method,draw_state.rs[151],draw_state.rs[167]);
    for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.stream[i]);fprintf(f,"],\"offset\":[");
    for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.offset[i]);fprintf(f,"],\"stride\":[");
    for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",draw_state.stride[i]);fprintf(f,"]}\n");if(fclose(f))fp_capture.io_errors++;
    fp_capture_shader(draw_state.vs);fp_capture_shader(draw_state.ps);
}
static void fp_capture_draw(uint32_t prim,uint32_t vertices,uint32_t stride,uint32_t nv,uint32_t indices,uint32_t ni,uint32_t fmt,int32_t base){
    fp_capture.current=0;fp_capture.backend[0]=0;if(!fp_capture_selected())return;
    if(fp_capture.draws>=FP_CAPTURE_DRAWS||!vertices||!stride||!nv||(uint64_t)vertices+(uint64_t)nv*stride>0x100000000ULL||(uint64_t)nv*stride>FP_CAPTURE_BYTES||(indices&&(fmt!=101&&fmt!=102))||(uint64_t)ni*(fmt==101?2u:4u)>FP_CAPTURE_BYTES){fp_capture.omitted++;return;}
    unsigned n=fp_capture.draws++;fp_capture.current=n+1;
    capture_draw_payload(fp_capture.dir,n,0,prim,vertices,stride,nv,indices,ni,fmt,base);fp_capture_geometry(n,"draw");
}
static void fp_capture_backend(const char *name){if(fp_capture.current)snprintf(fp_capture.backend,sizeof fp_capture.backend,"%s",name);}
static void fp_capture_normalized(const uint16_t *indices,uint32_t count){
    if(!fp_capture.current||!indices)return;char name[80];snprintf(name,sizeof name,"draw-%03u.normalized-indices.bin",fp_capture.current-1);fp_capture_write(name,indices,(size_t)count*2);
}
static void fp_capture_clip(const mr_vertex_fixed_clip *v,uint32_t count){
    if(!fp_capture.current)return;char name[80];snprintf(name,sizeof name,"draw-%03u.clip-vertices.bin",fp_capture.current-1);fp_capture_write(name,v,(size_t)count*sizeof *v);
}
static void fp_capture_result(uint32_t result){
    if(!fp_capture.current)return;char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.result.json",fp_capture.dir,fp_capture.current-1);FILE *f=fopen(path,"w");
    if(!f)fp_capture.io_errors++;else{fprintf(f,"{\"backend\":\"%s\",\"hresult\":%u,\"accepted\":%s}\n",fp_capture.backend,result,result==D3D_OK?"true":"false");if(fclose(f))fp_capture.io_errors++;}fp_capture.current=0;
}
static void fp_capture_upload(uint32_t start,uint32_t source,uint32_t count){
    if(start!=29||!count||count>227||!source||!fp_capture_selected())return;
    if(fp_capture.uploads>=8){fp_capture.omitted++;return;}
    unsigned n=fp_capture.uploads++;char name[80];snprintf(name,sizeof name,"bone-upload-%03u.bin",n);fp_capture_write(name,GPTR(source),(size_t)count*16);
    host_log("[fp-capture] boneUpload=%u frame=%u caller=%08x source=%08x start=%u count=%u",n,fp_capture.frame,fp_capture.caller,source,start,count);
}
static unsigned fp_capture_pv_begin(uint32_t start,uint32_t dest_index,uint32_t count,uint32_t dest_guest,uint32_t outdecl,uint32_t flags){
    if(!fp_capture_selected())return 0;
    D3DObj *src=obj_from_guest(draw_state.stream[0]),*dest=obj_from_guest(dest_guest);
    uint64_t bytes=(uint64_t)count*draw_state.stride[0],offset=(uint64_t)draw_state.offset[0]+(uint64_t)start*draw_state.stride[0];
    if(fp_capture.pvs>=FP_CAPTURE_PVS||!src||src->kind!=K_VB||!dest||dest->kind!=K_VB||bytes>FP_CAPTURE_BYTES||offset+bytes>src->size){fp_capture.omitted++;return 0;}
    unsigned n=FP_CAPTURE_DRAWS+fp_capture.pvs++;capture_draw_payload(fp_capture.dir,n,0,0,src->data+(uint32_t)offset,draw_state.stride[0],count,0,0,0,0);fp_capture_geometry(n,"ProcessVertices");
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.pv.json",fp_capture.dir,n);FILE *f=fopen(path,"w");
    if(!f)fp_capture.io_errors++;else{fprintf(f,"{\"start\":%u,\"destIndex\":%u,\"count\":%u,\"destGuest\":%u,\"destFVF\":%u,\"outdecl\":%u,\"flags\":%u}\n",start,dest_index,count,dest_guest,dest->fvf,outdecl,flags);if(fclose(f))fp_capture.io_errors++;}
    return n+1;
}
static void fp_capture_pv_end(unsigned id,uint32_t dest_index,uint32_t count,uint32_t dest_guest,uint32_t result){
    if(!id)return;D3DObj *dest=obj_from_guest(dest_guest);PVLayout layout;
    /* Decode destination FVF even on failed calls; do not modify its bytes. */
    if(dest&&pv_layout(dest->fvf,&layout)){
        uint64_t offset=(uint64_t)dest_index*layout.stride,bytes=(uint64_t)count*layout.stride;
        if(offset+bytes<=dest->size&&bytes<=FP_CAPTURE_BYTES){char name[80];snprintf(name,sizeof name,"draw-%03u.pv-output.bin",id-1);fp_capture_write(name,GPTR(dest->data+(uint32_t)offset),(size_t)bytes);}else fp_capture.omitted++;
    }
    char path[2048];snprintf(path,sizeof path,"%s/draw-%03u.pv-result.json",fp_capture.dir,id-1);FILE *f=fopen(path,"w");
    if(!f)fp_capture.io_errors++;else{fprintf(f,"{\"hresult\":%u,\"accepted\":%s}\n",result,result==D3D_OK?"true":"false");if(fclose(f))fp_capture.io_errors++;}
    host_log("[fp-capture] pv=%u frame=%u hresult=%08x count=%u",id-1,fp_capture.frame,result,count);
}
static void fp_capture_finish(uint32_t status,uint64_t epoch){
    if(!fp_capture.frame||fp_capture.finished)return;fp_capture.finished=1;
    char path[2048];snprintf(path,sizeof path,"%s/fp-frame.json",fp_capture.dir);FILE *f=fopen(path,"w");
    if(!f)fp_capture.io_errors++;else{fprintf(f,"{\"frame\":%u,\"pass\":1,\"scope\":\"004924b0\",\"panoramaStatus\":%u,\"complete\":%s,\"sourceEpoch\":%llu,\"draws\":%u,\"processVertices\":%u,\"boneUploads\":%u,\"scopeEntries\":%u,\"scopeExits\":%u,\"omitted\":%u,\"ioErrors\":%u}\n",fp_capture.frame,status,status==HALO_PANORAMA_COMPLETE?"true":"false",(unsigned long long)epoch,fp_capture.draws,fp_capture.pvs,fp_capture.uploads,fp_capture.entries,fp_capture.exits,fp_capture.omitted,fp_capture.io_errors);if(fclose(f))fp_capture.io_errors++;}
    host_log("[fp-capture] finished frame=%u status=%u epoch=%llu draws=%u PV=%u boneUploads=%u scope=%u/%u omitted=%u ioErrors=%u",fp_capture.frame,status,(unsigned long long)epoch,fp_capture.draws,fp_capture.pvs,fp_capture.uploads,fp_capture.entries,fp_capture.exits,fp_capture.omitted,fp_capture.io_errors);
}
