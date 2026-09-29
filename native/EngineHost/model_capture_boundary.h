#ifndef HALO_MODEL_CAPTURE_BOUNDARY_H
#define HALO_MODEL_CAPTURE_BOUNDARY_H
#include <stdint.h>
enum { HOST_MODEL_NORMAL=1,HOST_MODEL_DEFERRED=2 };
typedef struct {
    uint32_t kind,caller,object,model,nodes,node_count,flags,object_pointer,definition;
    uint32_t record,type;
} HostModelCaptureBoundary;
/* Diagnostic-only duration tokens; they never select a render target. */
int host_model_capture_begin(const HostModelCaptureBoundary *boundary);
void host_model_capture_end(int token,uint32_t escaped_pc);
#endif
