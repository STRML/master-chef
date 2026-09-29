/// Main-actor-owned operation generations. Async completions may only affect
/// the generation that scheduled them; opening a replacement invalidates old work.
struct EngineImmersiveOwnership {
    private(set) var generation: UInt64 = 0
    mutating func advance() -> UInt64 { generation &+= 1; return generation }
    func owns(_ token: UInt64) -> Bool { token == generation }
    mutating func beginOpen(active: Bool, opening: Bool) -> UInt64? {
        guard !active, !opening else { return nil }
        return advance()
    }
    func canDismiss(_ token: UInt64, active: Bool, opening: Bool) -> Bool {
        owns(token) && !active && !opening
    }
}
