#ifndef HALO_HOST_THREADING_H
#define HALO_HOST_THREADING_H

#include <stdint.h>

enum {
    HOST_WAIT_OBJECT_0 = 0,
    HOST_WAIT_ABANDONED = 0x80,
    HOST_WAIT_TIMEOUT = 0x102,
    HOST_WAIT_FAILED = 0xFFFFFFFFu,
    HOST_STILL_ACTIVE = 259,
    HOST_CREATE_SUSPENDED = 4
};

uint32_t host_event_create(int manual_reset, int initial_state);
int host_event_set(uint32_t handle);
uint32_t host_mutex_create(int initial_owner);
int host_mutex_release(uint32_t handle);
uint32_t host_wait_single(uint32_t handle, uint32_t timeout_ms);
/* Elapsed time/calls for nonzero-timeout waits (even immediate returns), plus
 * critical-section owner contention. Excludes zero-timeout polls and native
 * mutex acquisition before checking a critical section's owner. This is
 * elapsed API time, not exact blocked CPU time. Per-thread counters let the
 * engine sample its own totals at each Present (core_telemetry.h). */
extern _Thread_local uint64_t host_thread_wait_ns, host_thread_waits;
/* The core telemetry switch (native/EngineVision/CORE_TELEMETRY.md). While it
 * is 0 no wait reads a clock or counts, exactly as before the telemetry. The
 * app sets it once, before the engine runs: on unless HALO_CORE_TELEMETRY=0.
 * The standalone host has no report to put it in and leaves it at 0.
 * Written before any guest thread exists; read with relaxed atomics. */
extern int host_core_telemetry;

uint32_t host_thread_create(uint32_t stack_size, uint32_t start_address,
                            uint32_t parameter, uint32_t flags,
                            uint32_t *thread_id);
uint32_t host_thread_resume(uint32_t handle);
int host_thread_terminate(uint32_t handle, uint32_t exit_code);
int host_thread_get_exit_code(uint32_t handle, uint32_t *exit_code);
uint32_t host_current_thread_id(void);
/* For the device report: the qos_class_t the newest guest thread was created
 * with (0 when it had no attributes), and how many have been created. */
uint32_t host_guest_thread_qos(void);
uint32_t host_guest_threads(void);
void host_thread_yield(void);
_Noreturn void host_thread_exit(uint32_t exit_code);

void host_critical_section_initialize(uint32_t guest_address);
void host_critical_section_delete(uint32_t guest_address);
void host_critical_section_enter(uint32_t guest_address);
void host_critical_section_leave(uint32_t guest_address);

#endif
