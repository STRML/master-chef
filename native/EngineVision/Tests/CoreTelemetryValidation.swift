import Foundation

@main
struct CoreTelemetryValidation {
    static func main() throws {
        // The serializer must preserve counts, units, availability and all
        // fixed-size C arrays in the identical report/timeline payload.
        var t = EngineVisionCoreTelemetry()
        t.snapshot_available = 1
        t.frames = 8
        t.wall_ns = 800_000_000
        t.pass_ns = 240_000_000
        t.outside_pass_ns = 560_000_000
        t.counts_available = 1
        t.counts_frames = 7
        t.tick_unknown_frames = 1
        t.ticks = 10
        t.tick_frames = (1, 3, 2, 1, 0)
        t.tick_other_ns = (1_000_000, 2_000_000, 3_000_000, 4_000_000, 5_000_000)
        t.process_available = 1
        t.process_cpu_ns = 2_000_000_000
        t.process_performance_ns = 1_500_000_000
        t.process_cycles = 6_000_000_000
        t.process_performance_cycles = 4_800_000_000
        t.process_qos_ns = (1_000_000_000, 2_000_000_000, 3_000_000_000, 4_000_000_000, 5_000_000_000, 6_000_000_000, 7_000_000_000)
        t.thread_info_available = 1
        t.thread_run_ns = 123_000_000
        t.thread_cpu_usage = 850
        t.thread_policy = 1
        t.thread_priority = 47
        t.thread_base_priority = 37
        let fields = EngineCoreTelemetry.fields(t)
        let data = try JSONSerialization.data(withJSONObject: fields, options: [.sortedKeys])
        let json = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        assert(Set(json.keys) == ["engineFrameSplit", "engineThreadScheduling", "processCores"])
        let split = json["engineFrameSplit"] as! [String: Any]
        assert(split["snapshotAvailable"] as! Bool)
        assert(split["outsidePassSeconds"] as! Double == 0.56)
        assert(split["countsFrames"] as! Int == 7 && split["tickUnknownFrames"] as! Int == 1)
        assert(split["tickFrames"] as! [Int] == [1, 3, 2, 1, 0])
        assert(split["tickOtherSeconds"] as! [Double] == [0.001, 0.002, 0.003, 0.004, 0.005])
        let process = json["processCores"] as! [String: Any]
        assert(process["cycles"] as! UInt64 == 6_000_000_000)
        assert(process["performanceSeconds"] as! Double == 1.5)
        let qos = process["qosSeconds"] as! [String: Double]
        assert(qos["default"] == 1 && qos["maintenance"] == 2 && qos["background"] == 3)
        assert(qos["utility"] == 4 && qos["legacy"] == 5 && qos["userInitiated"] == 6 && qos["userInteractive"] == 7)
        let scheduling = json["engineThreadScheduling"] as! [String: Any]
        assert(scheduling["runSeconds"] as! Double == 0.123)
        assert(scheduling["cpuUsagePercent"] as! Double == 85)
        assert(scheduling["priority"] as! Int == 47 && scheduling["basePriority"] as! Int == 37)
        t.snapshot_available = 0
        t.counts_available = 0
        t.process_available = 0
        t.process_rusage_result = 78
        t.process_timebase_result = 5
        t.thread_info_available = 0
        t.thread_info_result = 4
        let unavailable = EngineCoreTelemetry.fields(t)
        assert((unavailable["engineFrameSplit"] as! [String: Any])["snapshotAvailable"] as! Bool == false)
        assert((unavailable["processCores"] as! [String: Any])["available"] as! Bool == false)
        assert((unavailable["processCores"] as! [String: Any])["rusageResult"] as! Int32 == 78)
        assert((unavailable["processCores"] as! [String: Any])["timebaseResult"] as! Int32 == 5)
        assert((unavailable["engineThreadScheduling"] as! [String: Any])["available"] as! Bool == false)
        print("PASS core telemetry JSON units, tick buckets, QoS order and explicit API availability")
    }
}
