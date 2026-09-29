/* Exercise the production collector against deterministic kernel replies,
 * including failures and a non-unit Mach timebase. No device or game assets. */
#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <assert.h>

static kern_return_t telemetry_thread_info(thread_inspect_t, thread_flavor_t, thread_info_t, mach_msg_type_number_t *);
static kern_return_t telemetry_task_info(task_name_t, task_flavor_t, task_info_t, mach_msg_type_number_t *);
static kern_return_t telemetry_timebase(mach_timebase_info_t);
#define proc_pid_rusage telemetry_proc_pid_rusage
#define thread_info telemetry_thread_info
#define task_info telemetry_task_info
#define mach_timebase_info telemetry_timebase
#include "../Sources/EngineVisionRuntime.m"
#undef proc_pid_rusage
#undef thread_info
#undef task_info
#undef mach_timebase_info

int host_core_telemetry = 1;
static int unavailable, timebase_invalid;
static unsigned kernel_queries;
int telemetry_proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer) {
    kernel_queries++;
    assert(pid == getpid() && flavor == RUSAGE_INFO_V6);
    if (unavailable) { errno = EACCES; return -1; }
    struct rusage_info_v6 *r = (struct rusage_info_v6 *)buffer;
    memset(r, 0, sizeof *r);
    r->ri_user_time = 300; r->ri_system_time = 150;
    r->ri_user_ptime = 240; r->ri_system_ptime = 60;
    r->ri_runnable_time = 600;
    r->ri_cycles = 50000; r->ri_instructions = 75000;
    r->ri_pcycles = 40000; r->ri_pinstructions = 60000;
    r->ri_cpu_time_qos_default = 3; r->ri_cpu_time_qos_maintenance = 6;
    r->ri_cpu_time_qos_background = 9; r->ri_cpu_time_qos_utility = 12;
    r->ri_cpu_time_qos_legacy = 15; r->ri_cpu_time_qos_user_initiated = 18;
    r->ri_cpu_time_qos_user_interactive = 21;
    r->ri_energy_nj = 3000; r->ri_penergy_nj = 2000;
    r->ri_diskio_bytesread = 4096; r->ri_pageins = 4;
    r->ri_interrupt_wkups = 5; r->ri_pkg_idle_wkups = 6;
    return 0;
}
static kern_return_t telemetry_timebase(mach_timebase_info_t t) {
    kernel_queries++;
    if (timebase_invalid == 1) return KERN_FAILURE;
    t->numer = 125; t->denom = timebase_invalid == 2 ? 0 : 3; return KERN_SUCCESS;
}
static kern_return_t telemetry_thread_info(thread_inspect_t port, thread_flavor_t flavor,
                                          thread_info_t out, mach_msg_type_number_t *count) {
    kernel_queries++;
    assert(port == 123 && flavor == THREAD_EXTENDED_INFO && *count == THREAD_EXTENDED_INFO_COUNT);
    if (unavailable) return KERN_FAILURE;
    thread_extended_info_t t = (thread_extended_info_t)out;
    memset(t, 0, sizeof *t);
    t->pth_user_time = 7000; t->pth_system_time = 2000;
    t->pth_policy = POLICY_TIMESHARE; t->pth_run_state = TH_STATE_WAITING;
    t->pth_flags = TH_FLAGS_SWAPPED; t->pth_cpu_usage = 850;
    t->pth_curpri = 47; t->pth_priority = 37; t->pth_maxpriority = 63;
    return KERN_SUCCESS;
}
static kern_return_t telemetry_task_info(task_name_t task, task_flavor_t flavor,
                                        task_info_t out, mach_msg_type_number_t *count) {
    kernel_queries++;
    assert(task == mach_task_self() && flavor == TASK_POWER_INFO_V2 && *count == TASK_POWER_INFO_V2_COUNT);
    if (unavailable) return KERN_FAILURE;
    task_power_info_v2_t power = (task_power_info_v2_t)out;
    memset(power, 0, sizeof *power); power->task_pset_switches = 42;
    return KERN_SUCCESS;
}

int main(void) { @autoreleasepool {
    engine_worker_port = 123;
    EngineVisionCoreTelemetry t;
    memset(&t, 0xA5, sizeof t);
    assert(enginevision_core_telemetry(&t) && kernel_queries == 4);
    assert(t.process_available && t.process_rusage_result == 0);
    assert(t.process_cpu_ns == 18750 && t.process_performance_ns == 12500 && t.process_runnable_ns == 25000);
    assert(t.process_cycles == 50000 && t.process_instructions == 75000);
    assert(t.process_performance_cycles == 40000 && t.process_performance_instructions == 60000);
    for (int k = 0; k < 7; k++) assert(t.process_qos_ns[k] == (uint64_t)(k + 1) * 125);
    assert(t.process_energy_nj == 3000 && t.process_performance_energy_nj == 2000);
    assert(t.process_disk_read_bytes == 4096 && t.process_pageins == 4);
    assert(t.process_interrupt_wakeups == 5 && t.process_idle_wakeups == 6);
    assert(t.process_pset_switches_available && t.process_pset_switches == 42);
    assert(t.thread_info_available && t.thread_info_result == KERN_SUCCESS && t.thread_run_ns == 9000);
    assert(t.thread_policy == POLICY_TIMESHARE && t.thread_run_state == TH_STATE_WAITING);
    assert(t.thread_flags == TH_FLAGS_SWAPPED && t.thread_cpu_usage == 850);
    assert(t.thread_priority == 47 && t.thread_base_priority == 37 && t.thread_max_priority == 63);
    assert(t.snapshot_available && t.presents == 0 && !t.counts_available);
    assert(t.process_timebase_result == KERN_SUCCESS);
    for (timebase_invalid = 1; timebase_invalid <= 2; timebase_invalid++) {
        enginevision_core_telemetry(&t);
        assert(!t.process_available && !t.process_rusage_result && t.process_timebase_result == KERN_FAILURE);
        assert(t.process_cpu_ns == 0 && t.thread_info_available);
    }
    timebase_invalid = 0;
    /* Reusing an output cannot retain counters from a previously successful call. */
    unavailable = 1;
    enginevision_core_telemetry(&t);
    assert(!t.process_available && t.process_rusage_result == EACCES && t.process_cycles == 0);
    assert(!t.process_pset_switches_available && t.process_pset_switches == 0);
    assert(!t.thread_info_available && t.thread_info_result == KERN_FAILURE && t.thread_run_ns == 0);
    engine_worker_port = 0;
    enginevision_core_telemetry(&t);
    assert(!t.thread_info_available && t.thread_info_result == KERN_INVALID_ARGUMENT);
    assert(!enginevision_core_telemetry(NULL));
    /* HALO_CORE_TELEMETRY=0 (or before engine_worker decides): no kernel
     * query, a zeroed output, and false, so the report omits the groups. */
    host_core_telemetry = 0;
    engine_worker_port = 123;
    unsigned queries = kernel_queries;
    EngineVisionCoreTelemetry zero;
    memset(&zero, 0, sizeof zero);
    memset(&t, 0xA5, sizeof t);
    assert(!enginevision_core_telemetry(&t) && !memcmp(&t, &zero, sizeof t) && kernel_queries == queries);
    puts("PASS production core collector: V6 mappings, 125/3 timebase, scheduling and unavailable counters; off queries nothing");
} }
