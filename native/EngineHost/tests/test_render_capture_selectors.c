/* Actual diagnostic helpers, synthetic state only; no engine/GPU launched. */
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
static char directory[]="/private/tmp/halo-render-selectors-XXXXXX";
static const char *names[]={"HALO_RENDER_CAPTURE_FROM","HALO_RENDER_CAPTURE_TO","HALO_RENDER_CAPTURE_PASS","HALO_RENDER_CAPTURE_LIMIT","HALO_RENDER_CAPTURE_VS","HALO_RENDER_CAPTURE_FVF","HALO_PROCESS_VERTICES_CAPTURE"};
static void reset(void){
    memset(&render_capture_selection,0,sizeof render_capture_selection);capture_payload_io_errors=capture_payload_omissions=0;
    for(unsigned i=0;i<sizeof names/sizeof *names;i++)unsetenv(names[i]);setenv("HALO_RENDER_CAPTURE",directory,1);
    DIR *d=opendir(directory);assert(d);struct dirent *e;char path[2048];
    while((e=readdir(d)))if(e->d_name[0]!='.'){snprintf(path,sizeof path,"%s/%s",directory,e->d_name);assert(!unlink(path));}closedir(d);
    frames_presented=1399;panorama_pass=1;render_trace_active=0;
}
static void draw(void){capture_draw(4,0x2000,32,3,0,0,101,0);render_trace_record(4,32,3,0,D3D_OK);}
static size_t read_file(const char *suffix,void *bytes,size_t capacity){char p[2048];snprintf(p,sizeof p,"%s/%s",directory,suffix);FILE *f=fopen(p,"rb");assert(f);size_t n=fread(bytes,1,capacity,f);assert(!ferror(f));fclose(f);return n;}
static void contains(const char *name,const char *value){char text[32768]={0};size_t n=read_file(name,text,sizeof text-1);assert(n&&strstr(text,value));}
int main(void){
    assert(mkdtemp(directory));engine_flat_base=calloc(1,0xA00000);assert(engine_flat_base);render_defaults(640,480);
    obj_count=4;objs[1]=(D3DObj){.guest=0x200,.kind=K_TEXTURE,.width=2,.height=2,.format=21,.levels=2,.level_data={0x3000,0x3100}};S32(0x204,1);
    objs[2]=(D3DObj){.guest=0x300,.kind=K_VSHADER,.data=0x4000,.size=8};S32(0x304,2);S32(0x4000,0xFFFE0101);S32(0x4004,0xffff);
    objs[3]=(D3DObj){.guest=0x380,.kind=K_CUBETEX,.width=2,.height=2,.format=21,.levels=1};S32(0x384,3);
    for(int i=1;i<obj_count;i++)obj_register(&objs[i]);
    draw_state.texture[0]=0x200;draw_state.texture[1]=0x380;draw_state.vs=0x300;
    memset(GPTR(0x2000),0x35,96);memset(GPTR(0x3000),0x61,16);memset(GPTR(0x3100),0xee,4);
    S16(0x69e8d8,1);S32(0x746f9c,0x123456);S32(0x7c048c,0x51f3e0);S16(0x69c67c,2);
    uint8_t *before=malloc(0xA00000);assert(before);memcpy(before,engine_flat_base,0xA00000);HostD3DDrawState state=draw_state;
    reset();unsetenv("HALO_RENDER_CAPTURE");draw();assert(!render_capture_selection.sequence);
    uint32_t number;assert(render_diag_number("4294967295",&number)&&number==UINT32_MAX);
    const char *invalid[]={"","+1","-1"," 1","1 ","1x","4294967296","18446744073709551616"};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++)assert(!render_diag_number(invalid[i],&number));
    for(unsigned field=0;field<4;field++)for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++){
        reset();setenv(names[field],invalid[i],1);draw();assert(!render_capture_selection.enabled&&!render_capture_selection.sequence);
    }
    reset();setenv("HALO_RENDER_CAPTURE_LIMIT","0",1);draw();assert(!render_capture_selection.enabled);
    reset();setenv("HALO_RENDER_CAPTURE_LIMIT","257",1);draw();assert(!render_capture_selection.enabled);
    reset();setenv("HALO_RENDER_CAPTURE_LIMIT","256",1);capture_selection_init();assert(render_capture_selection.enabled&&render_capture_selection.limit==256);
    reset();setenv("HALO_RENDER_CAPTURE_PASS","3",1);draw();assert(!render_capture_selection.enabled);
    reset();setenv("HALO_RENDER_CAPTURE_FROM","1401",1);setenv("HALO_RENDER_CAPTURE_TO","1400",1);draw();assert(!render_capture_selection.enabled);
    reset();setenv("HALO_RENDER_CAPTURE_FROM","1400",1);setenv("HALO_RENDER_CAPTURE_TO","1400",1);setenv("HALO_RENDER_CAPTURE_PASS","1",1);setenv("HALO_RENDER_CAPTURE_LIMIT","2",1);
    frames_presented=1398;draw();frames_presented=1399;panorama_pass=0;draw();panorama_pass=2;draw();assert(!render_capture_selection.eligible&&!render_capture_selection.sequence);
    panorama_pass=1;draw();assert(render_capture_selection.sequence==1&&render_capture_selection.results==1);
    contains("draw-000.json","\"panoramaPass\":1");contains("draw-000.json","\"activeBSP\":1");contains("draw-000.json","\"lowerMipsCaptured\":false");
    contains("draw-000.render-result.json","\"HRESULT\":0");contains("draw-000.tex-00.json","\"levels\":2");
    uint8_t bytes[sizeof light_slots];assert(read_file("draw-000.tex-00.bin",bytes,sizeof bytes)==16&&!memcmp(bytes,GPTR(0x3000),16));
    assert(read_file("draw-000.transforms.bin",bytes,sizeof bytes)==192&&!memcmp(bytes,draw_state.world,64));
    assert(read_file("draw-000.material.bin",bytes,sizeof bytes)==sizeof current_material&&!memcmp(bytes,&current_material,sizeof current_material));
    assert(read_file("draw-000.light-slots.bin",bytes,sizeof bytes)==sizeof light_slots&&!memcmp(bytes,light_slots,sizeof light_slots));
    assert(read_file("vs-00000300.bin",bytes,sizeof bytes)==8&&!memcmp(bytes,GPTR(0x4000),8));
    draw();draw();assert(render_capture_selection.sequence==2&&render_capture_selection.omitted_limit==1);
    frames_presented=1400;draw();assert(render_capture_selection.eligible==3);contains("capture-summary.json","\"selectedOperationCoverageComplete\":false");
    reset();for(unsigned i=0;i<41;i++)draw();assert(render_capture_selection.limit==40&&render_capture_selection.sequence==40&&render_capture_selection.omitted_limit==1);
    reset();uint64_t estimate=capture_payload_estimate(0x2000,32,3,0,0,101);render_capture_selection.payload_bytes=RENDER_CAPTURE_BUDGET-estimate+1;draw();
    assert(!render_capture_selection.sequence&&render_capture_selection.omitted_budget==1);contains("capture-summary.json","\"omittedBudget\":1");
    reset();render_capture_selection.payload_bytes=RENDER_CAPTURE_BUDGET-estimate;draw();assert(render_capture_selection.sequence==1&&render_capture_selection.payload_bytes==RENDER_CAPTURE_BUDGET);
    reset();capture_draw(0,0x2000,32,3,0,0,0,0);render_trace_record(4,32,3,0,D3D_OK);assert(!render_capture_selection.results&&!render_capture_selection.pending);
    capture_draw(4,0x2000,32,3,0,0,101,0);render_trace_record(4,32,3,0,D3DERR_NOTAVAILABLE);contains("draw-001.render-result.json","\"success\":false");
    assert(!memcmp(before,engine_flat_base,0xA00000)&&!memcmp(&state,&draw_state,sizeof state));
    reset();capture_draw(4,0x2000,32,3,0,0,101,0);frames_presented++;
    render_trace_record(4,32,3,0,D3D_OK);assert(!render_capture_selection.results&&render_capture_selection.payload_omissions==1);
    // Deliberate absent directory: failed payload writes must not report coverage.
    reset();setenv("HALO_RENDER_CAPTURE","/private/tmp/halo-render-capture-nonexistent-subdir/absent",1);draw();assert(render_capture_selection.io_errors);
    // Leave one valid fixture for an independent JSON parser to inspect.
    reset();setenv("HALO_RENDER_CAPTURE_FROM","1400",1);setenv("HALO_RENDER_CAPTURE_TO","1400",1);setenv("HALO_RENDER_CAPTURE_PASS","1",1);setenv("HALO_RENDER_CAPTURE_LIMIT","96",1);draw();
    contains("capture-summary.json","\"fullMaterialReplayComplete\":false");
    printf("PASS: production exact frame/pass/limit selectors, 40 default, 128MiB budget, metadata/results, unavailable scopes and read-only state. JSON_DIR=%s\n",directory);
    free(before);free(engine_flat_base);
}
