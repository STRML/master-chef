#include "shader_mod_pack.h"
#include <sys/stat.h>
static HaloShaderPack shader_mod_pack;
static pthread_once_t shader_mod_once=PTHREAD_ONCE_INIT;
static uint8_t shader_mod_seen[HSM_MAX_ENTRIES];
static void shader_mod_initialize(void) {
    const char *path=getenv("HALO_SHADER_PACK"),*enabled=getenv("HALO_SHADER_MODS");
    if(!path || !*path || (enabled && !strcmp(enabled,"0")))return;
    FILE *f=fopen(path,"rb");struct stat st;
    if(!f)return;
    if(fstat(fileno(f),&st) || st.st_size<16 || st.st_size>HSM_MAX_BYTES){fclose(f);return;}
    size_t size=(size_t)st.st_size;uint8_t *data=malloc(size);
    int ok=data && fread(data,1,size,f)==size;fclose(f);
    if(!ok || !hsm_open(&shader_mod_pack,data,size)) {
        free(data);memset(&shader_mod_pack,0,sizeof shader_mod_pack);
        host_log("[shader-mod] invalid pack; using originals");return;
    }
    /* Immutable bytes live for this renderer process; lookup happens only
     * when a shader is created, never for every draw or panorama bearing. */
    host_log("[shader-mod] loaded %u exact-byte replacements (%zu bytes)",shader_mod_pack.count,size);
}
static const HaloShaderMod *shader_mod_lookup(const void *original,uint32_t size) {
    pthread_once(&shader_mod_once,shader_mod_initialize);
    const HaloShaderMod *mod=hsm_find(&shader_mod_pack,original,size);
    if(mod) {
        size_t index=(size_t)(mod-shader_mod_pack.entries);
        if(!shader_mod_seen[index]) {
            shader_mod_seen[index]=1;
            host_log("[shader-mod] applied entry %zu: %u -> %u bytes",index,mod->original_size,mod->replacement_size);
        }
    }
    return mod;
}
