import SwiftUI
import UniformTypeIdentifiers
import CompositorServices
import GameController
import Combine

private let haloBuildLabel = "Build \(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "—")"

@main
struct HaloEngineVisionApp: App {
    @StateObject private var session = EngineAppSession()
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.openWindow) private var openWindow
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some Scene {
        ImmersiveSpace(id: EngineImmersiveSession.spaceID) {
            CompositorLayer(configuration: EngineImmersiveLayerConfiguration()) { layer in session.startImmersive(layer) }
        }
        .immersionStyle(selection: .constant(.full), in: .full)
        .upperLimbVisibility(.hidden)
        .defaultLaunchBehavior(.presented)
        .onChange(of: scenePhase, initial: true) { _, phase in
            EngineImmersiveTrace.shared.record("app.scenePhase: \(phase)")
            // The report is written every thirty seconds; one taken as the app
            // leaves the foreground may be the run's last, so it waits for disk.
            session.engine.diagnostics.flush(reason: "scenePhase: \(phase)", wait: phase == .background)
        }

        WindowGroup(id: "HaloSetup") {
            EngineContentView(assets: session.assets, engine: session.engine, immersive: session.immersive)
        }
        .defaultSize(width: 1180, height: 760)
        .defaultLaunchBehavior(.suppressed)
        /* The setup window has no place inside the game. visionOS keeps an
         * app's own windows up in a full immersive space, so the window is
         * put away when the space takes over. It is never opened by the app:
         * the game is the launch scene, and if the system shows the window
         * anyway (it does when the space could not present at launch), the
         * window opens the space itself as soon as the engine has a frame. */
        .onChange(of: session.immersiveActive) { _, active in
            if active {
                EngineImmersiveTrace.shared.record("app.dismissSetup")
                dismissWindow(id: "HaloSetup")
            }
        }

        /* A separate window so it can sit beside the immersive view and be
         * adjusted while the game runs. */
        WindowGroup(id: "HaloSettings") { EngineSettingsView() }
            .defaultSize(width: 620, height: 820)
            .defaultLaunchBehavior(.suppressed)
    }
}

/// Startup is owned by the app, not by a setup window that might never open.
@MainActor
final class EngineAppSession: ObservableObject {
    let assets = EngineAssetStore()
    let engine = EngineRuntimeModel()
    let immersive = EngineImmersiveSession()
    @Published private(set) var immersiveActive = false
    private var timer: Timer?
    private var immersiveWatch: AnyCancellable?

    init() {
        enginevision_prepare_controller()
        immersiveWatch = immersive.$active.removeDuplicates().sink { [weak self] active in self?.immersiveActive = active }
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 0.2, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.refresh() }
        }
    }
    deinit { timer?.invalidate() }

    func startImmersive(_ layer: LayerRenderer) {
        refresh()
        immersive.start(layer: layer)
    }

    private func refresh() {
        engine.diagnostics.preparationStatus = assets.detail
        engine.diagnostics.preparationCopiedBytes = assets.copied
        engine.diagnostics.preparationTotalBytes = assets.total
        engine.diagnostics.immersiveStatus = immersive.status
        engine.diagnostics.immersiveActive = immersive.active
        engine.diagnostics.immersiveLayerState = immersive.layerStateForDiagnostics
        let frameCounts = immersive.frameCounts()
        engine.diagnostics.immersiveSubmittedFrames = frameCounts.submitted
        engine.diagnostics.immersiveCancelledFrames = frameCounts.cancelled
        engine.diagnostics.immersiveTrackingLossFrames = frameCounts.trackingLoss
        engine.diagnostics.immersiveGPUCompletedFrames = frameCounts.gpuCompleted
        engine.diagnostics.immersiveGPUFailedFrames = frameCounts.gpuFailed
        engine.diagnostics.immersiveGPUError = frameCounts.gpuError
        engine.diagnostics.immersiveGPUMilliseconds = frameCounts.gpuMilliseconds
        engine.diagnostics.immersiveGPUMillisecondsMax = frameCounts.gpuMillisecondsMax
        engine.diagnostics.immersiveConfiguration = frameCounts.configuration
        engine.diagnostics.immersiveSubmission = immersive.submissionInfo()
        switch assets.phase {
        case .checking:
            immersive.setLoading(title: "Preparing Halo · \(haloBuildLabel)", detail: assets.detail, progress: nil, show: true)
        case .importing:
            let fraction = assets.total > 0 ? Double(assets.copied) / Double(assets.total) : nil
            immersive.setLoading(title: "Preparing Halo · \(haloBuildLabel)", detail: assets.detail, progress: fraction, show: true)
        case .missing, .failed:
            immersive.setLoading(title: "Halo setup needs attention", detail: assets.detail, progress: nil, show: true)
        case .ready:
            if engine.canStart { engine.start(root: assets.rootURL) }
            let failed = engine.state == ENGINEVISION_FAILED || engine.state == ENGINEVISION_STOPPED
            let show = failed || engine.frameSequence == 0
            immersive.setLoading(title: failed ? "Halo stopped · \(haloBuildLabel)" : "Starting Halo · \(haloBuildLabel)",
                                 detail: failed ? engine.status : "Loading the main menu. Use the left stick or D-pad to navigate, then press Cross to select.",
                                 progress: nil, show: show)
        }
    }
}

