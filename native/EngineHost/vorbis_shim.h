#ifndef HALO_VORBIS_SHIM_H
#define HALO_VORBIS_SHIM_H
#include "host.h"
void host_vorbis_open_callbacks(EngineCPU *cpu);
void host_vorbis_read(EngineCPU *cpu);
void host_vorbis_crosslap(EngineCPU *cpu);
void host_vorbis_clear(EngineCPU *cpu);
#endif
