/* Exercise the actual upload/cache path, including CPU writes and GPU aliases.
 * The fake Metal cache retains decoded pixels so key changes cannot mask stale
 * payloads. Actual game/Metal regression runs cover the other side of this seam. */
#include "../d3d9.c"
#include <assert.h>
uint8_t *engine_flat_base;
typedef struct {mr_context *renderer;uint64_t key;size_t size;uint8_t bytes[96];} Cached;
static Cached cache[64];static unsigned count,readbacks;static int fail_readback;
static uint8_t gpu_bytes[16];
void host_log(const char *format,...){(void)format;}
uint32_t mr_texture_find_cached(mr_context *c,uint64_t key){
    for(unsigned i=1;i<=count;i++)if(cache[i].renderer==c&&cache[i].key==key)return i;return 0;
}
uint32_t mr_texture_create_cached(mr_context *c,uint64_t key,int w,int h,const void *p,size_t pitch){
    assert(count<63&&w==2&&h==2&&pitch==8);unsigned id=++count;
    cache[id].renderer=c;cache[id].key=key;cache[id].size=16;memcpy(cache[id].bytes,p,16);return id;
}
uint32_t mr_texture_create_cube_cached(mr_context *c,uint64_t key,int size,const void *faces[6],size_t pitch){
    assert(count<63&&size==2&&pitch==8);unsigned id=++count;
    cache[id].renderer=c;cache[id].key=key;cache[id].size=96;
    for(int f=0;f<6;f++)memcpy(cache[id].bytes+16*f,faces[f],16);return id;
}
int mr_read_framebuffer(mr_context *c,void *out,size_t size){
    (void)c;assert(size==16);readbacks++;if(fail_readback)return -1;memcpy(out,gpu_bytes,16);return 0;
}
int main(void){
    engine_flat_base=calloc(1,0x10000);assert(engine_flat_base);obj_count=3;memset(objs,0,sizeof objs);
    mr_context *renderer=(mr_context *)(uintptr_t)1,*other=(mr_context *)(uintptr_t)2;
    D3DObj t={.kind=K_TEXTURE,.guest=0x100,.format=21,.width=2,.height=2,.level_data={0x1000}};
    for(unsigned i=0;i<16;i++)engine_flat_base[0x1000+i]=(uint8_t)(i*7);
    unsigned original=uploaded_texture(renderer,&t);assert(original&&count==1);
    assert(!memcmp(cache[original].bytes,GPTR(0x1000),16));
    assert(uploaded_texture(renderer,&t)==original&&count==1);
    /* Every byte matters, even when the guest address and all metadata stay put. */
    for(unsigned i=0;i<16;i++){
        engine_flat_base[0x1000+i]^=0x5a;unsigned changed=uploaded_texture(renderer,&t);
        assert(changed!=original&&!memcmp(cache[changed].bytes,GPTR(0x1000),16));
        engine_flat_base[0x1000+i]^=0x5a;assert(uploaded_texture(renderer,&t)==original);
    }
    unsigned separate=uploaded_texture(other,&t);assert(separate!=original&&cache[separate].renderer==other);
    /* A texture surface may have newer GPU contents at the same guest address. */
    objs[1].kind=K_SURFACE;objs[1].data=0x1000;objs[1].size=16;objs[1].renderer=renderer;objs[1].gpu_dirty=1;
    memset(gpu_bytes,0xbc,sizeof gpu_bytes);unsigned updated=uploaded_texture(renderer,&t);
    assert(readbacks==1&&!objs[1].gpu_dirty&&updated!=original&&!memcmp(cache[updated].bytes,gpu_bytes,16));
    objs[1].gpu_dirty=1;fail_readback=1;assert(uploaded_texture(renderer,&t)==0&&objs[1].gpu_dirty);fail_readback=0;objs[1].gpu_dirty=0;
    D3DObj cube={.kind=K_CUBETEX,.guest=0x200,.format=21,.width=2,.height=2};
    for(unsigned f=0;f<6;f++){cube.face_data[f][0]=0x2000+f*16;memset(GPTR(cube.face_data[f][0]),(int)f*20,16);}
    unsigned base_cube=uploaded_cube_texture(renderer,&cube);assert(base_cube);
    assert(uploaded_cube_texture(renderer,&cube)==base_cube);
    for(unsigned f=0;f<6;f++){
        uint8_t *p=GPTR(cube.face_data[f][0]);p[15]^=0x33;unsigned change=uploaded_cube_texture(renderer,&cube);
        assert(change!=base_cube&&!memcmp(cache[change].bytes+f*16,p,16));p[15]^=0x33;
        assert(uploaded_cube_texture(renderer,&cube)==base_cube);
    }
    objs[2].kind=K_SURFACE;objs[2].data=cube.face_data[4][0];objs[2].size=16;objs[2].renderer=renderer;objs[2].gpu_dirty=1;
    memset(gpu_bytes,0x71,sizeof gpu_bytes);unsigned gpu_cube=uploaded_cube_texture(renderer,&cube);
    assert(gpu_cube!=base_cube&&readbacks==3&&!memcmp(cache[gpu_cube].bytes+4*16,gpu_bytes,16));
    free(engine_flat_base);puts("PASS: production 2D/cube cache, every-byte writes, per-renderer ownership, GPU aliases, readback failure.");
}
