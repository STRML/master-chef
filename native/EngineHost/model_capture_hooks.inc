/* Observe original normal-world ABI and deferred actor identity. No original
 * function is skipped; exact entry stack guards permit its own reentry. */
#include "model_capture_boundary.h"
static int host_model_capture_dispatch(EngineCPU *cpu,uint32_t address) {
    unsigned kind=address==0x004D6FC0u?0:address==0x00533850u?1:address==0x00533730u?2:3;
    if(kind==3)return 0;
    static uint32_t entry_stack[3];
    uint32_t stack=cpu->gpr[4];
    if(entry_stack[kind]==stack || !getenv("HALO_MODEL_CAPTURE"))return 0;
    HostModelCaptureBoundary b={0};b.caller=G32(stack);b.type=UINT32_MAX;
    if(kind==0){
        b.kind=HOST_MODEL_NORMAL;b.model=cpu->gpr[0];b.nodes=cpu->gpr[1];
        b.object=G32(stack+36);b.flags=G32(stack+44);
        if(b.caller==0x0050F049u && !(b.flags&10u) && b.object!=UINT32_MAX){
            uint32_t arena=G32(0x008603B0u),tags=G32(0x0087BC14u);
            if(arena && arena<=UINT32_MAX-0x38u){
                uint32_t entries=G32(arena+0x34),offset=(b.object&65535u)*12u;
                if(entries && entries<=UINT32_MAX-offset-12u){
                    b.object_pointer=G32(entries+offset+8);
                    if(b.object_pointer && b.object_pointer<=UINT32_MAX-0xB5u){
                        b.type=G8(b.object_pointer+0xB4);b.definition=G32(b.object_pointer);
                    }
                }
            }
            uint32_t offset=(b.model&65535u)*32u;
            if(tags && tags<=UINT32_MAX-offset-24u){
                uint32_t data=G32(tags+offset+0x14);
                if(data && data<=UINT32_MAX-0xBCu)b.node_count=G32(data+0xB8);
            }
        }
    }else{
        b.kind=HOST_MODEL_DEFERRED;b.record=G32(stack+4);
        if(!b.record || b.record>UINT32_MAX-0xA8u)return 0;
        b.flags=G32(b.record);b.object=G32(b.record+4);
        b.nodes=G32(b.record+0x60);b.node_count=G16(b.record+0x64);
    }
    int token=host_model_capture_begin(&b);if(!token)return 0;
    uint32_t previous=entry_stack[kind];entry_stack[kind]=stack;
    engine_dispatch(cpu,address);entry_stack[kind]=previous;
    host_model_capture_end(token,cpu->pc==b.caller?0:cpu->pc);
    return 1;
}
