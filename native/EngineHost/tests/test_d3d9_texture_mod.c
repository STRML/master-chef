/* Exercise the real D3D upload boundary and generation changes; GPU calls are
 * recorded doubles. The separate texture upload test exercises real Metal. */
#define main lifetime_fixture_main
#include "test_d3d9_texture_lifetime.c"
#undef main
static uint64_t cached_key;
static unsigned upload_count;
static uint8_t captured[64];
static int fail_upload;
uint32_t mr_texture_find_cached(mr_context *r,uint64_t key){assert(r);return cached_key==key?100:0;}
uint32_t mr_texture_create_cached(mr_context *r,uint64_t key,int w,int h,const void *p,size_t pitch){
    assert(r && w==4 && h==4 && pitch==16);if(fail_upload)return 0;
    memcpy(captured,p,64);cached_key=key;upload_count++;return 100;
}
uint32_t mr_texture_create_cached_nomip(mr_context *r,uint64_t key,int w,int h,const void *p,size_t pitch){return mr_texture_create_cached(r,key,w,h,p,pitch);}
uint32_t mr_target_texture(mr_context *r){(void)r;return 0;}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
int main(void){
    engine_flat_base=mmap(NULL,GUEST_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(engine_flat_base!=MAP_FAILED);
    the_device=obj_new(K_DEVICE);render_defaults(64,64);
    assert(!create_texture(the_device,K_TEXTURE,4,4,1,1,0,21,1,out));
    D3DObj *t=obj_find(G32(out));uint8_t original[64],replacement[64];
    assert(!call(t,19,0,desc,0));assert(t->level_data[0]);
    for(unsigned i=0;i<64;i++){original[i]=(uint8_t)i;replacement[i]=(uint8_t)(255-i);}
    memcpy(GPTR(t->level_data[0]),original,64);
    uint32_t table[256];htm_crc_table(table);uint32_t hash=htm_crc(table,original,64);
    uint8_t pack[128]={0};memcpy(pack,"HVTEX001",8);put32(pack+8,1);put32(pack+12,32);put32(pack+16,32);put32(pack+24,128);
    put32(pack+32,hash);put32(pack+36,4);put32(pack+40,4);put32(pack+44,1);put32(pack+48,64);put32(pack+56,64);memcpy(pack+64,replacement,64);
    char path[]="/tmp/halo-texture-mod-XXXXXX";int fd=mkstemp(path);assert(fd>=0);assert(write(fd,pack,128)==128);close(fd);setenv("HALO_TEXTURE_PACK",path,1);
    mr_context *renderer=(mr_context *)(uintptr_t)1;
    assert(uploaded_texture(renderer,t)==100 && upload_count==1 && !memcmp(captured,replacement,64));
    assert(!memcmp(GPTR(t->level_data[0]),original,64));
    assert(uploaded_texture(renderer,t)==100 && upload_count==1); /* cached match */
    /* A changed guest texture must stop matching and upload its own pixels. */
    ((uint8_t *)GPTR(t->level_data[0]))[0]^=1;t->content_generation++;
    assert(uploaded_texture(renderer,t)==100 && upload_count==2 && captured[0]==(original[0]^1));
    memcpy(GPTR(t->level_data[0]),original,64);t->content_generation++;
    assert(uploaded_texture(renderer,t)==100 && upload_count==3 && !memcmp(captured,replacement,64));
    /* Cache eviction reuploads; resource failure retains the original path. */
    cached_key=0;assert(uploaded_texture(renderer,t)==100 && upload_count==4);
    cached_key=0;fail_upload=1;assert(uploaded_texture_mod(renderer,t,64)==0);fail_upload=0;
    t->usage=1;assert(!uploaded_texture_mod(renderer,t,64));t->usage=0;
    assert(obj_release(t)==0);unlink(path);
    munmap((void *)texture_mod_pack.data,texture_mod_pack.size);free(texture_mod_seen);
    munmap(engine_flat_base,GUEST_SIZE);
    puts("PASS native texture replacement: original pixels preserved, alpha preserved, cache hits, mutation invalidation, eviction and allocation fallback");
}