struct EngineContentView: View {
    @ObservedObject var assets: EngineAssetStore
    @ObservedObject var engine: EngineRuntimeModel
    @ObservedObject var immersive: EngineImmersiveSession
    @Environment(\.openWindow) private var openWindow
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @State private var showingImporter = false
    @State private var importAlert: String?
    @State private var autoOpened = false

    private var isReady: Bool { assets.phase == .ready }
    private var isImporting: Bool { assets.phase == .importing }
    private var stateName: String {
        switch engine.state {
        case Int32(ENGINEVISION_IDLE): "Ready"
        case Int32(ENGINEVISION_STARTING): "Starting"
        case Int32(ENGINEVISION_RUNNING): "Running"
        case Int32(ENGINEVISION_STOPPED): "Stopped"
        case Int32(ENGINEVISION_FAILED): "Failed"
        default: "Unknown"
        }
    }

    var body: some View {
        HStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Text("HALO").font(.system(size: 36, weight: .black, design: .rounded)).tracking(5)
                    Text("COMBAT EVOLVED").font(.caption.weight(.bold)).tracking(2).foregroundStyle(.cyan)
                    Text("Vision Pro · \(haloBuildLabel)").font(.caption).foregroundStyle(.secondary)
                    Divider()
                    Label(stateName, systemImage: engine.state == ENGINEVISION_FAILED ? "exclamationmark.triangle.fill" : "cpu")
                        .font(.headline).foregroundStyle(engine.state == ENGINEVISION_FAILED ? .orange : .primary)
                    if engine.state == ENGINEVISION_FAILED {
                        Text(engine.status).font(.caption).foregroundStyle(.orange).textSelection(.enabled)
                    }

                    if !assets.hasBundledPayload {
                        Button("Import owned Halo folder", systemImage: "externaldrive.badge.plus") { showingImporter = true }
                            .disabled(isImporting || engine.state != ENGINEVISION_IDLE)
                    }
                    if isImporting {
                        ProgressView(value: assets.total > 0 ? Double(assets.copied) : 0,
                                     total: assets.total > 0 ? Double(assets.total) : 1)
                        Text(assets.currentFile).font(.caption2.monospaced()).lineLimit(3)
                        if assets.total > 0 {
                            Text("\(Int(100 * Double(assets.copied) / Double(assets.total)))% prepared on this headset")
                                .font(.caption.monospacedDigit())
                        }
                    }
                    Text(assets.detail).font(.caption).foregroundStyle(isReady ? .green : .secondary).textSelection(.enabled)
                    if let receipt = assets.receipt {
                        Text("\(receipt.fileCount) files · \(ByteCountFormatter.string(fromByteCount: receipt.totalBytes, countStyle: .file))")
                            .font(.caption2.monospacedDigit())
                    }

                    Button("Start Halo", systemImage: "play.fill") { engine.start(root: assets.rootURL) }
                        .buttonStyle(.borderedProminent).disabled(!isReady || !engine.canStart)
                    if engine.isActive {
                        Button("Stop Halo", systemImage: "stop.fill", role: .destructive) { engine.stop() }
                    }
                    Button(immersive.active ? "Leave immersive view" : "Enter immersive Halo", systemImage: "visionpro") {
                        Task { await toggleImmersive() }
                    }.disabled(immersive.opening || engine.frameSequence == 0)
                    if immersive.active { Button("Recenter view", systemImage: "scope") { immersive.recenter() } }
                    Button("Presentation settings", systemImage: "slider.horizontal.3") { openWindow(id: "HaloSettings") }
                    Text(immersive.status).font(.caption).foregroundStyle(.secondary)
                    if assets.hasBundledPayload, case .failed = assets.phase {
                        Button("Retry local setup") { assets.prepareBundledGame() }
                    }
                    Divider()
                    Label(engine.controllerStatus, systemImage: "gamecontroller.fill").font(.caption)
                    DisclosureGroup("PS5 controller controls") {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Left stick: move · Right stick: look")
                            Text("Cross: jump / select · Circle: crouch / back")
                            Text("Square: use / reload · Triangle: switch weapon")
                            Text("R2: fire · L2: zoom · L1: grenade · R1: melee")
                            Text("D-pad: ↑ flashlight · ↓ grenade type · ← reload · → pick up weapon")
                            Text("Options: pause / skip cinematic")
                            Text("Press Square when Halo asks for E to exit the cryotube.")
                        }.font(.caption).padding(.top, 8)
                    }
                    DisclosureGroup("Diagnostics") {
                        VStack(alignment: .leading, spacing: 8) {
                            Text(engine.status).textSelection(.enabled)
                            Text(engine.frameSize)
                            Text("Frame \(engine.frameSequence)")
                            Text(engine.diagnosticsStatus)
                        }.font(.caption2).foregroundStyle(.secondary).padding(.top, 8)
                    }
                    if let report = engine.diagnostics.reportURL {
                        ShareLink(item: report) { Label("Share test report", systemImage: "square.and.arrow.up") }.font(.caption)
                    }
                    Text("Play reclined with the controller. Your head position does not control aiming or movement.")
                        .font(.caption2).foregroundStyle(.secondary)
                }.padding(24)
            }
            .frame(width: 340)
            .background(.black.opacity(0.18))

            ZStack {
                EngineFrameView()
                if engine.frameSequence == 0 {
                    VStack(spacing: 12) {
                        ProgressView().opacity(engine.isActive ? 1 : 0)
                        Text(engine.isActive ? "Starting Halo…" : "Preparing Halo for this headset…")
                            .font(.headline).multilineTextAlignment(.center)
                    }.padding(30).background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 18))
                }
                VStack { Spacer(); HStack { Text(stateName); Spacer(); Text(engine.frameSize) }
                        .font(.caption.monospacedDigit()).padding(12).background(.black.opacity(0.55)) }
            }.padding(16)
        }
        .frame(minWidth: 1050, minHeight: 700)
        .handlesGameControllerEvents(matching: .gamepad)
        .fileImporter(isPresented: $showingImporter, allowedContentTypes: [.folder], allowsMultipleSelection: false) { result in
            do { if let folder = try result.get().first { assets.importFolder(folder) } }
            catch { importAlert = error.localizedDescription }
        }
        .alert("Game import failed", isPresented: Binding(get: { importAlert != nil }, set: { if !$0 { importAlert = nil } })) {
            Button("OK", role: .cancel) { importAlert = nil }
        } message: { Text(importAlert ?? "Unknown import failure") }
        .onAppear { EngineImmersiveTrace.shared.record("setup.appear") }
        .onDisappear { EngineImmersiveTrace.shared.record("setup.disappear") }
        .onChange(of: immersive.closeRequest) { _, _ in
            let generation = immersive.closeGeneration
            Task {
                guard immersive.canDismissClosedSession(generation) else { return }
                EngineImmersiveTrace.shared.record("setup.dismissImmersive.begin (closeRequest)")
                await dismissImmersiveSpace()
                EngineImmersiveTrace.shared.record("setup.dismissImmersive.end (closeRequest)")
            }
        }
        .onChange(of: engine.frameSequence) { _, sequence in
            // This window only exists when the game could not be the launch
            // scene; it opens the game itself once the engine has a frame,
            // once per showing, so nobody has to press anything.
            guard sequence > 0, !autoOpened, !immersive.active, !immersive.opening else { return }
            autoOpened = true
            NSLog("[immersive] auto-open at engine frame %llu", sequence)
            Task { await ensureImmersiveOpen() }
        }
    }

    private func toggleImmersive() async {
        NSLog("[immersive] toggle: active %d opening %d", immersive.active ? 1 : 0, immersive.opening ? 1 : 0)
        if immersive.active {
            EngineImmersiveTrace.shared.record("setup.dismissImmersive.begin (toggle)")
            let generation = immersive.operationGeneration
            await dismissImmersiveSpace()
            EngineImmersiveTrace.shared.record("setup.dismissImmersive.end (toggle)")
            if immersive.ownsOperation(generation) { immersive.finish() }
            return
        }
        await ensureImmersiveOpen()
    }

    private func ensureImmersiveOpen() async {
        guard let generation = immersive.prepare() else { return }
        EngineImmersiveTrace.shared.record("setup.openImmersive.begin")
        let result = await openImmersiveSpace(id: EngineImmersiveSession.spaceID)
        EngineImmersiveTrace.shared.record("setup.openImmersive.end: \(result)")
        guard immersive.ownsOperation(generation) else { return }
        switch result {
        case .opened: break
        case .userCancelled: immersive.finish(); immersive.status = "Immersive screen opening was cancelled."
        case .error: immersive.finish(); immersive.status = "visionOS could not open the immersive screen."
        @unknown default: immersive.finish()
        }
    }
}
