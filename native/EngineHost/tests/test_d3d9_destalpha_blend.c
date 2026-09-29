/* Actual fixed-3D bridge with observed POSITION/NORMAL/TEXCOORD declaration;
 * renderer calls are CPU fakes, never an engine or GPU launch. */
#include "../d3d9.c"
#include <assert.h>
uint8_t *engine_flat_base;
/* This fixture never owns reclaimable guest allocations. */
void guest_free(uint32_t p){(void)p;abort();}
int guest_page_free(uint32_t p){(void)p;abort();}

static unsigned submitted;
static mr_program_state captured;
void host_log(const char *format,...){(void)format;}
mr_context *mr_create(int w,int h){(void)w;(void)h;assert(!"unexpected renderer creation");return NULL;}
int mr_write_framebuffer(mr_context *c,const void *p,size_t n){(void)c;(void)p;(void)n;assert(!"unexpected framebuffer upload");return -1;}
int mr_read_framebuffer(mr_context *c,void *p,size_t n){(void)c;(void)p;(void)n;return -1;}
void mr_set_overlay_target(mr_context *c){(void)c;}
void mr_set_viewport(mr_context *c,int x,int y,int w,int h){(void)c;assert(x==0&&y==0&&w==640&&h==480);}
const char *mr_last_error(void){return "CPU fake";}
uint32_t mr_texture_find_cached(mr_context *c,uint64_t k){(void)c;(void)k;return 0;}
uint32_t mr_texture_create_cached(mr_context *c,uint64_t k,int w,int h,const void *p,size_t n){(void)c;(void)k;(void)w;(void)h;(void)p;(void)n;return 0;}
int mr_draw_fixed_clip(mr_context *c,const mr_program_state *s,int prim,const void *vertices,size_t stride,uint32_t nv,const uint16_t *ix,uint32_t ni){
    const mr_vertex_fixed_clip *v=vertices;
    assert(c==(mr_context *)(uintptr_t)1&&prim==MR_TRIANGLE_LIST&&stride==sizeof *v&&nv==3&&!ix&&!ni);
    assert(v[0].x==.1f&&v[0].y==.2f&&v[0].z==.3f&&v[0].w==1);
    captured=*s;submitted++;return 0;
}
int main(void){
    engine_flat_base=calloc(1,0x10000);assert(engine_flat_base);
    const uint8_t declaration[]={0,0,0,0,2,0,0,0, 0,0,12,0,2,0,3,0, 0,0,24,0,1,0,5,0, 255,0,0,0,17,0,0,0};
    obj_count=3;objs[1]=(D3DObj){.kind=K_VDECL,.guest=0x200,.data=0x400,.size=sizeof declaration};memcpy(GPTR(0x400),declaration,sizeof declaration);
    objs[2]=(D3DObj){.kind=K_SURFACE,.guest=0x300,.data=0x800,.format=21,.width=640,.height=480,.renderer=(mr_context *)(uintptr_t)1,.gpu_dirty=1};current_rt_guest=0x300;S32(0x204,1);S32(0x304,2);
    for(int i=1;i<obj_count;i++)obj_register(&objs[i]);
    render_defaults(640,480);draw_state.decl=0x200;draw_state.rs[27]=1;draw_state.rs[19]=7;draw_state.rs[20]=2;draw_state.rs[171]=1;draw_state.rs[168]=7;draw_state.rs[137]=0;
    /* Texture stage simplification avoids unrelated upload mocks; layout,
     * transforms and the actual blend mapping remain production paths. */
    draw_state.ts[0][1]=1;
    float vertices[3][8]={{.1f,.2f,.3f,0,0,1,0,0},{.4f,.2f,.3f,0,0,1,1,0},{.1f,.5f,.3f,0,0,1,0,1}};memcpy(GPTR(0x2000),vertices,sizeof vertices);
    HostD3DDrawState before=draw_state;
    assert(ff3d_draw(4,0x2000,32,3,0,0,101,0)==D3D_OK&&submitted==1);
    assert(captured.blend==MR_BLEND_DEST_ALPHA_ADD&&captured.color_write_mask==7&&captured.depth_enable&&captured.depth_write&&captured.depth_compare==4);
    assert(!memcmp(&before,&draw_state,sizeof before));
    draw_state.rs[20]=1;assert(ff3d_draw(4,0x2000,32,3,0,0,101,0)==D3DERR_NOTAVAILABLE&&submitted==1);
    draw_state.rs[19]=2;draw_state.rs[20]=2;assert(ff3d_draw(4,0x2000,32,3,0,0,101,0)==D3D_OK&&captured.blend==MR_BLEND_ADD&&submitted==2);
    free(engine_flat_base);puts("PASS: actual fixed-3D bridge accepts observed DESTALPHA/ONE with normal+UV declaration/stride32, forwards RGB mask/depth, preserves live state, rejects neighboring unsupported pair, preserves additive mapping; no GPU.");
}
