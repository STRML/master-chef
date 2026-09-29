import Foundation

/// Effective native BSP-gather setting and lifetime dispatch counts. The same
/// fields go to report.json and each timeline record so interval differences
/// show whether the optimization was active during the measured workload.
enum EngineGatherDiagnostics {
    static func fields(_ sample: EngineDiagnosticSample) -> [String: Any] {
        [
            "enabled": sample.native_gather_enabled != 0,
            "calls": sample.native_gather_calls,
            "nativeCalls": sample.native_gather_native_calls,
            "fallbackCalls": sample.native_gather_fallback_calls
        ]
    }
}
