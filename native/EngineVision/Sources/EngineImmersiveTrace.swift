import Foundation

/// A bounded startup timeline that survives renderer invalidation. Frame
/// counters alone cannot distinguish an ARKit wait from a paused compositor.
/// Shared by the main actor, render thread and first GPU completion callback.
final class EngineImmersiveTrace: @unchecked Sendable {
    static let shared = EngineImmersiveTrace()
    private struct Entry {
        let number: UInt64
        let seconds: Double
        let event: String
        let id: String
        let state: UInt32?
    }
    private let lock = NSLock()
    private let started = ProcessInfo.processInfo.systemUptime
    private var count: UInt64 = 0
    private var entries: [Entry] = []

    func record(_ event: String, id: String = "app", state: UInt32? = nil) {
        lock.lock()
        count += 1
        let entry = Entry(number: count, seconds: ProcessInfo.processInfo.systemUptime - started,
                          event: event, id: id, state: state)
        entries.append(entry)
        if entries.count > 128 { entries.removeFirst(entries.count - 128) }
        lock.unlock()
        NSLog("[immersive] trace #%llu +%.3fs %@ id=%@ state=%@", entry.number, entry.seconds,
              event, id, state.map { String($0) } ?? "n/a")
    }

    func snapshot() -> [[String: Any]] {
        lock.lock(); let copy = entries; lock.unlock()
        return copy.map { entry in
            ["number": entry.number, "elapsedSeconds": entry.seconds, "event": entry.event,
             "layerID": entry.id, "layerState": entry.state.map { Int($0) } ?? 0]
        }
    }
}
