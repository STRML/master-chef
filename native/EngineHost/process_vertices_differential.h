#ifndef HALO_PROCESS_VERTICES_DIFFERENTIAL_H
#define HALO_PROCESS_VERTICES_DIFFERENTIAL_H
#include "process_vertices_skin.h"
#include <fenv.h>
/* Strict decimal uint32: reject signs, whitespace, hex, trailing text/overflow. */
static inline int pv_differential_parse_frame(const char *value,uint32_t *out) {
    if(!value||!*value||!out)return 0;uint32_t number=0;
    for(const unsigned char *p=(const unsigned char *)value;*p;p++) {
        if(*p<'0'||*p>'9')return 0;unsigned digit=*p-'0';
        if(number>(UINT32_MAX-digit)/10u)return 0;number=number*10u+digit;
    }
    *out=number;return 1;
}
static inline int pv_differential_frame_allowed(uint32_t frame,uint32_t first,int valid) { return valid&&frame>=first; }
#define PV_DIFFERENTIAL_BYTE_CAP (4u*1024u*1024u)
typedef struct {
    unsigned skipped,matched,generic_ok,candidate_ok,environment_restore_failed;
    size_t bytes,first_byte;
    uint32_t generic_word,candidate_word;
} PVDifferentialResult;
/* Caller must match the exact shader tokens first. Generic output and success
 * remain authoritative. Candidate execution cannot leak FP flags/rounding or
 * enabled traps into the original engine. No epsilon changes acceptance. */
static inline int pv_process_differential(const PVProgram *p,const uint8_t *decl,size_t decl_bytes,
                             const PVStream streams[16],uint32_t start,uint32_t count,
                             uint8_t *dest,size_t dest_bytes,uint32_t dest_index,uint32_t fvf,uint32_t flags,
                             PVDifferentialResult *result) {
    PVLayout layout;memset(result,0,sizeof *result);result->first_byte=SIZE_MAX;
    uint8_t *copy=NULL;
    if(!dest||!count||flags!=1||!pv_layout(fvf,&layout)||
       (uint64_t)dest_index*layout.stride>dest_bytes||
       (uint64_t)count*layout.stride>dest_bytes-(uint64_t)dest_index*layout.stride)result->skipped=1;
    else if((uint64_t)count*layout.stride>PV_DIFFERENTIAL_BYTE_CAP)result->skipped=2;
    else {
        result->bytes=(size_t)count*layout.stride;
        uintptr_t first=(uintptr_t)(dest+(size_t)dest_index*layout.stride);
        /* Generic's commit may change an aliased source before the second run. */
        for(unsigned i=0;i<16;i++)if(streams[i].bytes&&streams[i].size) {
            uintptr_t source=(uintptr_t)streams[i].bytes;
            if(first>=source?first-source<streams[i].size:source-first<result->bytes)result->skipped=5;
        }
        if(!result->skipped){copy=malloc(result->bytes);if(!copy)result->skipped=3;else memcpy(copy,(void *)first,result->bytes);}
    }
    int generic_ok=pv_process(p,decl,decl_bytes,streams,start,count,dest,dest_bytes,dest_index,fvf,flags);
    result->generic_ok=generic_ok;
    if(!copy)return generic_ok;
    fenv_t saved;
    if(feholdexcept(&saved)){result->skipped=4;free(copy);return generic_ok;}
    result->candidate_ok=pv_process_execute(p,decl,decl_bytes,streams,start,count,copy,result->bytes,0,fvf,flags,pv_skin_execute);
    const uint8_t *actual=dest+(size_t)dest_index*layout.stride;
    result->matched=generic_ok==result->candidate_ok&&!memcmp(actual,copy,result->bytes);
    if(!result->matched)for(size_t i=0;i<result->bytes;i++)if(actual[i]!=copy[i]) {
        result->first_byte=i;size_t word=i&~(size_t)3;
        if(word+4<=result->bytes){memcpy(&result->generic_word,actual+word,4);memcpy(&result->candidate_word,copy+word,4);}break;
    }
    free(copy);
    result->environment_restore_failed=fesetenv(&saved)!=0;
    return generic_ok;
}
#endif
