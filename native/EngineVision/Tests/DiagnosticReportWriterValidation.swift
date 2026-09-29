import Foundation

@main struct DiagnosticReportWriterValidation {
    static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        // Thirty seconds unless HALO_REPORT_SECONDS says otherwise, within 1-600.
        precondition(EngineDiagnosticReportWriter.defaultInterval(environment: [:]) == 30)
        precondition(EngineDiagnosticReportWriter.defaultInterval(environment: ["HALO_REPORT_SECONDS": "5"]) == 5)
        precondition(EngineDiagnosticReportWriter.defaultInterval(environment: ["HALO_REPORT_SECONDS": "0"]) == 1)
        precondition(EngineDiagnosticReportWriter.defaultInterval(environment: ["HALO_REPORT_SECONDS": "9999"]) == 600)
        precondition(EngineDiagnosticReportWriter.defaultInterval(environment: ["HALO_REPORT_SECONDS": "soon"]) == 30)

        let url = root.appendingPathComponent("report.json")
        let history = EngineDiagnosticHistory(directory: root)
        let writer = EngineDiagnosticReportWriter(reportURL: url, history: history, interval: 30)

        // Due: the first sample, a change of engine state, or the period gone by.
        precondition(writer.reportDue(uptime: 100, state: 1) == "first")
        for second in 0..<3 { writer.appendTimeline(["elapsedSeconds": second, "state": 1]) }
        writer.writeReport(["schema": 2, "state": 1], uptime: 100, state: 1, reason: "first")
        precondition(writer.reportDue(uptime: 101, state: 1) == nil)
        precondition(writer.reportDue(uptime: 129.9, state: 1) == nil)
        precondition(writer.reportDue(uptime: 130, state: 1) == "interval")
        precondition(writer.reportDue(uptime: 101, state: 3) == "state")

        // Written in order off the caller's thread: the report counts the
        // timeline records handed over before it, and carries no samples.
        writer.drain()
        let text = try String(contentsOf: url, encoding: .utf8)
        precondition(!text.contains("\n"), "report.json is compact")
        var report = try JSONSerialization.jsonObject(with: Data(text.utf8)) as! [String: Any]
        precondition(report["samples"] == nil && report["historyRecords"] as? Int == 3)
        precondition(report["reportReason"] as? String == "first" && report["reportWrites"] as? Int == 0)
        precondition(report["historyFiles"] as? [String] == ["timeline-000000.jsonl"])
        let timeline = try String(contentsOf: root.appendingPathComponent("timeline-000000.jsonl"), encoding: .utf8)
        precondition(timeline.split(separator: "\n").count == 3)

        // The next report accounts for the writes before it.
        writer.appendTimeline(["elapsedSeconds": 3, "state": 3])
        writer.writeReport(["schema": 2, "state": 3], uptime: 101, state: 3, reason: "state")
        writer.drain()
        report = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as! [String: Any]
        precondition(report["historyRecords"] as? Int == 4 && report["reportWrites"] as? Int == 1)
        precondition((report["reportWriteMilliseconds"] as? Double ?? -1) >= 0 && report["reportReason"] as? String == "state")
        let stats = writer.stats
        precondition(stats.writes == 2 && stats.bytes > 0 && stats.failure == nil)

        // A heavy report, the kind the old samples array made, goes out
        // without holding up the caller for its serialization.
        let sample: [String: Any] = ["elapsedSeconds": 1.0, "frameSequence": 1, "residentBytes": 1, "audioPeak": 0.5,
                                     "immersiveSubmission": ["mode": "panorama", "layerEpochs": Array(0..<10)]]
        let heavy: [String: Any] = ["schema": 1, "samples": Array(repeating: sample, count: 20000)]
        let handedOver = DispatchTime.now().uptimeNanoseconds
        writer.writeReport(heavy, uptime: 200, state: 3, reason: "interval")
        let callerMilliseconds = Double(DispatchTime.now().uptimeNanoseconds - handedOver) / 1e6
        writer.drain()
        let heavyMilliseconds = writer.stats.lastMilliseconds
        precondition(heavyMilliseconds > callerMilliseconds, "serialization happens on the writer's queue")

        // Separate bounded deep history and atomic live snapshot, keyed to events.
        writer.appendDeep(["elapsedSeconds": 1.0, "events": [], "runID": "test"], uptime: 300)
        writer.appendDeep(["elapsedSeconds": 1.2, "events": ["controller.disconnect"], "runID": "test"], uptime: 300.2)
        writer.writeReport(["schema": 2], uptime: 301, state: 3, reason: "interval")
        writer.drain()
        let live = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent("live.json"))) as! [String: Any]
        precondition(live["elapsedSeconds"] as? Double == 1.2)
        let deep = try String(contentsOf: root.appendingPathComponent("deep-000000.jsonl"), encoding: .utf8)
        precondition(deep.split(separator: "\n").count == 2)
        report = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as! [String: Any]
        precondition(report["deepHistoryRecords"] as? Int == 2)

        // A report that cannot be saved is recorded, never fatal.
        let lost = EngineDiagnosticReportWriter(reportURL: root.appendingPathComponent("missing/report.json"), history: nil)
        lost.appendTimeline(["ignored": true])
        lost.writeReport(["schema": 2], uptime: 0, state: 0, reason: "first")
        lost.drain()
        precondition(lost.stats.failure != nil && lost.stats.writes == 0)
        print(String(format: "Diagnostic report writer passed: 30 s period, state/first triggers, ordered compact writes without samples, "
                     + "off-thread serialization (caller %.2f ms, writer %.1f ms for a 20000-sample report), nonfatal storage errors",
                     callerMilliseconds, heavyMilliseconds))
    }
}
