@main
struct MenuInputValidation {
    static func main() {
        typealias Input = EngineMenuInput<Int>
        let a = Input.Target(origin: .zero, direction: SIMD3(0, 0, -1), trackingArea: 1)
        let b = Input.Target(origin: SIMD3(1, 0, 0), direction: SIMD3(0, 0, -1), trackingArea: 2)
        var input = Input()
        input.update(id: 1, phase: .active, target: a)
        input.update(id: 1, phase: .ended, target: nil)
        precondition(input.drain().clicks.isEmpty, "No menu means no input")
        input.setEnabled(true)
        for id in 1...2 {
            input.update(id: id, phase: .active, target: id == 1 ? a : b)
            input.update(id: id, phase: .ended, target: nil)
        }
        precondition(input.drain().clicks == [a, b], "Two complete taps between render frames survive")
        precondition(input.drain().clicks.isEmpty, "A completed pinch is delivered once")
        input.update(id: 3, phase: .active, target: a)
        input.update(id: 4, phase: .active, target: b)
        input.update(id: 4, phase: .cancelled, target: nil)
        precondition(input.drain().preview == a, "Another hand cancelling cannot release the first")
        input.update(id: 3, phase: .active, target: b)
        input.update(id: 3, phase: .ended, target: nil)
        precondition(input.drain().clicks == [a], "Pinch keeps its original gaze target")
        input.update(id: 5, phase: .active, target: nil)
        input.update(id: 5, phase: .ended, target: b)
        precondition(input.drain().clicks.isEmpty, "An ended-only/missing-ray event cannot borrow an old ray")
        input.update(id: 6, phase: .active, target: .init(origin: .zero, direction: .zero))
        input.update(id: 6, phase: .ended, target: nil)
        input.update(id: 7, phase: .active, target: .init(origin: SIMD3(.nan, 0, 0), direction: a.direction))
        input.update(id: 7, phase: .ended, target: nil)
        precondition(input.drain().clicks.isEmpty, "Invalid rays never click")
        input.update(id: 8, phase: .active, target: a)
        input.update(id: 8, phase: .cancelled, target: nil)
        precondition(input.drain().clicks.isEmpty, "Cancelled pinches never click")
        input.update(id: 9, phase: .active, target: a)
        input.update(id: 10, phase: .active, target: b)
        input.update(id: 10, phase: .ended, target: nil)
        input.setEnabled(false); input.setEnabled(true)
        input.update(id: 9, phase: .active, target: a)
        input.update(id: 9, phase: .ended, target: nil)
        precondition(input.drain().clicks.isEmpty, "Menu/lifecycle reset clears input and suppresses a still-held pinch")
        // The adapter must feed terminal events even with the menu hidden.
        // Repeated reuse also catches a suppression set that never retires IDs.
        for id in 200..<240 {
            input.update(id: id, phase: .active, target: a)
            input.setEnabled(false)
            input.update(id: id, phase: id % 2 == 0 ? .ended : .cancelled, target: nil)
            input.setEnabled(true)
            input.update(id: id, phase: .active, target: b)
            input.update(id: id, phase: .ended, target: nil)
            precondition(input.drain().clicks == [b], "A terminal event while hidden retires a reused ID")
        }
        for id in 20..<120 {
            input.update(id: id, phase: .active, target: a)
            input.update(id: id, phase: .ended, target: nil)
        }
        precondition(input.drain().clicks.count == Input.capacity, "Backlog is bounded")
        print("Menu spatial-event transitions passed")
    }
}
