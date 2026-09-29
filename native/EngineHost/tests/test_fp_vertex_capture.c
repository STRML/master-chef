/* Actual production capture helpers; temporary local files, no engine/GPU. */
#include "../d3d9.c"
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
static char directory[]="/private/tmp/halo-fp-capture-XXXXXX";
static void reset(const char *from){memset(&fp_capture,0,sizeof fp_capture);setenv("HALO_FP_CAPTURE",directory,1);if(from)setenv("HALO_FP_CAPTURE_FROM",from,1);else unsetenv("HALO_FP_CAPTURE_FROM");}
static size_t read_capture(const char *name,void *data,size_t size){char path[2048];snprintf(path,sizeof path,"%s/%s",directory,name);FILE *f=fopen(path,"rb");assert(f);size_t n=fread(data,1,size,f);assert(!ferror(f));fclose(f);return n;}
int main(void){
    assert(mkdtemp(directory));engine_flat_base=calloc(1,0x800000);assert(engine_flat_base);render_defaults(640,480);
    obj_count=4;objs[1]=(D3DObj){.guest=0x200,.kind=K_VDECL,.data=0x400,.size=8};S32(0x204,1);const uint8_t decl[]={255,0,0,0,17,0,0,0};memcpy(GPTR(0x400),decl,8);draw_state.decl=0x200;
    objs[2]=(D3DObj){.guest=0x300,.kind=K_VB,.data=0x2000,.size=96};S32(0x304,2);draw_state.stream[0]=0x300;draw_state.stride[0]=32;
    objs[3]=(D3DObj){.guest=0x380,.kind=K_VB,.data=0x3000,.size=96,.fvf=0x112};S32(0x384,3);
    for(int i=1;i<obj_count;i++)obj_register(&objs[i]);
    memset(GPTR(0x2000),0x25,96);memset(GPTR(0x3000),0x42,96);draw_state.vs_float[29][0]=.75f;
    reset("3");frames_presented=1;panorama_pass=1;host_panorama_viewmodel(1);assert(!fp_capture_selected());
    frames_presented=2;panorama_pass=0;assert(!fp_capture_selected());panorama_pass=1;host_panorama_viewmodel(0);assert(!fp_capture_selected());host_panorama_viewmodel(1);
    HostD3DDrawState before=draw_state;unsigned char *memory=malloc(0x800000);memcpy(memory,engine_flat_base,0x800000);
    fp_capture.caller=0x0051c2d3;fp_capture_upload(29,0x007c04e0,6);assert(fp_capture.frame==3&&fp_capture.uploads==1);
    fp_capture_draw(4,0x2000,32,3,0,0,101,0);assert(fp_capture.current==1&&fp_capture.draws==1);fp_capture_backend("fixed3D");
    uint16_t ix[]={2,1,0};fp_capture_normalized(ix,3);mr_vertex_fixed_clip clip[3]={{.x=.5f,.w=1}};fp_capture_clip(clip,3);fp_capture_result(0);assert(!fp_capture.current);
    unsigned pv=fp_capture_pv_begin(0,0,3,0x380,0,1);assert(pv==17);fp_capture_pv_end(pv,0,3,0x380,D3DERR_NOTAVAILABLE);
    unsigned char bytes[128];assert(read_capture("draw-016.pv-output.bin",bytes,sizeof bytes)==96&&!memcmp(bytes,GPTR(0x3000),96));
    assert(read_capture("draw-000.normalized-indices.bin",bytes,sizeof bytes)==6&&!memcmp(bytes,ix,6));
    assert(!memcmp(&before,&draw_state,sizeof before)&&!memcmp(memory,engine_flat_base,0x800000));
    for(unsigned i=0;i<100;i++)fp_capture_draw(4,0x2000,32,3,0,0,101,0);assert(fp_capture.draws==16&&fp_capture.omitted==85);
    unsigned omitted=fp_capture.omitted;fp_capture_write("oversize.bin",bytes,FP_CAPTURE_BYTES+1);assert(fp_capture.omitted==omitted+1);
    frames_presented=3;assert(!fp_capture_selected());frames_presented=2;host_panorama_viewmodel(0);assert(fp_capture.exits==1);
    fp_capture_finish(HALO_PANORAMA_COMPLETE,987);assert(fp_capture.finished);char json[2048]={0};read_capture("fp-frame.json",json,sizeof json-1);assert(strstr(json,"\"complete\":true")&&strstr(json,"\"sourceEpoch\":987")&&strstr(json,"\"scopeExits\":1"));
    host_panorama_viewmodel(1);assert(!fp_capture_selected());
    reset("bad");assert(!fp_capture_selected());reset(NULL);unsetenv("HALO_FP_CAPTURE");assert(!fp_capture_selected());
    reset(NULL);host_panorama_viewmodel(1);assert(fp_capture_selected());fp_capture_finish(HALO_PANORAMA_WORLD_INCOMPLETE,988);memset(json,0,sizeof json);read_capture("fp-frame.json",json,sizeof json-1);assert(strstr(json,"\"complete\":false"));
    DIR *dir=opendir(directory);assert(dir);struct dirent *entry;while((entry=readdir(dir))){if(entry->d_name[0]=='.')continue;char path[2048];snprintf(path,sizeof path,"%s/%s",directory,entry->d_name);assert(!unlink(path));}closedir(dir);assert(!rmdir(directory));free(memory);free(engine_flat_base);
    puts("PASS: opt-in/FROM/exact center FP scope/one-frame gates, shared draw payload, raw/normalized/clip/PV output, matrices/bones, unchanged live+guest bytes, caps, COMPLETE epoch and aborted manifest, one-shot closure; no GPU.");
}
