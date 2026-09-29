import Foundation

/// Append-only, bounded session evidence: the run's one-second samples, for
/// campaign and suspend/resume audits. report.json keeps only the latest state.
/// Called only on EngineDiagnosticReportWriter's queue, about once a second.
final class EngineDiagnosticHistory {
    private let directory: URL
    private let prefix: String
    private let segmentBytes: Int
    private let maximumSegments: Int
    private var handle: FileHandle?
    private var index = 0
    private var bytes = 0
    private var files: [URL] = []
    private(set) var records: UInt64 = 0
    private(set) var truncated = false
    private(set) var failure: String?

    init(directory: URL, segmentBytes: Int = 8 * 1024 * 1024, maximumSegments: Int = 16, prefix: String = "timeline") {
        self.directory = directory
        self.prefix = prefix
        self.segmentBytes = max(128, segmentBytes)
        self.maximumSegments = max(1, maximumSegments)
    }

    deinit { try? handle?.close() }

    var filenames: [String] { files.map(\.lastPathComponent) }

    func append(_ sample: [String: Any]) {
        guard failure == nil else { return }
        do {
            var data = try JSONSerialization.data(withJSONObject: sample, options: [.sortedKeys])
            data.append(0x0a)
            guard data.count <= segmentBytes else {
                throw NSError(domain: "HaloDiagnosticHistory", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "Diagnostic record exceeds segment size"])
            }
            if handle == nil || bytes + data.count > segmentBytes {
                try handle?.close(); handle = nil
                let url = directory.appendingPathComponent(String(format: "%@-%06d.jsonl", prefix, index))
                guard !FileManager.default.fileExists(atPath: url.path),
                      FileManager.default.createFile(atPath: url.path, contents: nil) else {
                    throw NSError(domain: "HaloDiagnosticHistory", code: 2,
                                  userInfo: [NSLocalizedDescriptionKey: "Cannot create a new diagnostic segment"])
                }
                handle = try FileHandle(forWritingTo: url)
                files.append(url); index += 1; bytes = 0
                if files.count > maximumSegments {
                    try FileManager.default.removeItem(at: files[0])
                    files.removeFirst(); truncated = true
                }
            }
            try handle?.write(contentsOf: data)
            bytes += data.count; records += 1
        } catch {
            failure = error.localizedDescription
            try? handle?.close(); handle = nil
        }
    }
}
