/// System spatial events are interactions, not a continuously available eye
/// pose. Keep the first valid selection ray for each ID, and commit a menu
/// click only when that interaction ends successfully. The renderer protects
/// this value with its input lock; no SwiftUI or ARKit dependency is needed.
struct EngineMenuInput<ID: Hashable> {
    struct Target: Equatable {
        var origin: SIMD3<Float>
        var direction: SIMD3<Float>
        var trackingArea: UInt64 = 0

        var isValid: Bool {
            let values = [origin.x, origin.y, origin.z, direction.x, direction.y, direction.z]
            return values.allSatisfy(\.isFinite) &&
                direction.x * direction.x + direction.y * direction.y + direction.z * direction.z > 0.000001
        }
    }
    enum Phase { case active, ended, cancelled }
    private(set) var enabled = false
    private var active: [ID: Target] = [:]
    private var order: [ID] = []
    private var completed: [Target] = []
    private var suppressed: Set<ID> = []
    static var capacity: Int { 16 }

    mutating func setEnabled(_ enabled: Bool) {
        self.enabled = enabled
        if !enabled {
            for id in order where suppressed.count < Self.capacity { suppressed.insert(id) }
            active.removeAll(); order.removeAll(); completed.removeAll()
        }
    }

    mutating func update(id: ID, phase: Phase, target: Target?) {
        if phase != .active, suppressed.remove(id) != nil { return }
        guard enabled else {
            if phase == .active, suppressed.count < Self.capacity { suppressed.insert(id) }
            return
        }
        guard !suppressed.contains(id) else { return }
        switch phase {
        case .active:
            // Updates from another hand cannot release this hand. A missing
            // or malformed ray can never borrow another interaction's target.
            if active[id] == nil, active.count < Self.capacity, let target, target.isValid {
                active[id] = target; order.append(id)
            }
        case .ended, .cancelled:
            if let target = active.removeValue(forKey: id), phase == .ended, completed.count < Self.capacity {
                completed.append(target)
            }
            order.removeAll { $0 == id }
        }
    }

    mutating func drain() -> (preview: Target?, clicks: [Target]) {
        let result = (preview: order.last.flatMap { active[$0] }, clicks: completed)
        completed.removeAll(keepingCapacity: true)
        return result
    }
}
