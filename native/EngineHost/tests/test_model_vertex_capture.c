/* Production diagnostics, synthetic original ABI, no original game/GPU. */
#include "../d3d9.c"
#include "../model_capture_hooks.inc"
#include <assert.h>
/* This fixture never destroys a live Metal context. */
void mr_destroy(mr_context *context){(void)context;abort();}
/* No resident vertex copy is ever taken here: nothing draws. */
void mr_buffer_release(void *buffer){(void)buffer;abort();}

#include <dirent.h>
#include <unistd.h>
uint8_t *engine_flat_base;
/* This fixture never owns reclaimable guest allocations. */
void guest_free(uint32_t p){(void)p;abort();}
int guest_page_free(uint32_t p){(void)p;abort();}

void host_log(const char *format,...){(void)format;}
static char directory[]="/private/tmp/halo-model-capture-XXXXXX";
static unsigned dispatches;static int escape,nested;
void engine_dispatch(EngineCPU *cpu,uint32_t address){
    dispatches++;assert(!host_model_capture_dispatch(cpu,address)); /* exact-entry guard */
    if(nested){
        int active=model_capture.scope.active;HostModelCaptureBoundary context=model_capture.context;
        EngineCPU other=*cpu;other.gpr[4]=0x12000;nested=0;
        assert(host_model_capture_dispatch(&other,0x00533850));assert(model_capture.scope.active==active);assert(!memcmp(&model_capture.context,&context,sizeof context));
    }
    if(model_capture.scope.active){model_capture_draw(4,0x2000,32,3,0,0,101,0);model_capture_backend("fixed3D");model_capture_result(0);}
    cpu->gpr[0]=0x12345678;cpu->pc=escape?0xDEAD:G32(cpu->gpr[4]);cpu->gpr[4]+=4;
}
static void reset(const char *from){memset(&model_capture,0,sizeof model_capture);memset(&model_inventory,0,sizeof model_inventory);setenv("HALO_MODEL_CAPTURE",directory,1);if(from)setenv("HALO_MODEL_CAPTURE_FROM",from,1);else unsetenv("HALO_MODEL_CAPTURE_FROM");unsetenv("HALO_MODEL_CAPTURE_OBJECT");}
static size_t read_capture(const char *name,void *data,size_t size){char path[2048];snprintf(path,sizeof path,"%s/%s",directory,name);FILE *f=fopen(path,"rb");assert(f);size_t n=fread(data,1,size,f);assert(!ferror(f));fclose(f);return n;}
static EngineCPU normal(uint32_t caller,uint32_t flags){EngineCPU c={0};c.gpr[4]=0x10000;c.gpr[0]=2;c.gpr[1]=0x18000;S32(0x10000,caller);S32(0x10024,0xABCD0001);S32(0x1002C,flags);return c;}
int main(void){
    assert(mkdtemp(directory));engine_flat_base=calloc(1,0xA00000);assert(engine_flat_base);render_defaults(640,480);
    obj_count=5;objs[1]=(D3DObj){.guest=0x200,.kind=K_VDECL,.data=0x400,.size=24};S32(0x204,1);
    const uint8_t decl[]={0,0,0,0,2,0,0,0,1,0,0,0,1,0,5,0,255,0,0,0,17,0,0,0};memcpy(GPTR(0x400),decl,sizeof decl);draw_state.decl=0x200;
    objs[2]=(D3DObj){.guest=0x300,.kind=K_VB,.data=0x2000,.size=96};S32(0x304,2);draw_state.stream[0]=0x300;draw_state.stride[0]=32;
    objs[3]=(D3DObj){.guest=0x380,.kind=K_VB,.data=0x3000,.size=96,.fvf=0x112};S32(0x384,3);
    objs[4]=(D3DObj){.guest=0x480,.kind=K_VB,.data=0x5000,.size=32};S32(0x484,4);draw_state.stream[1]=0x480;draw_state.stride[1]=8;draw_state.offset[1]=8;
    for(int i=1;i<obj_count;i++)obj_register(&objs[i]);
    memset(GPTR(0x2000),0x25,96);memset(GPTR(0x3000),0x42,96);memset(GPTR(0x5000),0x61,32);
    S32(0x008603B0,0x20000);S32(0x20034,0x21000);S32(0x21014,0x22000);S8(0x220B4,0);S32(0x22000,0xFF000004);
    S32(0x0087BC14,0x23000);S32(0x23054,0x24000);S32(0x240B8,2);
    panorama_pass=1;frames_presented=2;reset("3");
    EngineCPU cpu=normal(0x0049266F,0);assert(!host_model_capture_dispatch(&cpu,0x004D6FC0));cpu=normal(0x004926DA,0);assert(!host_model_capture_dispatch(&cpu,0x004D6FC0));
    cpu=normal(0x0050F0BD,2);assert(!host_model_capture_dispatch(&cpu,0x004D6FC0));cpu=normal(0x0050F049,8);assert(!host_model_capture_dispatch(&cpu,0x004D6FC0));
    S8(0x220B4,1);cpu=normal(0x0050F049,0);assert(!host_model_capture_dispatch(&cpu,0x004D6FC0));S8(0x220B4,0);
    cpu=normal(0x0050F049,4);panorama_pass=0;assert(!host_model_capture_dispatch(&cpu,0x004D6FC0));panorama_pass=1;frames_presented=1;assert(!host_model_capture_dispatch(&cpu,0x004D6FC0));frames_presented=2;
    S32(0x12000,0x500002);S32(0x12004,0x16000);S32(0x16004,999);
    HostD3DDrawState before=draw_state;unsigned char *memory=malloc(0xA00000);memcpy(memory,engine_flat_base,0xA00000);nested=1;
    assert(host_model_capture_dispatch(&cpu,0x004D6FC0));assert(cpu.pc==0x0050F049&&cpu.gpr[0]==0x12345678&&cpu.gpr[4]==0x10004);
    assert(model_capture.frame==3&&model_capture.identity.object==0xABCD0001&&model_capture.identity.model==2&&model_capture.identity.node_count==2);
    assert(!model_capture.scope.active&&!model_capture.scope.depth&&model_capture.draws==1);
    assert(!memcmp(memory,engine_flat_base,0xA00000)); /* observer and synthetic callee leave guest bytes unchanged */
    /* Deferred datum association after normal scope ended, and escape cleanup. */
    cpu=(EngineCPU){0};cpu.gpr[4]=0x10000;S32(0x10000,0x500003);S32(0x10004,0x15000);S32(0x15000,0);S32(0x15004,0xABCD0001);S32(0x15060,0x18000);S16(0x15064,2);escape=1;
    assert(host_model_capture_dispatch(&cpu,0x00533850));assert(cpu.pc==0xDEAD&&model_capture.draws==2&&model_capture.escaped==1&&!model_capture.scope.active&&!model_capture.scope.depth);escape=0;
    HostModelCaptureBoundary b={.kind=HOST_MODEL_DEFERRED,.object=0xABCD0001,.nodes=0x18000,.node_count=2};int token=host_model_capture_begin(&b);assert(token&&model_capture_selected());
    unsigned pv=model_capture_pv_begin(0,0,3,0x380,0,1);assert(pv==65);model_capture_pv_end(pv,0,3,0x380,D3DERR_NOTAVAILABLE);
    unsigned char bytes[128];assert(read_capture("draw-064.pv-before.bin",bytes,sizeof bytes)==96&&!memcmp(bytes,GPTR(0x3000),96));assert(read_capture("draw-064.pv-output.bin",bytes,sizeof bytes)==96);
    assert(read_capture("draw-000.stream-01.bin",bytes,sizeof bytes)==24&&!memcmp(bytes,GPTR(0x5008),24));
    float material[17]={0};assert(read_capture("draw-000.material.bin",material,sizeof material)==sizeof material&&!memcmp(material,&current_material,sizeof material));
    model_capture_upload(29,0x007C04E0,6);assert(model_capture.uploads==1);assert(read_capture("bone-upload-000.metadata.bin",bytes,sizeof bytes)==24);
    assert(!memcmp(&before,&draw_state,sizeof before)); /* stub dispatch changes no live renderer state */
    for(unsigned i=0;i<100;i++)model_capture_draw(4,0x2000,32,3,0,0,101,0);assert(model_capture.draws==64&&model_capture.omitted>0);
    unsigned omitted=model_capture.omitted;model_capture_write("oversize.bin",bytes,MODEL_CAPTURE_BYTES+1);assert(model_capture.omitted==omitted+1&&model_capture.reasons[0]);
    size_t budget=model_capture.bytes;model_capture.bytes=64u*1024u*1024u;assert(!model_capture_payload_budget(32,3,0,0));model_capture.bytes=budget;
    unsigned errors=model_capture.io_errors;model_capture_write("missing/subfile.bin",bytes,1);assert(model_capture.io_errors==errors+1);
    host_model_capture_end(token,0);model_capture_finish(HALO_PANORAMA_COMPLETE,987);char json[4096]={0};read_capture("model-frame.json",json,sizeof json-1);assert(strstr(json,"\"publicationComplete\":true")&&strstr(json,"\"coverageComplete\":false")&&strstr(json,"\"sourceEpoch\":987"));
    memset(json,0,sizeof json);read_capture("model-candidates.json",json,sizeof json-1);assert(strstr(json,"\"frame\":3")&&strstr(json,"\"sourceEpoch\":987")&&strstr(json,"\"uniqueRecorded\":1"));
    /* Exact datum opt-in; malformed numbers/defaultoff; deferred-before-selection. */
    reset(NULL);setenv("HALO_MODEL_CAPTURE_OBJECT","5",1);b=(HostModelCaptureBoundary){.kind=HOST_MODEL_NORMAL,.caller=0x0050F049,.object=7,.nodes=0x18000,.node_count=2};assert(!host_model_capture_begin(&b));
    reset("-0");assert(!host_model_capture_begin(&b));reset("bad");assert(!host_model_capture_begin(&b));reset(NULL);unsetenv("HALO_MODEL_CAPTURE");assert(!host_model_capture_begin(&b));
    reset(NULL);b.kind=HOST_MODEL_DEFERRED;assert(!host_model_capture_begin(&b));b.kind=HOST_MODEL_NORMAL;token=host_model_capture_begin(&b);assert(token&&model_capture.reasons[6]==1);host_model_capture_end(token,0);
    HostModelCaptureScope scope={.object=7};b.object=7;int tokens[HOST_MODEL_SCOPE_CAP];for(unsigned i=0;i<HOST_MODEL_SCOPE_CAP;i++)tokens[i]=host_model_capture_scope_begin(&scope,&b);assert(host_model_capture_scope_begin(&scope,&b)==-1&&!scope.active);assert(host_model_capture_scope_end(&scope,-1)&&scope.active);for(unsigned i=HOST_MODEL_SCOPE_CAP;i;i--)assert(host_model_capture_scope_end(&scope,tokens[i-1]));assert(!scope.active&&!scope.depth);
    reset(NULL);model_capture_init();b=(HostModelCaptureBoundary){.kind=HOST_MODEL_NORMAL,.caller=0x0050F049,.nodes=0x18000,.node_count=2,.object_pointer=0x22000};
    for(unsigned i=0;i<65;i++){b.object=i;model_inventory_observe(&b);}assert(model_inventory.used==64&&model_inventory.overflow==1);
    b.object=2;model_inventory_observe(&b);assert(model_inventory.duplicates==1);frames_presented=3;b.object=99;model_inventory_observe(&b);assert(model_inventory.used==64&&model_inventory.submissions==66);frames_presented=2;
    model_inventory_finish(HALO_PANORAMA_COMPLETE,988);assert(model_inventory.finished);memset(json,0,sizeof json);read_capture("model-candidates.json",json,sizeof json-1);assert(strstr(json,"\"uniqueRecorded\":64")&&strstr(json,"\"overflowSubmissions\":1"));
    DIR *dir=opendir(directory);assert(dir);struct dirent *entry;while((entry=readdir(dir))){if(entry->d_name[0]=='.')continue;char path[2048];snprintf(path,sizeof path,"%s/%s",directory,entry->d_name);assert(!unlink(path));}closedir(dir);assert(!rmdir(directory));free(memory);free(engine_flat_base);
    puts("PASS: production actor ABI/world-biped filter, defaultoff/FROM/object gates, same-entry reentry, nested unrelated/deferred identity, guest escape cleanup, pre/postPV and stream slices, material/lights/bones, bounds/omissions, epoch manifest; synthetic CPU only.");
}
