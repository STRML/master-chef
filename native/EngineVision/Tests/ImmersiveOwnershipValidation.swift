@main
struct ImmersiveOwnershipValidation {
    static func main() {
        var state = EngineImmersiveOwnership()
        let startupA = state.advance()
        let startupB = state.advance()
        precondition(!state.owns(startupA)) // A failing must not clear B.
        precondition(state.owns(startupB))
        let beforeAutomaticOpen = state.generation
        precondition(state.beginOpen(active: true, opening: false) == nil)
        precondition(state.generation == beforeAutomaticOpen) // queued open cannot toggle closed
        let closingB = state.generation
        precondition(state.canDismiss(closingB, active: false, opening: false))
        let openingC = state.beginOpen(active: false, opening: false)!
        precondition(!state.canDismiss(closingB, active: false, opening: true))
        precondition(state.beginOpen(active: false, opening: true) == nil)
        _ = state.advance() // C's layer arrives before openImmersiveSpace returns.
        precondition(!state.owns(openingC)) // late open failure cannot clear live C
        precondition(!state.canDismiss(closingB, active: true, opening: false))
        print("Immersive ownership interleavings passed")
    }
}
