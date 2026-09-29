import Foundation

@main
struct GatherDiagnosticsValidation {
    static func main() throws {
        let initial = EngineGatherDiagnostics.fields(EngineDiagnosticSample())
        precondition(initial["enabled"] as? Bool == false)
        precondition(initial["calls"] as? UInt64 == 0)
        precondition(initial["nativeCalls"] as? UInt64 == 0)
        precondition(initial["fallbackCalls"] as? UInt64 == 0)

        // Counts above 32 bits must survive both the imported C sample and the
        // JSON path used by report.json and the timeline without truncation.
        var sample = EngineDiagnosticSample()
        sample.native_gather_enabled = 1
        sample.native_gather_native_calls = 0x1_0000_0001
        sample.native_gather_fallback_calls = 17
        sample.native_gather_calls = sample.native_gather_native_calls + sample.native_gather_fallback_calls
        let fields = EngineGatherDiagnostics.fields(sample)
        let data = try JSONSerialization.data(withJSONObject: ["nativeGather": fields], options: [.sortedKeys])
        let decoded = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        let gather = decoded["nativeGather"] as! [String: Any]
        precondition(gather["enabled"] as? Bool == true)
        precondition((gather["calls"] as? NSNumber)?.uint64Value == sample.native_gather_calls)
        precondition((gather["nativeCalls"] as? NSNumber)?.uint64Value == sample.native_gather_native_calls)
        precondition((gather["fallbackCalls"] as? NSNumber)?.uint64Value == sample.native_gather_fallback_calls)
        // The switch is separate from the counters; turning it off does not
        // make already accumulated work disappear from a later sample.
        sample.native_gather_enabled = 0
        let disabled = EngineGatherDiagnostics.fields(sample)
        precondition(disabled["enabled"] as? Bool == false)
        precondition(disabled["nativeCalls"] as? UInt64 == sample.native_gather_native_calls)
        print("PASS native gather diagnostics: effective setting, cumulative 64-bit counts, JSON round trip")
    }
}
