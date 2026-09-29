/* Production D3D fixed-3D bridge and programmable fog-mode selection.
 * CPU renderer fakes capture submitted vertices; no engine or GPU launch. */
#define HALO_TEST_RADIAL_FOG_SETTING 1 /* this test supplies the setting */
#include "../d3d9.c"
#include <assert.h>

uint8_t *engine_flat_base;
static int radial_enabled;
static unsigned submitted;
static mr_program_state captured;
static mr_vertex_fixed_clip captured_vertices[3];

int halo_settings_radial_fog(void){return radial_enabled;}
void guest_free(uint32_t p){(void)p;abort();}
void mr_buffer_release(void *buffer){(void)buffer;}
int guest_page_free(uint32_t p){(void)p;abort();}
void host_log(const char *format,...){(void)format;}
mr_context *mr_create(int w,int h){(void)w;(void)h;assert(!"unexpected renderer creation");return NULL;}
void mr_destroy(mr_context *c){(void)c;abort();}
int mr_write_framebuffer(mr_context *c,const void *p,size_t n){(void)c;(void)p;(void)n;assert(!"unexpected framebuffer upload");return -1;}
int mr_read_framebuffer(mr_context *c,void *p,size_t n){(void)c;(void)p;(void)n;return -1;}
void mr_set_overlay_target(mr_context *c){(void)c;}
void mr_set_viewport(mr_context *c,int x,int y,int w,int h){(void)c;assert(x==0&&y==0&&w==640&&h==480);}
const char *mr_last_error(void){return "CPU fake";}
uint32_t mr_texture_find_cached(mr_context *c,uint64_t k){(void)c;(void)k;return 0;}
uint32_t mr_texture_create_cached(mr_context *c,uint64_t k,int w,int h,const void *p,size_t n){(void)c;(void)k;(void)w;(void)h;(void)p;(void)n;return 0;}
uint32_t mr_texture_create_cached_nomip(mr_context *c,uint64_t k,int w,int h,const void *p,size_t n){(void)c;(void)k;(void)w;(void)h;(void)p;(void)n;abort();}
uint32_t mr_target_texture(mr_context *c){(void)c;abort();}
size_t halo_texture_bgra_size(uint32_t w,uint32_t h){(void)w;(void)h;abort();}
int halo_texture_decode(uint32_t format,uint32_t w,uint32_t h,const void *src,size_t size,size_t pitch,void *dest,size_t capacity){
    (void)format;(void)w;(void)h;(void)src;(void)size;(void)pitch;(void)dest;(void)capacity;abort();
}
int mr_draw_fixed_clip(mr_context *c,const mr_program_state *s,int prim,const void *vertices,size_t stride,uint32_t nv,const uint16_t *ix,uint32_t ni){
    assert(c==(mr_context *)(uintptr_t)1&&prim==MR_TRIANGLE_LIST&&stride==sizeof captured_vertices[0]&&nv==3&&!ix&&!ni);
    captured=*s;memcpy(captured_vertices,vertices,sizeof captured_vertices);submitted++;return 0;
}

static void render_state_float(unsigned state,float value){memcpy(&draw_state.rs[state],&value,sizeof value);}
static void close_float(float actual,float expected){assert(isfinite(actual)&&fabsf(actual-expected)<2e-6f);}
static void submit(void){
    HostD3DDrawState before=draw_state;unsigned previous=submitted;
    assert(ff3d_draw(4,0x2000,32,3,0,0,101,0)==D3D_OK&&submitted==previous+1);
    assert(!memcmp(&before,&draw_state,sizeof before));
}
/* Row-vector view transform, using the same eye for every bearing. Both
 * camera translation and a nonidentity world transform exercise the bridge. */
static void set_view(float yaw,float pitch){
    const float eye[3]={11,-7,3};
    const float right[3]={-sinf(yaw),cosf(yaw),0};
    const float up[3]={-sinf(pitch)*cosf(yaw),-sinf(pitch)*sinf(yaw),cosf(pitch)};
    const float forward[3]={cosf(pitch)*cosf(yaw),cosf(pitch)*sinf(yaw),sinf(pitch)};
    const float *axis[3]={right,up,forward};
    ff3d_identity(draw_state.view);
    for(unsigned i=0;i<3;i++){
        float translated=0;
        for(unsigned j=0;j<3;j++){draw_state.view[j*4+i]=axis[i][j];translated-=eye[j]*axis[i][j];}
        draw_state.view[12+i]=translated;
    }
}

