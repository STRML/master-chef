#include "texture_mod_pack.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
static HaloTexturePack texture_mod_pack;
static uint32_t texture_mod_crc_table[256];
static uint8_t *texture_mod_seen;
static unsigned texture_mod_hits;
/* Engine-thread-only comparison control. The app keeps the exact native CRC;
 * the diagnostic probe can alternate it with the previous scalar algorithm. */
static int texture_crc_fast=1;
void host_texture_crc_set_fast(int enabled){texture_crc_fast=!!enabled;}
int host_texture_crc_fast_enabled(void){return texture_crc_fast;}
static pthread_once_t texture_mod_once=PTHREAD_ONCE_INIT;
static void texture_mod_initialize(void) {
    const char *enabled=getenv("HALO_TEXTURE_MODS"), *path=getenv("HALO_TEXTURE_PACK");
    if((enabled && !strcmp(enabled,"0")) || !path || !*path)return;
    int fd=open(path,O_RDONLY);struct stat st;
    if(fd<0){host_log("[texture-mod] pack unavailable: %s",path);return;}
    if(fstat(fd,&st) || st.st_size<0 || !htm_file_size_valid((uint64_t)st.st_size)){
        close(fd);host_log("[texture-mod] unsupported pack size; using originals");return;
    }
    void *data=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
    if(data==MAP_FAILED)return;
    if(!htm_open(&texture_mod_pack,data,(size_t)st.st_size)) {
        munmap(data,(size_t)st.st_size);host_log("[texture-mod] invalid pack; using originals");return;
    }
    texture_mod_seen=calloc(texture_mod_pack.count,1);
    htm_crc_table(texture_mod_crc_table);
    host_log("[texture-mod] loaded %u replacements, %llu mapped bytes",texture_mod_pack.count,(unsigned long long)texture_mod_pack.size);
    /* The immutable mapping lives with the renderer process. Pages are demand
     * loaded; replacement uploads use the existing bounded Metal LRU cache. */
}
static uint32_t uploaded_texture_mod(mr_context *renderer,D3DObj *t,uint32_t size) {
    pthread_once(&texture_mod_once,texture_mod_initialize);
    if(!texture_mod_pack.count || (t->usage&1u))return 0;
    if(!t->mod_checked || t->mod_generation!=t->content_generation) {
        uint32_t hash=texture_crc_fast ? htm_crc(texture_mod_crc_table,GPTR(t->level_data[0]),size)
                                     : htm_crc_scalar(texture_mod_crc_table,GPTR(t->level_data[0]),size);
        t->mod_entry=htm_find(&texture_mod_pack,hash);
        t->mod_generation=t->content_generation;t->mod_checked=1;
    }
    if(!t->mod_entry)return 0;
    HaloTextureMod mod=htm_entry(&texture_mod_pack,t->mod_entry);
    uint64_t key=UINT64_C(0x4856545800000000)|mod.hash;
    uint32_t id=mr_texture_find_cached(renderer,key);
    if(!id)id=mr_texture_create_cached(renderer,key,(int)mod.width,(int)mod.height,mod.bgra,(size_t)mod.width*4);
    if(id && texture_mod_seen && !texture_mod_seen[t->mod_entry-1]) {
        texture_mod_seen[t->mod_entry-1]=1;texture_mod_hits++;
        host_log("[texture-mod] matched %08x %ux%u -> %ux%u (%u/%u unique)",mod.hash,t->width,t->height,mod.width,mod.height,texture_mod_hits,texture_mod_pack.count);
    }
    return id; /* Allocation failure falls back to the original, never black. */
}
