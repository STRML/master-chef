import Foundation

/// Cumulative counters shared by report.json and the one-second timeline.
/// Delta cycles / delta CPU nanoseconds is effective GHz while running, not
/// instantaneous frequency. Process residency covers all app threads.
enum EngineCoreTelemetry {
    static let qosNames = ["default", "maintenance", "background", "utility", "legacy", "userInitiated", "userInteractive"]

    static func fields(_ t: EngineVisionCoreTelemetry) -> [String: Any] {
        func seconds(_ nanoseconds: UInt64) -> Double { Double(nanoseconds) / 1e9 }
        func values<T>(_ tuple: T) -> [UInt64] { withUnsafeBytes(of: tuple) { Array($0.bindMemory(to: UInt64.self)) } }
        // Bucket index means 0, 1, 2, 3, or 4+ ticks. Unknown/reset intervals
        // are excluded; residual time is an association, not isolated tick cost.
        let split: [String: Any] = [
            "snapshotAvailable": t.snapshot_available != 0,
            "frames": t.frames, "wallSeconds": seconds(t.wall_ns), "passSeconds": seconds(t.pass_ns),
            "outsidePassSeconds": seconds(t.outside_pass_ns),
            "idleSeconds": seconds(t.idle_ns), "waitSeconds": seconds(t.wait_ns), "waits": t.waits,
            "sleepSeconds": seconds(t.sleep_ns), "otherSeconds": seconds(t.other_ns),
            "countsAvailable": t.counts_available != 0, "countsFrames": t.counts_frames,
            "cpuSeconds": seconds(t.cpu_ns), "offCoreSeconds": seconds(t.off_core_ns),
            "unaccountedOffCoreSeconds": seconds(t.unaccounted_ns),
            "ticks": t.ticks, "tickFrames": values(t.tick_frames),
            "tickOtherSeconds": values(t.tick_other_ns).map(seconds),
            "tickResyncs": t.tick_resyncs, "tickMismatchFrames": t.tick_mismatch_frames,
            "tickUnknownFrames": t.tick_unknown_frames,
            "maxTicksPerFrame": t.max_ticks, "stallFrames": t.stall_frames,
            "stallSeconds": seconds(t.stall_ns), "stallOutsidePassSeconds": seconds(t.stall_outside_pass_ns),
            "stallTicks": t.stall_ticks, "stallTickUnknownFrames": t.stall_tick_unknown_frames,
            "presents": t.presents]
        // CPU usage is the kernel's decaying average (TH_USAGE_SCALE = 1000).
        let scheduling: [String: Any] = [
            "available": t.thread_info_available != 0, "infoResult": t.thread_info_result,
            "policy": t.thread_policy, "priority": t.thread_priority, "basePriority": t.thread_base_priority,
            "maxPriority": t.thread_max_priority, "runState": t.thread_run_state, "flags": t.thread_flags,
            "cpuUsagePercent": Double(t.thread_cpu_usage) / 10, "runSeconds": seconds(t.thread_run_ns)]
        let qos = values(t.process_qos_ns).map(seconds)
        let process: [String: Any] = [
            "available": t.process_available != 0, "rusageResult": t.process_rusage_result,
            "timebaseResult": t.process_timebase_result,
            "cpuSeconds": seconds(t.process_cpu_ns), "performanceSeconds": seconds(t.process_performance_ns),
            "runnableSeconds": seconds(t.process_runnable_ns),
            "cycles": t.process_cycles, "instructions": t.process_instructions,
            "performanceCycles": t.process_performance_cycles,
            "performanceInstructions": t.process_performance_instructions,
            "qosSeconds": Dictionary(uniqueKeysWithValues: zip(qosNames, qos)),
            "energyJoules": Double(t.process_energy_nj) / 1e9,
            "performanceEnergyJoules": Double(t.process_performance_energy_nj) / 1e9,
            "diskReadBytes": t.process_disk_read_bytes, "pageins": t.process_pageins,
            "interruptWakeups": t.process_interrupt_wakeups, "idleWakeups": t.process_idle_wakeups,
            "clusterSwitchesAvailable": t.process_pset_switches_available != 0,
            "clusterSwitches": t.process_pset_switches]
        return ["engineFrameSplit": split, "engineThreadScheduling": scheduling, "processCores": process]
    }
}