int main(void){
    engine_flat_base=calloc(1,0x800000);assert(engine_flat_base);
    const uint8_t declaration[]={0,0,0,0,2,0,0,0, 0,0,12,0,2,0,3,0, 0,0,24,0,1,0,5,0, 255,0,0,0,17,0,0,0};
    obj_count=3;objs[1]=(D3DObj){.kind=K_VDECL,.guest=0x200,.data=0x400,.size=sizeof declaration};memcpy(GPTR(0x400),declaration,sizeof declaration);
    objs[2]=(D3DObj){.kind=K_SURFACE,.guest=0x300,.data=0x800,.format=21,.width=640,.height=480,.renderer=(mr_context *)(uintptr_t)1,.gpu_dirty=1};current_rt_guest=0x300;S32(0x204,1);S32(0x304,2);
    for(int i=1;i<obj_count;i++)obj_register(&objs[i]);
    render_defaults(640,480);draw_state.decl=0x200;draw_state.rs[168]=7;draw_state.rs[137]=0;
    /* No texture stages: test actual transforms, fog configuration and
     * submission without unrelated upload dependencies. */
    draw_state.ts[0][1]=1;
    draw_state.world[12]=2;draw_state.world[13]=5;draw_state.world[14]=-4;
    /* After world translation these lie 1, 5 and 20 units from eye. */
    const float vertices[3][8]={{10,-12,7,0,0,1,0,0},{12,-8,7,0,0,1,1,0},{9,-12,27,0,0,1,0,1}};
    memcpy(GPTR(0x2000),vertices,sizeof vertices);
    draw_state.rs[28]=1;draw_state.rs[34]=0xFF90A0B0;draw_state.rs[35]=0;draw_state.rs[140]=3;
    render_state_float(36,2);render_state_float(37,10);
    panorama_pass=0;set_view(0,0);

    /* Default disabled: preserve the existing no-VS renderer behavior. */
    assert(!radial_enabled);submit();assert(!captured.fog_enable);
    radial_enabled=1;panorama_pass=-1;submit();assert(!captured.fog_enable);
    panorama_pass=0;draw_state.rs[28]=0;submit();assert(!captured.fog_enable);
    draw_state.rs[28]=1;draw_state.rs[35]=3;submit();assert(!captured.fog_enable);
    draw_state.rs[35]=0;

    const float bearings[9][2]={{0,0},{-60,0},{60,0},{120,0},{180,0},{-120,0},{0,90},{0,-90},{23,41}};
    float original_z=0,largest_z_change=0;
    for(unsigned b=0;b<9;b++){
        set_view(bearings[b][0]*(float)(M_PI/180),bearings[b][1]*(float)(M_PI/180));panorama_pass=(int)b;
        submit();assert(captured.fog_enable&&captured.fog_color==0xFF90A0B0);
        close_float(captured_vertices[0].fog,1);
        close_float(captured_vertices[1].fog,.625f);
        close_float(captured_vertices[2].fog,0);
        for(unsigned j=0;j<3;j++)assert(captured_vertices[j].color==0xFFFFFFFF&&captured_vertices[j].specular==0);
        if(!b)original_z=captured_vertices[1].z;
        else largest_z_change=fmaxf(largest_z_change,fabsf(captured_vertices[1].z-original_z));
    }
    assert(largest_z_change>4); /* Test bearings really change axial depth. */

    /* Shader paths carry fog through colors/texture coordinates as well as
     * oFog, so RS28 must not suppress their token rewrite. */
    D3DObj shader={.kind=K_VSHADER,.radial_fog_terms=2,.radial_fog_view_plane=1};
    S16(0x007C1424u,0);panorama_pass=0;draw_state.rs[28]=0;
    assert(radial_fog_mode(&shader)==HALO_RADIAL_FOG_DEPTH);
    S16(0x007C1424u,1);assert(radial_fog_mode(&shader)==HALO_RADIAL_FOG_DEPTH);
    S16(0x007C1424u,2);assert(radial_fog_mode(&shader)==HALO_RADIAL_FOG_VIEW_PLANE);
    shader.radial_fog_view_plane=0;assert(radial_fog_mode(&shader)==HALO_RADIAL_FOG_DEPTH);shader.radial_fog_view_plane=1;
    shader.radial_fog_terms=0;assert(!radial_fog_mode(&shader));shader.radial_fog_terms=2;
    panorama_pass=-1;assert(!radial_fog_mode(&shader));panorama_pass=0;
    radial_enabled=0;assert(!radial_fog_mode(&shader));

    free(draw_vertex_scratch);free(engine_flat_base);
    printf("PASS: production fixed-3D bridge, 3 world vertices x 9 bearings, radial fog factors 1/0.625/0; color/state preserved; default-off, flat, FOGENABLE and table-fog gates; programmable depth/type-2/no-fog selection (%u submissions); no GPU.\n",submitted);
    return 0;
}
