/* ENGINE_DIRECT calls a fixed function entry straight away, with cpu->pc at
 * the entry as engine_dispatch would leave it, and still routes every hooked
 * entry through engine_dispatch so its hook runs. Trace-only hooks go direct
 * unless the chunks are built with ENGINE_TRACE_HOOKS=1. */
#include "engine_cpu.h"
#include <assert.h>
#include <stdio.h>

uint8_t *engine_flat_base;
static unsigned dispatched, called;
static uint32_t dispatched_address, called_pc;
static void engine_dispatch(EngineCPU *cpu, uint32_t address) { (void)cpu; dispatched++; dispatched_address = address; }
#include "engine_hooks.h"
static void sub_00401000(EngineCPU *cpu) { called++; called_pc = cpu->pc; }
static void sub_0050BEA0(EngineCPU *cpu) { (void)cpu; assert(!"a hooked entry must go through engine_dispatch"); }
static void sub_00511F30(EngineCPU *cpu) { called++; called_pc = cpu->pc; }
static void sub_00552DE0(EngineCPU *cpu) { (void)cpu; assert(!"the BSP walk is a hook now"); }

int main(void) {
    EngineCPU c; memset(&c, 0, sizeof c); EngineCPU *cpu = &c;
    cpu->pc = 0x00400000u;
    ENGINE_DIRECT(cpu, 0x00401000u, sub_00401000);
    assert(called == 1 && dispatched == 0 && called_pc == 0x00401000u);
    ENGINE_DIRECT(cpu, 0x0050BEA0u, sub_0050BEA0);          /* the panorama renderer hook */
    assert(dispatched == 1 && dispatched_address == 0x0050BEA0u && called == 1);
    ENGINE_DIRECT(cpu, 0x00449590u, sub_00401000);          /* the native sort override */
    assert(dispatched == 2 && dispatched_address == 0x00449590u);
    ENGINE_DIRECT(cpu, 0x00553920u, sub_00401000);          /* the native visible-surface marking */
    assert(dispatched == 3 && dispatched_address == 0x00553920u && called == 1);
    ENGINE_DIRECT(cpu, 0x00552DE0u, sub_00552DE0);          /* the BSP walk, native when it is dead */
    assert(dispatched == 4 && dispatched_address == 0x00552DE0u);
    ENGINE_DIRECT(cpu, 0x00511F30u, sub_00511F30);          /* HALO_A10_TRACE only */
#if ENGINE_TRACE_HOOKS
    assert(dispatched == 5 && called == 1);
#else
    assert(dispatched == 4 && called == 2 && called_pc == 0x00511F30u);
#endif
    /* Every listed address is hooked, and a neighbour is not. */
#define CHECK_HOOKED(a) assert(engine_hooked(a) && !engine_hooked((a) + 1u));
    ENGINE_HOOK_ADDRESSES(CHECK_HOOKED)
    puts("PASS: direct calls set pc and call the entry; hooked entries go through engine_dispatch");
    return 0;
}
