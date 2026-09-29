import Foundation

/// Cadence of distinct world epochs submitted by the app. This measures
/// submission, not proof that the display scanned out a particular frame.
struct EngineWorldCadence {
    private var scene: UInt64 = 0
    private var epoch: UInt64 = 0
    private var lastNewTime: Double?
    private var intervals = [Double]()
    private var submissionHolds = [Int]()
    private var heldSubmissions = 0
    private var next = 0
    private let capacity = 4096
    private(set) var uniqueFrames: UInt64 = 0
    private(set) var repeatedFrames: UInt64 = 0
    private(set) var stallsOver100ms: UInt64 = 0
    private(set) var maximumRetainedMilliseconds: Double = 0

    mutating func record(scene: UInt64, epoch: UInt64, time: Double, eligible: Bool) {
        guard eligible, scene > 0, epoch > 0, time.isFinite else {
            lastNewTime = nil; self.scene = 0; self.epoch = 0; heldSubmissions = 0; return
        }
        guard self.scene == scene, epoch >= self.epoch,
              let lastNewTime, time >= lastNewTime else {
            self.scene = scene; self.epoch = epoch; self.lastNewTime = time
            heldSubmissions = 1; uniqueFrames += 1; return
        }
        let milliseconds = (time - lastNewTime) * 1000
        if epoch == self.epoch {
            repeatedFrames += 1; heldSubmissions += 1
            maximumRetainedMilliseconds = max(maximumRetainedMilliseconds, milliseconds)
            return
        }
        self.epoch = epoch; self.lastNewTime = time; uniqueFrames += 1
        if milliseconds > 100 { stallsOver100ms += 1 }
        if intervals.count < capacity {
            intervals.append(milliseconds); submissionHolds.append(heldSubmissions)
        } else {
            intervals[next] = milliseconds; submissionHolds[next] = heldSubmissions
            next = (next + 1) % capacity
        }
        heldSubmissions = 1
    }

    var statistics: [String: Double] {
        let sorted = intervals.sorted()
        func percentile(_ p: Double) -> Double {
            guard !sorted.isEmpty else { return 0 }
            return sorted[max(0, min(sorted.count - 1, Int(ceil(Double(sorted.count) * p)) - 1))]
        }
        // Submit-to-submit intervals expressed in nominal 90 Hz periods
        // are an estimate of display holds, not proof of scanout. Also count
        // actual app submissions of each epoch: these differ if the app is
        // submitting below 90 Hz or misses a compositor frame.
        func mode(_ values: [Int]) -> (count: Int, share: Double) {
            var counts = [Int: Int]()
            for value in values { counts[value, default: 0] += 1 }
            let modal = counts.max { $0.value == $1.value ? $0.key > $1.key : $0.value < $1.value }
            return (modal?.key ?? 0, values.isEmpty ? 0 : Double(modal?.value ?? 0) / Double(values.count))
        }
        let nominal = mode(sorted.map { max(1, Int(($0 * 0.09).rounded())) })
        let submitted = mode(submissionHolds)
        let mean = sorted.isEmpty ? 0 : sorted.reduce(0, +) / Double(sorted.count)
        let spread = sorted.isEmpty ? 0 : (sorted.reduce(0) { $0 + ($1 - mean) * ($1 - mean) } / Double(sorted.count)).squareRoot()
        return ["uniqueSubmittedWorldFrames": Double(uniqueFrames),
                "repeatedSubmittedWorldFrames": Double(repeatedFrames),
                "intervalSamples": Double(sorted.count),
                "uniqueIntervalP50Milliseconds": percentile(0.50),
                "uniqueIntervalP95Milliseconds": percentile(0.95),
                "uniqueIntervalP99Milliseconds": percentile(0.99),
                "uniqueIntervalModal90HzPeriods": Double(nominal.count),
                "uniqueIntervalModalShare": nominal.share,
                "uniqueIntervalModalSubmissionFrames": Double(submitted.count),
                "uniqueIntervalModalSubmissionShare": submitted.share,
                "uniqueIntervalStdDevMilliseconds": spread,
                "stallsOver100ms": Double(stallsOver100ms),
                "maximumRetainedMilliseconds": maximumRetainedMilliseconds]
    }
}
