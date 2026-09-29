import Foundation

@MainActor
final class EngineRuntimeModel: ObservableObject {
    @Published var state = Int32(ENGINEVISION_IDLE)
    @Published var status = "Engine has not started."
    @Published var frameSequence: UInt64 = 0
    @Published var frameSize = "No engine frame"
    @Published var controllerStatus = "Checking controller"
    @Published var diagnosticsStatus = "Preparing test report"
    let diagnostics = EngineDiagnostics()
    private var timer: Timer?

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        }
    }
    deinit { timer?.invalidate() }

    var canStart: Bool { state == ENGINEVISION_IDLE }
    var isActive: Bool { state == ENGINEVISION_STARTING || state == ENGINEVISION_RUNNING }

    func start(root: URL) {
        guard canStart else { return }
        let result = root.path.withCString { enginevision_start($0) }
        refresh()
        if result != 0 && state != ENGINEVISION_FAILED { status = "Engine start failed with POSIX status \(result)." }
    }

    func stop() { if isActive { enginevision_request_stop(); refresh() } }

    func refresh() {
        state = Int32(enginevision_runtime_state())
        var bytes = [CChar](repeating: 0, count: 512)
        enginevision_copy_status(&bytes, bytes.count)
        status = String(cString: bytes)
        var frame = EngineVisionFrameInfo()
        if enginevision_frame_info(&frame) {
            frameSequence = frame.sequence
            frameSize = "\(frame.width) × \(frame.height) · \(ByteCountFormatter.string(fromByteCount: Int64(frame.byte_count), countStyle: .memory))"
        }
        diagnostics.sample(state: state, status: status, frame: frame)
        controllerStatus = diagnostics.controllerStatus
        diagnosticsStatus = diagnostics.saveStatus
    }
}
