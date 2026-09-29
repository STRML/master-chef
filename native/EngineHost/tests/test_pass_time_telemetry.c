/* The actual D3D pass timer must retain lifetime totals when the optional
 * interval profiler resets; otherwise outside-pass telemetry invents work. */
#include "../d3d9.c"
#include "../core_telemetry.h"
#include <assert.h>
uint8_t *engine_flat_base;
uint64_t host_sleep_calls, host_sleep_ns, host_yield_calls, host_yield_spin_ns;
static unsigned reports;
void host_log(const char *format, ...) { (void)format; reports++; }
int main(void) {
    setenv("HALO_DRAW_PROFILE", "1", 1);
    HaloFrameSplit split = {0};
    HaloFrameSample first = {.now_ns = 1000000000};
    halo_frame_split_add(&split, &first);
    host_pass_profile_add(4000000);
    host_pass_profile_add(6000000);
    assert(host_pass_profile_total_ns() == 10000000);
    uint64_t profile[12]; host_draw_profile_snapshot(profile);
    assert(profile[0] == 2 && profile[1] == 10000000);
    host_draw_profile_report(120, 0.020);
    host_draw_profile_snapshot(profile);
    assert(reports == 1 && profile[0] == 0 && profile[1] == 0);
    assert(host_pass_profile_total_ns() == 10000000);
    HaloFrameSample second = {.now_ns = 1020000000, .pass_ns = host_pass_profile_total_ns()};
    halo_frame_split_add(&split, &second);
    host_pass_profile_add(9000000);
    host_draw_profile_report(240, 0.030);
    HaloFrameSample third = {.now_ns = 1050000000, .pass_ns = host_pass_profile_total_ns()};
    halo_frame_split_add(&split, &third);
    HaloFrameSplitTotals total;
    assert(halo_frame_split_read(&split, &total));
    assert(reports == 2 && total.frames == 2 && total.pass_ns == 19000000 && total.outside_pass_ns == 31000000);
    puts("PASS pass-time telemetry: interval logs reset, lifetime pass time remains 19 ms, outside-pass time remains 31 ms");
    return 0;
}
