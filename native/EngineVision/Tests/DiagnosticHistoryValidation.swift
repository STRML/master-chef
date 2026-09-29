import Foundation

@main struct DiagnosticHistoryValidation {
    static func main() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let history = EngineDiagnosticHistory(directory: root, segmentBytes: 8192, maximumSegments: 16)
        for i in 0..<2100 { history.append(["sample": i, "state": "running"]) }
        precondition(history.failure == nil && history.records == 2100 && !history.truncated)
        let rows = try history.filenames.flatMap { name in
            try String(contentsOf: root.appendingPathComponent(name), encoding: .utf8).split(separator: "\n").map {
                try JSONSerialization.jsonObject(with: Data($0.utf8)) as! [String: Any]
            }
        }
        precondition(rows.count == 2100 && rows.first?["sample"] as? Int == 0 && rows.last?["sample"] as? Int == 2099)
        let retained = root.appendingPathComponent("bounded")
        try FileManager.default.createDirectory(at: retained, withIntermediateDirectories: true)
        let bounded = EngineDiagnosticHistory(directory: retained, segmentBytes: 128, maximumSegments: 2)
        for i in 0..<100 { bounded.append(["sample": i, "padding": "abcdefghij"]) }
        precondition(bounded.failure == nil && bounded.truncated && bounded.filenames.count == 2)
        let retainedFiles = try FileManager.default.contentsOfDirectory(atPath: retained.path)
        precondition(retainedFiles.count == 2)
        for name in bounded.filenames {
            let size = try Data(contentsOf: retained.appendingPathComponent(name)).count
            precondition(size <= 128)
        }
        // Reusing a run directory cannot silently overwrite an earlier run.
        let before = try Data(contentsOf: root.appendingPathComponent("timeline-000000.jsonl"))
        let collision = EngineDiagnosticHistory(directory: root)
        collision.append(["sample": 9999])
        precondition(collision.failure != nil && collision.records == 0)
        let after = try Data(contentsOf: root.appendingPathComponent("timeline-000000.jsonl"))
        precondition(after == before)
        // Storage failure is reported once; it must not terminate the engine.
        let bad = EngineDiagnosticHistory(directory: root.appendingPathComponent("missing"))
        bad.append(["sample": 1]); bad.append(["sample": 2])
        precondition(bad.failure != nil && bad.records == 0)
        print("Diagnostic history passed: >30 minute retention, bounded rotation, no overwrite, nonfatal storage errors")
    }
}
