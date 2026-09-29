#ifndef HOST_GATHER_DIAGNOSTICS_H
#define HOST_GATHER_DIAGNOSTICS_H

#include <stdint.h>

/* Native exact BSP-gather dispatch, controlled by HALO_NATIVE_GATHER=1.
 * Counts are cumulative for enabled dispatch entries, for the process
 * lifetime. Calls includes native and translated fallback entries; internal
 * recursion is not a separate entry. Disabled calls are not counted. Enabled
 * reports the effective switch after the first intercepted gather. */
typedef struct {
    uint64_t calls, native_calls, fallback_calls;
    uint32_t enabled;
} HostGatherDiagnostics;

void host_gather_get_diagnostics(HostGatherDiagnostics *out);

#endif
