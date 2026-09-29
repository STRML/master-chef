import Foundation
@main struct WorldCadenceValidation {
    static func main() {
        var cadence = EngineWorldCadence()
        precondition(cadence.statistics["uniqueIntervalModal90HzPeriods"] == 0)
        precondition(cadence.statistics["uniqueIntervalModalSubmissionFrames"] == 0)
        for frame in 0..<900 {
            cadence.record(scene: 1, epoch: UInt64(frame / 3 + 1), time: Double(frame) / 90, eligible: true)
        }
        precondition(cadence.uniqueFrames == 300 && cadence.repeatedFrames == 600)
        precondition(abs(cadence.statistics["uniqueIntervalP50Milliseconds"]! - 1000 / 30) < 0.001)
        precondition(cadence.stallsOver100ms == 0)
        // An even 30 fps holds every picture for three display frames.
        precondition(cadence.statistics["uniqueIntervalModal90HzPeriods"] == 3)
        precondition(cadence.statistics["uniqueIntervalModalShare"] == 1)
        precondition(cadence.statistics["uniqueIntervalModalSubmissionFrames"] == 3)
        precondition(cadence.statistics["uniqueIntervalModalSubmissionShare"] == 1)
        precondition(cadence.statistics["uniqueIntervalStdDevMilliseconds"]! < 0.001)
        // Pictures held 5, 6, 7, 6 display frames: six is the commonest, half.
        var uneven = EngineWorldCadence()
        var tick = 0
        for (n, hold) in [5, 6, 7, 6].enumerated() {
            uneven.record(scene: 1, epoch: UInt64(n + 1), time: Double(tick) / 90, eligible: true); tick += hold
        }
        uneven.record(scene: 1, epoch: 5, time: Double(tick) / 90, eligible: true)
        precondition(uneven.statistics["uniqueIntervalModal90HzPeriods"] == 6)
        precondition(uneven.statistics["uniqueIntervalModalShare"] == 0.5)
        precondition(abs(uneven.statistics["uniqueIntervalStdDevMilliseconds"]! - 1000 / 90 * (0.5).squareRoot()) < 0.001)
        // At 45 Hz submission, a 15 fps picture spans three submissions but
        // six nominal 90 Hz periods; neither metric asserts actual scanout.
        var halfRate = EngineWorldCadence()
        for frame in 0..<90 {
            halfRate.record(scene: 1, epoch: UInt64(frame / 3 + 1), time: Double(frame) / 45, eligible: true)
        }
        precondition(halfRate.statistics["uniqueIntervalModal90HzPeriods"] == 6)
        precondition(halfRate.statistics["uniqueIntervalModalSubmissionFrames"] == 3)
        precondition(halfRate.statistics["uniqueIntervalModalSubmissionShare"] == 1)
        // A scene boundary discards its unfinished hold, and a repeated
        // epoch can still appear in the new scene as its first unique frame.
        halfRate.record(scene: 2, epoch: 30, time: 5, eligible: true)
        halfRate.record(scene: 2, epoch: 31, time: 5 + 1.0 / 45, eligible: true)
        precondition(halfRate.statistics["intervalSamples"] == 30)
        precondition(halfRate.statistics["uniqueIntervalModalSubmissionFrames"] == 3)
        cadence.record(scene: 1, epoch: 300, time: 11, eligible: true)
        precondition(cadence.maximumRetainedMilliseconds > 1000)
        cadence.record(scene: 1, epoch: 301, time: 11.1, eligible: true)
        precondition(cadence.stallsOver100ms == 1)
        // A loading/menu interval or changed scene cannot contaminate gameplay cadence.
        cadence.record(scene: 0, epoch: 0, time: 12, eligible: false)
        cadence.record(scene: 2, epoch: 302, time: 60, eligible: true)
        precondition(cadence.stallsOver100ms == 1)
        // Nonfinite time and regressed epochs restart the observation interval.
        cadence.record(scene: 2, epoch: 303, time: .nan, eligible: true)
        cadence.record(scene: 2, epoch: 300, time: 61, eligible: true)
        for i in 1...6000 { cadence.record(scene: 2, epoch: UInt64(300+i), time: 61+Double(i)/30, eligible: true) }
        precondition(cadence.statistics["intervalSamples"] == 4096)
        precondition(abs(cadence.statistics["uniqueIntervalP99Milliseconds"]! - 1000 / 30) < 0.001)
        precondition(cadence.statistics["uniqueIntervalModalSubmissionFrames"] == 1)
        precondition(cadence.statistics["uniqueIntervalModalSubmissionShare"] == 1)
        print("PASS distinct world cadence, repeated frames, stalls, scene gaps, bounded percentiles, evenness and measured versus nominal holds")
    }
}
