#!/usr/bin/env python3
"""Compile and link telemetry APIs for xrOS; never run or install the binary."""
from pathlib import Path
import re
import subprocess
import tempfile


PROBE = r"""
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* The xrOS SDK exports this symbol and the rusage structures, but omits
 * libproc.h. Keep this strong: a weak import would not establish that the
 * target SDK resolves the symbol used by the runtime's optional call. */
extern int proc_pid_rusage(int pid, int flavor, rusage_info_t *buffer);

_Static_assert(RUSAGE_INFO_V6 == 6, "rusage flavor");
static volatile uint64_t telemetry_result;

int main(void) {
    struct rusage_info_v6 usage = {0};
    int result = proc_pid_rusage(getpid(), RUSAGE_INFO_V6,
                               (rusage_info_t *)&usage);
    telemetry_result = usage.ri_user_time + usage.ri_system_time
        + usage.ri_user_ptime + usage.ri_system_ptime
        + usage.ri_cycles + usage.ri_instructions
        + usage.ri_pcycles + usage.ri_pinstructions
        + usage.ri_runnable_time
        + usage.ri_cpu_time_qos_default + usage.ri_cpu_time_qos_maintenance
        + usage.ri_cpu_time_qos_background + usage.ri_cpu_time_qos_utility
        + usage.ri_cpu_time_qos_legacy + usage.ri_cpu_time_qos_user_initiated
        + usage.ri_cpu_time_qos_user_interactive
        + usage.ri_energy_nj + usage.ri_penergy_nj
        + usage.ri_diskio_bytesread + usage.ri_pageins
        + usage.ri_interrupt_wkups + usage.ri_pkg_idle_wkups;

    thread_t port = mach_thread_self();
    thread_basic_info_data_t basic = {0};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    result |= thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&basic, &count);
    telemetry_result += basic.user_time.seconds + basic.user_time.microseconds
                      + basic.system_time.seconds + basic.system_time.microseconds;

    thread_extended_info_data_t thread = {0};
    count = THREAD_EXTENDED_INFO_COUNT;
    result |= thread_info(port, THREAD_EXTENDED_INFO, (thread_info_t)&thread, &count);
    telemetry_result += thread.pth_user_time + thread.pth_system_time
        + thread.pth_cpu_usage + thread.pth_policy + thread.pth_run_state
        + thread.pth_flags + thread.pth_sleep_time + thread.pth_curpri
        + thread.pth_priority + thread.pth_maxpriority;

    /* The per-Present path uses the borrowed pthread port and raw uptime. */
    size_t cpu = 0;
    result |= pthread_cpu_number_np(&cpu);
    telemetry_result += cpu + pthread_mach_thread_np(pthread_self())
        + clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

    task_power_info_v2_data_t power = {0};
    count = TASK_POWER_INFO_V2_COUNT;
    result |= task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&power, &count);
    telemetry_result += power.task_pset_switches;

    mach_timebase_info_data_t timebase = {0};
    result |= mach_timebase_info(&timebase);
    telemetry_result += timebase.numer + timebase.denom;
    result |= mach_port_deallocate(mach_task_self(), port);
    return result;
}
"""

STRONG_IMPORTS = (
    "proc_pid_rusage", "thread_info", "task_info", "mach_timebase_info",
    "mach_thread_self", "mach_port_deallocate", "pthread_mach_thread_np",
    "pthread_cpu_number_np", "pthread_self", "clock_gettime_nsec_np", "getpid",
)


def main():
    # A Mac without the visionOS SDK cannot link for xrOS; that is a missing
    # tool, not a failed check. The headset build itself needs the SDK.
    try:
        sdk = subprocess.run(
            ["xcrun", "--sdk", "xros", "--show-sdk-path"], text=True, timeout=30,
            capture_output=True,
        ).stdout.strip()
    except (OSError, subprocess.TimeoutExpired):
        sdk = ""
    if not sdk or not Path(sdk).is_dir():
        print("SKIP xrOS telemetry API link check: no visionOS (xros) SDK found by xcrun")
        return
    with tempfile.TemporaryDirectory(prefix="halo-core-telemetry-xros-") as temp:
        source = Path(temp) / "telemetry-api-link.c"
        binary = Path(temp) / "telemetry-api-link"
        source.write_text(PROBE)
        subprocess.run(
            ["xcrun", "--sdk", "xros", "clang", "-target", "arm64-apple-xros26.0",
             "-isysroot", sdk, "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wl,-fatal_warnings",
             str(source), "-o", str(binary)],
            check=True, timeout=60,
        )
        symbols = subprocess.check_output(
            ["xcrun", "nm", "-m", str(binary)], text=True, timeout=30
        )
        for name in STRONG_IMPORTS:
            # A weak import may link even when a symbol is absent from the SDK.
            expected = f"(undefined) external _{name} (from libSystem)"
            if expected not in [line.strip() for line in symbols.splitlines()]:
                raise RuntimeError(f"missing strong libSystem import: {name}")
        loads = subprocess.check_output(
            ["xcrun", "otool", "-l", str(binary)], text=True, timeout=30
        )
        # PLATFORM_XROS is 11; reject an accidental macOS/simulator check.
        if not re.search(r"cmd LC_BUILD_VERSION\s+cmdsize \d+\s+platform (?:11|XROS)\s+minos 26\.0\b", loads):
            raise RuntimeError("probe was not linked for xrOS 26.0")
    print(f"PASS xrOS telemetry API compile and {len(STRONG_IMPORTS)} strong libSystem imports "
          f"({Path(sdk).name}, arm64-apple-xros26.0); no device execution")


if __name__ == "__main__":
    main()
