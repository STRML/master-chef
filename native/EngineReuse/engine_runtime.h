#ifndef HALO_ENGINE_RUNTIME_H
#define HALO_ENGINE_RUNTIME_H
#include <stddef.h>
#include <stdint.h>

typedef struct HaloEngineRuntime HaloEngineRuntime;
typedef struct {
    uint32_t address, byte_count;
    const uint8_t *bytes;
    uint8_t writable;
} HaloEngineRegion;
typedef struct {
    uint32_t registers[8], flags, pc;
    double fp_values[8]; /* ST(0)..ST(7) in stack order, whatever fp_top is */
    uint16_t fp_control, fp_status;
    uint8_t fp_valid, fp_top;
} HaloEngineState;
typedef enum {
    HALO_ENGINE_OK=0, HALO_ENGINE_INVALID=1, HALO_ENGINE_MEMORY=2,
    HALO_ENGINE_BUSY=3, HALO_ENGINE_EXECUTION=4
} HaloEngineStatus;
typedef struct {
    HaloEngineStatus status;
    uint32_t pc;
    char reason[160];
} HaloEngineResult;

/* Copies explicit source/live-state regions. No files, snapshots, emulator,
   network, host executable loading, or hidden runtime initialization. */
HaloEngineRuntime *halo_engine_create(const HaloEngineRegion *regions,size_t count,HaloEngineResult *result);
void halo_engine_destroy(HaloEngineRuntime *runtime);
HaloEngineStatus halo_engine_read(HaloEngineRuntime *runtime,uint32_t address,void *output,size_t count);
HaloEngineStatus halo_engine_write(HaloEngineRuntime *runtime,uint32_t address,const void *input,size_t count);
/* On failure, restores every mutable region and leaves caller state unchanged.
   Calls on one runtime must be serialized by its owner. */
HaloEngineResult halo_engine_invoke(HaloEngineRuntime *runtime,uint32_t entry,HaloEngineState *state);
/* A positive instruction limit bounds original loops; exhaustion rolls back. */
HaloEngineResult halo_engine_invoke_bounded(HaloEngineRuntime *runtime,uint32_t entry,HaloEngineState *state,uint64_t instruction_limit);
#endif
