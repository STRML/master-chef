#include "engine_runtime.h"
#include "engine_cpu.h"
#include <setjmp.h>

typedef struct {uint32_t address,length;uint8_t*data;uint8_t writable;} OwnedRegion;
struct HaloEngineRuntime {
    OwnedRegion *regions;
    size_t count;
    int busy;
    EngineCPU cpu;
    jmp_buf failure;
    HaloEngineResult result;
};
extern void engine_reuse_entry(EngineCPU*,uint32_t);

static HaloEngineResult result(HaloEngineStatus status,uint32_t pc,const char*reason) {
    HaloEngineResult r={0};r.status=status;r.pc=pc;
    if(reason){strncpy(r.reason,reason,sizeof r.reason-1);r.reason[sizeof r.reason-1]=0;}
    return r;
}
static int compare_regions(const void*a,const void*b) {
    uint32_t aa=((const OwnedRegion*)a)->address,bb=((const OwnedRegion*)b)->address;
    return aa<bb?-1:aa>bb?1:0;
}
static void*address(HaloEngineRuntime*r,uint32_t a,size_t n,int write) {
    if(!r || n>UINT32_MAX || (uint64_t)a+n>UINT64_C(0x100000000))return NULL;
    size_t low=0,high=r->count;
    while(low<high){size_t mid=low+(high-low)/2;OwnedRegion*p=&r->regions[mid];
        if(a<p->address)high=mid;
        else if((uint64_t)a>=(uint64_t)p->address+p->length)low=mid+1;
        else return (uint64_t)a+n<=(uint64_t)p->address+p->length && (!write||p->writable)?p->data+(a-p->address):NULL;
    }
    return NULL;
}
static void*read_address(EngineCPU*c,uint32_t a,size_t n){return address(c->context,a,n,0);}
static void*write_address(EngineCPU*c,uint32_t a,size_t n){return address(c->context,a,n,1);}
static void fail(EngineCPU*c,const char*reason){
    HaloEngineRuntime*r=c->context;r->result=result(HALO_ENGINE_EXECUTION,c->pc,reason);longjmp(r->failure,1);
}
void halo_engine_destroy(HaloEngineRuntime*r){
    if(!r)return;
    for(size_t i=0;i<r->count;i++)free(r->regions[i].data);
    free(r->regions);free(r);
}
HaloEngineRuntime*halo_engine_create(const HaloEngineRegion*regions,size_t count,HaloEngineResult*out){
    if(out)*out=result(HALO_ENGINE_INVALID,0,"invalid region descriptors");
    if(!regions||!count||count>65536)return NULL;
    HaloEngineRuntime*r=calloc(1,sizeof *r);
    if(!r){if(out)*out=result(HALO_ENGINE_MEMORY,0,"runtime allocation");return NULL;}
    r->regions=calloc(count,sizeof *r->regions);
    if(!r->regions){free(r);if(out)*out=result(HALO_ENGINE_MEMORY,0,"region table allocation");return NULL;}
    r->count=count;
    /* Validate the whole layout before reading any supplied bytes. */
    for(size_t i=0;i<count;i++){
        const HaloEngineRegion*p=&regions[i];
        if(!p->bytes||!p->byte_count||p->writable>1||(uint64_t)p->address+p->byte_count>UINT64_C(0x100000000)){
            halo_engine_destroy(r);return NULL;}
        r->regions[i]=(OwnedRegion){p->address,p->byte_count,NULL,p->writable};
    }
    qsort(r->regions,count,sizeof *r->regions,compare_regions);
    for(size_t i=1;i<count;i++)if((uint64_t)r->regions[i-1].address+r->regions[i-1].length>r->regions[i].address){
        halo_engine_destroy(r);return NULL;}
    for(size_t i=0;i<count;i++){
        OwnedRegion*p=&r->regions[i];const uint8_t*input=NULL;
        for(size_t j=0;j<count;j++)if(regions[j].address==p->address){input=regions[j].bytes;break;}
        p->data=malloc(p->length);
        if(!p->data){halo_engine_destroy(r);if(out)*out=result(HALO_ENGINE_MEMORY,0,"region allocation");return NULL;}
        memcpy(p->data,input,p->length);
    }
    if(out)*out=result(HALO_ENGINE_OK,0,NULL);return r;
}
HaloEngineStatus halo_engine_read(HaloEngineRuntime*r,uint32_t a,void*out,size_t n){
    if(!r||!out||!n)return HALO_ENGINE_INVALID;
    if(r->busy)return HALO_ENGINE_BUSY;
    void*p=address(r,a,n,0);if(!p)return HALO_ENGINE_INVALID;memcpy(out,p,n);return HALO_ENGINE_OK;
}
HaloEngineStatus halo_engine_write(HaloEngineRuntime*r,uint32_t a,const void*input,size_t n){
    if(!r||!input||!n)return HALO_ENGINE_INVALID;
    if(r->busy)return HALO_ENGINE_BUSY;
    void*p=address(r,a,n,1);if(!p)return HALO_ENGINE_INVALID;memcpy(p,input,n);return HALO_ENGINE_OK;
}
HaloEngineResult halo_engine_invoke(HaloEngineRuntime*r,uint32_t entry,HaloEngineState*state){
    return halo_engine_invoke_bounded(r,entry,state,UINT64_C(10000000));
}
HaloEngineResult halo_engine_invoke_bounded(HaloEngineRuntime*r,uint32_t entry,HaloEngineState*state,uint64_t instruction_limit){
    if(!r||!state)return result(HALO_ENGINE_INVALID,entry,"missing runtime or CPU state");
    if(!instruction_limit)return result(HALO_ENGINE_INVALID,entry,"instruction limit must be positive");
    if(r->busy)return result(HALO_ENGINE_BUSY,entry,"runtime is already executing");
    if(state->fp_top>7 || (state->fp_control&0x3f)!=0x3f)return result(HALO_ENGINE_INVALID,entry,"invalid x87 state");
    uint8_t**saved=calloc(r->count,sizeof *saved);
    if(!saved)return result(HALO_ENGINE_MEMORY,entry,"transaction allocation");
    for(size_t i=0;i<r->count;i++)if(r->regions[i].writable){
        saved[i]=malloc(r->regions[i].length);
        if(!saved[i]){for(size_t j=0;j<i;j++)free(saved[j]);free(saved);return result(HALO_ENGINE_MEMORY,entry,"transaction region allocation");}
        memcpy(saved[i],r->regions[i].data,r->regions[i].length);
    }
    r->busy=1;r->result=result(HALO_ENGINE_OK,entry,NULL);
    EngineCPU*c=&r->cpu;memset(c,0,sizeof *c);
    memcpy(c->gpr,state->registers,sizeof c->gpr);c->flags=state->flags;c->pc=entry;
    c->fp_control=state->fp_control;c->fp_status=state->fp_status;c->fp_valid=state->fp_valid;c->fp_top=state->fp_top;
    for(unsigned i=0;i<8;i++)c->fp_reg[engine_fp_physical(c,i)]=state->fp_values[i];
    c->address=read_address;c->write_address=write_address;c->failure=fail;c->context=r;
    c->instruction_limit=instruction_limit;
    if(setjmp(r->failure)==0){
        engine_reuse_entry(c,entry);
        memcpy(state->registers,c->gpr,sizeof c->gpr);state->flags=c->flags;state->pc=c->pc;
        for(unsigned i=0;i<8;i++)state->fp_values[i]=c->fp_reg[engine_fp_physical(c,i)];
        state->fp_control=c->fp_control;state->fp_status=c->fp_status;state->fp_valid=c->fp_valid;state->fp_top=c->fp_top;
        r->result.pc=c->pc;
    }else{
        for(size_t i=0;i<r->count;i++)if(saved[i])memcpy(r->regions[i].data,saved[i],r->regions[i].length);
    }
    for(size_t i=0;i<r->count;i++)free(saved[i]);free(saved);r->busy=0;
    return r->result;
}
