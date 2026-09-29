import Foundation

/// Writes the device report and its timeline away from the main thread.
///
/// report.json used to be rewritten every second on the main actor,
/// pretty-printed with sorted keys and carrying every one-second sample since
/// launch: 1.0-1.25 MB and 25-39 ms a write four minutes in, 177 ms at the
/// thirty-minute cap, on the thread the app's timers and SwiftUI run on. The
/// one-second samples already went to the timeline segments
/// (EngineDiagnosticHistory), so they now go only there, with every field the
/// report's copy had. The report keeps the latest state, a few kilobytes of
/// compact JSON, written when the engine's state changes, every thirty
/// seconds, on scene-phase changes and at exit. Both files are written on one
/// serial queue in the order they were handed over, and the timeline is only
/// ever touched there.
final class EngineDiagnosticReportWriter {
    struct Stats {
        var writes: UInt64 = 0
        var lastMilliseconds = 0.0
        var maxMilliseconds = 0.0
        var bytes = 0
        var failure: String?
        var deepDropped: UInt64 = 0
    }

    /// Seconds between reports while nothing else asks for one.
    let interval: TimeInterval
    private let reportURL: URL
    private let history: EngineDiagnosticHistory?
    private let deepHistory: EngineDiagnosticHistory
    private var deepPending = 0
    private var lastLiveUptime = -Double.infinity // writer queue only
    private let queue = DispatchQueue(label: "halo.diagnostics.writer", qos: .utility)
    private let lock = NSLock()
    private var written = Stats()
    // Main-actor side: when the report was last handed over and in what state.
    private var lastReportUptime: TimeInterval?
    private var lastReportState: Int32?

    init(reportURL: URL, history: EngineDiagnosticHistory?, interval: TimeInterval = EngineDiagnosticReportWriter.defaultInterval()) {
        self.reportURL = reportURL
        self.history = history
        self.deepHistory = EngineDiagnosticHistory(directory: reportURL.deletingLastPathComponent(),
            segmentBytes: 2 * 1024 * 1024, maximumSegments: 16, prefix: "deep")
        self.interval = max(1, interval)
    }

    /// Thirty seconds; HALO_REPORT_SECONDS (1-600) sets another period.
    static func defaultInterval(environment: [String: String] = ProcessInfo.processInfo.environment) -> TimeInterval {
        guard let text = environment["HALO_REPORT_SECONDS"], let seconds = Double(text), seconds.isFinite else { return 30 }
        return min(600, max(1, seconds))
    }

    /// Why a report is due at this sample, or nil: the first one, a change
    /// of engine state (a failure or a stop must not wait half a minute), or
    /// the period gone by.
    func reportDue(uptime: TimeInterval, state: Int32) -> String? {
        guard let lastReportUptime, let lastReportState else { return "first" }
        if state != lastReportState { return "state" }
        return uptime - lastReportUptime >= interval ? "interval" : nil
    }

    /// Appends one timeline record after everything handed over before it.
    func appendTimeline(_ record: [String: Any]) {
        guard let history else { return }
        queue.async { history.append(record) }
    }

    /// At most eight records can queue: slow storage must not grow memory
    /// indefinitely. Loss is explicit in live.json and the session report.
    /// The compact latest snapshot is overwritten once a second or on events.
    func appendDeep(_ record: [String: Any], uptime: TimeInterval) {
        lock.lock()
        guard deepPending < 8 else { written.deepDropped += 1; lock.unlock(); return }
        deepPending += 1
        lock.unlock()
        queue.async { [self] in
            defer { lock.lock(); deepPending -= 1; lock.unlock() }
            var record = record
            record["writerDroppedRecords"] = stats.deepDropped
            deepHistory.append(record)
            let events = record["events"] as? [String] ?? []
            if uptime - lastLiveUptime >= 1 || !events.isEmpty {
                record["deepHistoryFiles"] = deepHistory.filenames
                record["deepHistoryTruncated"] = deepHistory.truncated
                record["deepHistoryError"] = deepHistory.failure ?? ""
                do {
                    let data = try JSONSerialization.data(withJSONObject: record, options: [.sortedKeys])
                    try data.write(to: reportURL.deletingLastPathComponent().appendingPathComponent("live.json"), options: .atomic)
                    lastLiveUptime = uptime
                } catch {
                    lock.lock(); written.failure = error.localizedDescription; lock.unlock()
                }
            }
        }
    }

    /// Writes the report after the timeline records handed over before it,
    /// adding the timeline's own account and this writer's: how many reports
    /// went before this one and what the last and slowest of them cost.
    func writeReport(_ report: [String: Any], uptime: TimeInterval, state: Int32, reason: String) {
        lastReportUptime = uptime; lastReportState = state
        let url = reportURL, history = history
        queue.async { [self] in
            let started = DispatchTime.now().uptimeNanoseconds
            var report = report
            report["reportReason"] = reason
            report["reportIntervalSeconds"] = interval
            report["historyFiles"] = history?.filenames ?? []
            report["historyRecords"] = history?.records ?? 0
            report["historyTruncated"] = history?.truncated ?? false
            report["historyError"] = history?.failure ?? ""
            report["deepHistoryFiles"] = deepHistory.filenames
            report["deepHistoryRecords"] = deepHistory.records
            report["deepHistoryTruncated"] = deepHistory.truncated
            report["deepHistoryError"] = deepHistory.failure ?? ""
            let previous = stats
            report["reportWrites"] = previous.writes
            report["deepDroppedRecords"] = previous.deepDropped
            report["reportWriteMilliseconds"] = previous.lastMilliseconds
            report["reportWriteMillisecondsMax"] = previous.maxMilliseconds
            var failure: String?
            var bytes = 0
            do {
                let data = try JSONSerialization.data(withJSONObject: report, options: [.sortedKeys])
                try data.write(to: url, options: .atomic)
                bytes = data.count
            } catch { failure = error.localizedDescription }
            let milliseconds = Double(DispatchTime.now().uptimeNanoseconds - started) / 1e6
            lock.lock()
            written.writes += failure == nil ? 1 : 0
            written.lastMilliseconds = milliseconds
            written.maxMilliseconds = max(written.maxMilliseconds, milliseconds)
            if failure == nil { written.bytes = bytes }
            written.failure = failure
            lock.unlock()
        }
    }

    /// The writes so far. Safe from any thread.
    var stats: Stats {
        lock.lock(); defer { lock.unlock() }
        return written
    }

    /// Returns once everything handed over so far is on disk, for the moments
    /// the app may be suspended or ended straight after.
    func drain() { queue.sync {} }
}
