#ifndef HALO_STATEBLOCK_H
#define HALO_STATEBLOCK_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#ifndef HOST_SB_MALLOC
#define HOST_SB_MALLOC malloc
#endif
#ifndef HOST_SB_CALLOC
#define HOST_SB_CALLOC calloc
#endif
#ifndef HOST_SB_FREE
#define HOST_SB_FREE free
#endif

/* Custom blocks: selected bytes are logical setter ranges, never a whole-device
 * snapshot. Resource offsets identify complete uint32_t binding fields. The
 * embedding host owns live bindings separately and transfers their references
 * when applying the resulting snapshot. */
typedef void (*HostSBRef)(void *, uint32_t);
typedef struct HostStateBlock {
    size_t size, resource_count;
    unsigned char *values, *mask;
    const size_t *resources;
    HostSBRef retain, release;
    void *context;
} HostStateBlock;
static uint32_t host_sb_resource(const void *bytes,size_t offset){uint32_t value;memcpy(&value,(const unsigned char *)bytes+offset,4);return value;}
static HostStateBlock *host_sb_new(const void *initial,size_t size,const size_t *resources,size_t count,HostSBRef retain,HostSBRef release,void *context){
    for(size_t i=0;i<count;i++)if(resources[i]>size||size-resources[i]<4)return NULL;
    if(!initial||!size||size>SIZE_MAX/2)return NULL;
    HostStateBlock *b=HOST_SB_CALLOC(1,sizeof *b);if(!b)return NULL;
    b->values=HOST_SB_MALLOC(size*2);if(!b->values){HOST_SB_FREE(b);return NULL;}
    b->size=size;b->mask=b->values+size;b->resources=resources;b->resource_count=count;
    b->retain=retain;b->release=release;b->context=context;
    memcpy(b->values,initial,size);memset(b->mask,0,size);return b;
}
static void host_sb_replace_ref(HostStateBlock *b,size_t offset,uint32_t next){
    uint32_t old=b->mask[offset]?host_sb_resource(b->values,offset):0;
    if(old==next)return;
    if(next&&b->retain)b->retain(b->context,next);
    if(old&&b->release)b->release(b->context,old);
}
static int host_sb_write(HostStateBlock *b,size_t offset,const void *value,size_t size){
    if(!b||offset>b->size||size>b->size-offset||(!value&&size))return 0;
    /* Reject partial binding writes before any mutation/reference operation. */
    for(size_t i=0;i<b->resource_count;i++){
        size_t r=b->resources[i];
        if(size&&offset<r+4&&r<offset+size&&(offset>r||offset+size<r+4))return 0;
    }
    for(size_t i=0;i<b->resource_count;i++){
        size_t r=b->resources[i];
        if(size&&offset<=r&&r+4<=offset+size)host_sb_replace_ref(b,r,host_sb_resource(value,r-offset));
    }
    if(size){memcpy(b->values+offset,value,size);memset(b->mask+offset,1,size);}return 1;
}
static void host_sb_capture(HostStateBlock *b,const void *live){
    for(size_t i=0;i<b->resource_count;i++){size_t r=b->resources[i];if(b->mask[r])host_sb_replace_ref(b,r,host_sb_resource(live,r));}
    for(size_t i=0;i<b->size;i++)if(b->mask[i])b->values[i]=((const unsigned char *)live)[i];
}
static void host_sb_apply(const HostStateBlock *b,void *live){
    for(size_t i=0;i<b->size;i++)if(b->mask[i])((unsigned char *)live)[i]=b->values[i];
}
static void host_sb_free(HostStateBlock *b){
    if(!b)return;
    for(size_t i=0;i<b->resource_count;i++){size_t r=b->resources[i];uint32_t value=b->mask[r]?host_sb_resource(b->values,r):0;if(value&&b->release)b->release(b->context,value);}
    HOST_SB_FREE(b->values);HOST_SB_FREE(b);
}
#endif
