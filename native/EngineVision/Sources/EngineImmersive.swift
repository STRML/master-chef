import SwiftUI
import Combine
import Metal
import CompositorServices
import ARKit
import QuartzCore
import simd
import ImageIO

struct EngineImmersiveLayerConfiguration: CompositorLayerConfiguration {
    /// Why tracking areas are on or off this launch, for the report.
    nonisolated(unsafe) static var trackingDecision = "off"
    static let trackingAttemptedKey = "HaloTrackingAreasAttempted"
    static let trackingSucceededKey = "HaloTrackingAreasSucceeded"

    func makeConfiguration(capabilities: LayerRenderer.Capabilities, configuration: inout LayerRenderer.Configuration) {
        EngineImmersiveTrace.shared.record("configuration.begin")
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        // Foveation was switched off early, after a report of stale
        // peripheral tiles, when the screen was a 192-triangle cylinder and
        // full resolution cost little. It is now eight densely divided panels
        // over the whole sphere, sampled for both eyes at every display frame,
        // and the headset measured 43.5 Hz on a 90 Hz display: fragment work
        // is what it is short of, and this is what foveation is for. The
        // rate map is applied per pass in encode(). If the stale tiles come
        // back, that report is what to look at, not this line.
        configuration.isFoveationEnabled = capabilities.supportsFoveation
        let layouts = capabilities.supportedLayouts(options: [])
        configuration.layout = layouts.contains(.dedicated) ? .dedicated : (layouts.contains(.layered) ? .layered : .shared)
        configuration.defaultDepthRange = SIMD2(100, max(0.05, capabilities.supportedMinimumNearPlaneDistance))
        // Tracking areas: an 8-bit identifier per pixel that the system reads
        // to draw its own hover glow where the eyes rest, and to say which
        // area a pinch chose. This is the only eye tracking an app gets.
        //
        // Build64 asked for them with an explicit usage and the scene aborted
        // at launch with "Layer configuration not supported", although the
        // format was listed as supported. So: the usage is left at its
        // default, the configuration is validated the documented way
        // (LayerRenderer.Properties throws for an unsupported one) before it
        // is kept, and a launch that asks for tracking areas records the
        // attempt; only a frame actually presented clears it, so a second
        // failure of any kind runs the next launch without them.
        Self.trackingDecision = "off"
        if #available(visionOS 26.0, *) {
            let defaults = UserDefaults.standard
            let attempted = defaults.bool(forKey: Self.trackingAttemptedKey)
            let succeeded = defaults.bool(forKey: Self.trackingSucceededKey)
            // Off unless asked for: on this headset (xrOS 26.3) the request
            // passed the validator and the layer still never started, so the
            // game sat behind its setup window. HALO_EYE_MENU=1 tries again.
            if ProcessInfo.processInfo.environment["HALO_EYE_MENU"] != "1" {
                Self.trackingDecision = "off (HALO_EYE_MENU not set)"
            } else if attempted && !succeeded {
                Self.trackingDecision = "off (the previous launch with tracking areas never presented a frame)"
            } else if !capabilities.supportedTrackingAreasFormats.contains(.r8Uint) {
                Self.trackingDecision = "off (r8Uint not offered: \(capabilities.supportedTrackingAreasFormats.map { $0.rawValue }))"
            } else {
                defaults.set(true, forKey: Self.trackingAttemptedKey); defaults.set(false, forKey: Self.trackingSucceededKey)
                defaults.synchronize()
                let usage = configuration.trackingAreasUsage
                configuration.trackingAreasFormat = .r8Uint
                do {
                    _ = try LayerRenderer.Properties(configuration: configuration)
                    Self.trackingDecision = "on (usage \(usage.rawValue))"
                } catch {
                    configuration.trackingAreasFormat = .invalid
                    configuration.trackingAreasUsage = usage
                    Self.trackingDecision = "off (configuration rejected: \(error))"
                }
            }
        }
        EngineImmersiveTrace.shared.record("configuration.end: tracking=\(Self.trackingDecision)")
        NSLog("[immersive] tracking areas: %@", Self.trackingDecision)
    }
}

struct EngineImmersiveSubmission: Sendable {
    var mode = "inactive"
    var sequence: UInt64 = 0
    var sourceEpoch: UInt64 = 0
    var producerEpoch: UInt64 = 0
    var sceneEpoch: UInt64 = 0
    var layerEpochs: [UInt64] = []
    /// What layer alignment did (EngineLayerAlignment): per-layer turns for
    /// the last engine frame, and running sums for the timeline.
    var layerAlignment = EngineLayerAlignmentStatistics()
    var worldCadence: [String: Double] = [:]
    var sourceFlatSequence: UInt64 = 0
    var latestFlatSequence: UInt64 = 0
    var poolSlot = -1
    var sourceStatus: UInt32 = 0
    var failureReason: UInt32 = 0
    var eyeProjections: [[Float]] = []
    var neutralFromEyes: [[Float]] = []
    var neutralOriginFromHead: [Float] = []
}

final class EngineImmersiveControl: @unchecked Sendable {
    let lifecycleID = UUID().uuidString
    private var loading: EngineLoadingState
    private let lock = NSLock()
    private var stopped = false
    private var recenterGeneration = 0
    private var submittedFrames: UInt64 = 0
    private var cancelledFrames: UInt64 = 0
    private var trackingLossFrames: UInt64 = 0
    private var gpuCompletedFrames: UInt64 = 0
    private var gpuFailedFrames: UInt64 = 0
    private var lastGPUError = ""
    private var rendererConfiguration = "pending"
    private var lastSubmission = EngineImmersiveSubmission()
    private var worldCadence = EngineWorldCadence()
    init(loading: EngineLoadingState) { self.loading = loading }
    func read() -> (stopped: Bool, recenter: Int, loading: EngineLoadingState) {
        lock.lock(); defer { lock.unlock() }
        return (stopped, recenterGeneration, loading)
    }
    func stop() {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        stopped = true
        // Fatal renderer errors also release pending menu input. Repeated
        // stops from an old renderer must not clear its replacement's taps.
        enginevision_pointer(-1, -1, 0, -1)
    }
    func pointer(_ u: Float, _ v: Float, _ onPanel: Int32, _ action: Int32) {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        // Session replacement calls stop before resetting global input. A
        // superseded renderer can therefore neither enqueue nor clear taps.
        enginevision_pointer(u, v, onPanel, action)
    }
    func recenter() { lock.lock(); recenterGeneration += 1; lock.unlock() }
    func setLoading(_ state: EngineLoadingState) {
        lock.lock(); loading = state; lock.unlock()
    }
    func didCancelFrame() { lock.lock(); cancelledFrames += 1; lock.unlock() }
    func didSubmitFrame(trackingMissing: Bool, submission: EngineImmersiveSubmission) {
        lock.lock(); submittedFrames += 1
        lastSubmission = submission
        worldCadence.record(scene: submission.sceneEpoch, epoch: submission.sourceEpoch,
                            time: ProcessInfo.processInfo.systemUptime,
                            eligible: !trackingMissing && (submission.mode == "panorama" || submission.mode == "retainedPanorama"))
        if trackingMissing { trackingLossFrames += 1 }
        lock.unlock()
    }
    func submissionInfo() -> EngineImmersiveSubmission {
        // The compositor's render thread takes this lock in didSubmitFrame
        // between commit and endSubmission. Copy under it (the sample arrays
        // are shared copy-on-write) and sort and count outside it.
        lock.lock()
        var snapshot = lastSubmission
        let cadence = worldCadence
        lock.unlock()
        snapshot.worldCadence = cadence.statistics
        return snapshot
    }
    func configured(_ description: String) { lock.lock(); rendererConfiguration = description; lock.unlock() }
    /// GPU time per compositor frame, in milliseconds: a running average and
    /// the worst seen. The display gives each frame 11.1 ms at 90 Hz; a
    /// compositor that takes longer is silently halved to 45, which is what
    /// the headset measured. This is the number that decides it.
    private var gpuMillisecondsAverage: Double = 0
    private var gpuMillisecondsMax: Double = 0
    func didCompleteGPU(error: String?, gpuMilliseconds: Double = 0) {
        lock.lock(); defer { lock.unlock() }
        if let error { gpuFailedFrames += 1; lastGPUError = error }
        else {
            gpuCompletedFrames += 1
            if gpuMilliseconds.isFinite && gpuMilliseconds >= 0 {
                gpuMillisecondsAverage = gpuMillisecondsAverage == 0 ? gpuMilliseconds
                    : gpuMillisecondsAverage * 0.98 + gpuMilliseconds * 0.02
                gpuMillisecondsMax = max(gpuMillisecondsMax, gpuMilliseconds)
            }
        }
    }
    func frameCounts() -> (submitted: UInt64, cancelled: UInt64, trackingLoss: UInt64,
                           gpuCompleted: UInt64, gpuFailed: UInt64, gpuError: String, configuration: String,
                           gpuMilliseconds: Double, gpuMillisecondsMax: Double) {
        lock.lock(); defer { lock.unlock() }
        return (submittedFrames, cancelledFrames, trackingLossFrames, gpuCompletedFrames, gpuFailedFrames,
                lastGPUError, rendererConfiguration, gpuMillisecondsAverage, gpuMillisecondsMax)
    }
}

@MainActor
final class EngineImmersiveSession: ObservableObject {
    static let spaceID = "HaloEngineImmersive"
    @Published var active = false
    @Published var opening = false
    @Published var status = "Immersive Halo is ready."
    @Published var closeRequest = 0
    private var ownership = EngineImmersiveOwnership()
    var operationGeneration: UInt64 { ownership.generation }
    private(set) var closeGeneration: UInt64 = 0
    private var control: EngineImmersiveControl?
    private weak var diagnosticLayer: LayerRenderer?
    var layerStateForDiagnostics: UInt32? { diagnosticLayer?.state.rawValue }
    private var latestLoading = EngineLoadingState(title: "HALO", detail: "Preparing the original engine…", progress: nil, show: false)

    func setLoading(title: String, detail: String, progress: Double?, show: Bool) {
        let state = EngineLoadingState(title: title, detail: detail, progress: progress, show: show)
        guard state != latestLoading else { return }
        latestLoading = state
        control?.setLoading(state)
    }

    func prepare() -> UInt64? {
        guard let token = ownership.beginOpen(active: active, opening: opening) else { return nil }
        EngineImmersiveTrace.shared.record("session.prepare")
        opening = true; status = "Opening immersive Halo…"
        return token
    }
    func ownsOperation(_ token: UInt64) -> Bool { ownership.owns(token) }
    func canDismissClosedSession(_ token: UInt64) -> Bool {
        ownership.canDismiss(token, active: active, opening: opening)
    }
    func start(layer: LayerRenderer) {
        NSLog("[immersive] session start")
        self.control?.stop()
        enginevision_pointer(-1, -1, 0, -1) // discard input from the previous session
        let generation = ownership.advance()
        let control = EngineImmersiveControl(loading: latestLoading); self.control = control
        diagnosticLayer = layer
        EngineImmersiveTrace.shared.record("session.start", id: control.lifecycleID, state: layer.state.rawValue)
        active = true; opening = false
        Task {
            do {
                EngineImmersiveTrace.shared.record("renderer.init.begin", id: control.lifecycleID, state: layer.state.rawValue)
                let renderer = try EngineImmersiveRenderer(layer: layer, control: control) { [weak self] message in
                    Task { @MainActor in
                        guard self?.control === control else { return }
                        self?.status = message
                    }
                }
                EngineImmersiveTrace.shared.record("renderer.init.end", id: control.lifecycleID, state: layer.state.rawValue)
                EngineImmersiveTrace.shared.record("arkit.start.begin", id: control.lifecycleID, state: layer.state.rawValue)
                try await renderer.startTracking()
                EngineImmersiveTrace.shared.record("arkit.start.end", id: control.lifecycleID, state: layer.state.rawValue)
                guard self.control === control, !control.read().stopped else {
                    renderer.stopTracking()
                    return
                }
                Thread.detachNewThread { [weak self] in
                    renderer.run()
                    EngineImmersiveTrace.shared.record("renderLoop.exit", id: control.lifecycleID, state: layer.state.rawValue)
                    NSLog("[immersive] renderer loop ended: layer state %d, stopped %d", layer.state.rawValue, control.read().stopped ? 1 : 0)
                    Task { @MainActor in
                        guard self?.control === control else { return }
                        EngineImmersiveTrace.shared.record("session.closeRequest", id: control.lifecycleID, state: layer.state.rawValue)
                        self?.closeGeneration = generation
                        self?.active = false; self?.opening = false; self?.control = nil; self?.closeRequest += 1
                    }
                }
            } catch {
                guard self.control === control else { return }
                EngineImmersiveTrace.shared.record("renderer.failed: \(error.localizedDescription)", id: control.lifecycleID, state: layer.state.rawValue)
                NSLog("[immersive] renderer failed: %@", error.localizedDescription)
                status = "Immersive renderer failed: \(error.localizedDescription)"
                closeGeneration = generation
                active = false; opening = false; self.control = nil; closeRequest += 1
            }
        }
    }
    func recenter() { control?.recenter(); status = "Recentering Halo on the current reclined pose…" }
    func finish() {
        _ = ownership.advance()
        control?.stop()
        enginevision_pointer(-1, -1, 0, -1)
        EngineImmersiveTrace.shared.record("session.finish", id: control?.lifecycleID ?? "none", state: diagnosticLayer?.state.rawValue)
        NSLog("[immersive] session finish"); control?.stop(); control = nil; active = false; opening = false
    }
    func submissionInfo() -> EngineImmersiveSubmission { control?.submissionInfo() ?? EngineImmersiveSubmission() }
    func frameCounts() -> (submitted: UInt64, cancelled: UInt64, trackingLoss: UInt64,
                           gpuCompleted: UInt64, gpuFailed: UInt64, gpuError: String, configuration: String,
                           gpuMilliseconds: Double, gpuMillisecondsMax: Double) {
        control?.frameCounts() ?? (0, 0, 0, 0, 0, "", "inactive", 0, 0)
    }
}

final class EngineImmersiveRenderer: @unchecked Sendable {
    private typealias Vertex = EngineImmersiveScreenVertex
    private let layer: LayerRenderer
    private let control: EngineImmersiveControl
    private let report: @Sendable (String) -> Void
    private let queue: MTLCommandQueue
    private let pipeline: MTLRenderPipelineState
    /// Same shaders as `pipeline`, with blending on so the panorama bands can
    /// cross-fade into one another across their overlap.
    private let panoramaPipeline: MTLRenderPipelineState
    /// The same panels turned per layer to meet the newest centre camera,
    /// with their fades computed per fragment (EngineLayerAlignment). Built
    /// off the render thread the first time the setting is on, so a session
    /// with it off (the default) never compiles it; until it is ready, or if
    /// it fails, the panels are drawn as before.
    private let alignedPipeline: EngineAlignedPipeline
    /// The cameras the panel meshes were built from, which the aligned
    /// fades are measured against.
    private var panoramaPanelSources: [EngineImmersiveScreenGeometry.PanoramaSource] = []
    private var alignment = EngineLayerAlignmentState()
    private let backdropPipeline: MTLRenderPipelineState
    private let overlayPipeline: MTLRenderPipelineState
    /// The surround mesh and the content extent it was built for.
    private var backdropMesh: (vertices: MTLBuffer, indices: MTLBuffer, count: Int)?
    private var backdropExtent = SIMD2<Float>(0, 0)
    private let haptics = EngineHaptics()
    /// Surround texture for the frame being encoded; set before encode().
    private var backdropTexture: MTLTexture?
    private var backdropTextureRight: MTLTexture?
    private let overlayDepth: MTLDepthStencilState
    private let panorama: EnginePanoramaTexture
    /// The five source pictures as panels on one sphere, in source order:
    /// left band, centre band, right band, sky, floor.
    private var panoramaPanels: [(vertices: MTLBuffer, indices: MTLBuffer, count: Int)] = []
    private var panoramaPanelSignature: [SIMD4<Float>] = []
    /// Zenith and nadir surfaces, rebuilt when their projection changes.
    private var panoramaActive = false
    private let sampleCount: Int
    /// Eye tracking for the menus. Every text row of the interface layer is
    /// a tracking area: the system glows the row the eyes are on and names
    /// it in the pinch. Rows are found on the GPU from the interface's own
    /// pixels each frame (hud_rows, hud_bands), so no menu layout is
    /// hard-coded; the identifier of a row is its order from the top.
    private let trackingEnabled: Bool
    private var rowsPipeline: MTLComputePipelineState?
    private var bandsPipeline: MTLComputePipelineState?
    private var trackingRows: MTLBuffer?, trackingBands: MTLBuffer?
    private var trackingInfo: [MTLBuffer] = [], trackingInfoIndex = 0
    private var trackingHeight = 0
    private var trackingValues = [UInt32](repeating: 0, count: 33)
    private var trackingActive = false
    private var trackingSucceededMarked = false
    private let depth: MTLDepthStencilState
    /// The panels all lie on one sphere, so their depths are equal to within
    /// rounding and a depth test could drop whichever arrived second. Their
    /// draw order is what decides them, so the test is disabled outright.
    private let panoramaDepth: MTLDepthStencilState
    private var vertices: MTLBuffer
    private var imageAspect: Float = 1024 / 640
    private var hudVertices: MTLBuffer
    /// The interface's shadow surface, behind and slightly offset from it.
    private var hudShadowVertices: MTLBuffer
    private var hudImageAspect: Float = 1024 / 640
    private let indices: MTLBuffer
    private let indexCount: Int
    private let frame: EngineFrameTexture
    private let loadingTexture: EngineImmersiveLoadingTexture
    private let black: MTLTexture
    private let tracking = WorldTrackingProvider()
    /// Gaze selection comes from the compositor's spatial events. Raw hand
    /// joints neither provide gaze nor improve a system-recognized pinch;
    /// requesting their authorization must not gate the renderer's startup.
    /// The scripted head pose for simulator captures (HALO_SIM_HEAD), if any.
    private static let scriptedHead: (yaw: Float, pitch: Float)? = {
        guard let raw = ProcessInfo.processInfo.environment["HALO_SIM_HEAD"] else { return nil }
        let parts = raw.split(separator: ",").compactMap { Float($0.trimmingCharacters(in: .whitespaces)) }
        guard parts.count == 2, parts.allSatisfy({ $0.isFinite }) else { return nil }
        return (parts[0] * .pi / 180, parts[1] * .pi / 180)
    }()
    private let pinchLock = NSLock()
    private var menuInput = EngineMenuInput<SpatialEventCollection.Event.ID>()
    private let arSession = ARKitSession()
    private var neutralOriginFromHead: simd_float4x4?
    private var lastRecenter = -1
    // The first valid panorama is the gameplay presentation baseline. Keep
    // loading/menu frames from fixing the world to a transient head pose.
    private var didAutoRecenterPanorama = false
    private var trackingWasAvailable = false

    init(layer: LayerRenderer, control: EngineImmersiveControl,
         report: @escaping @Sendable (String) -> Void) throws {
        self.layer = layer; self.control = control; self.report = report
        let device = layer.device
        sampleCount = EngineImmersiveRenderTargets.sampleCount(device: device)
        guard let queue = device.makeCommandQueue() else { throw RendererError("Metal command queue is unavailable.") }
        self.queue = queue; frame = EngineFrameTexture(device: device)
        panorama = EnginePanoramaTexture(device: device)
        loadingTexture = EngineImmersiveLoadingTexture(device: device)
        let mesh = EngineImmersiveScreenGeometry.make(aspect: imageAspect)
        let hudMesh = EngineImmersiveScreenGeometry.hud(aspect: hudImageAspect)
        let shadowMesh = EngineImmersiveScreenGeometry.hudShadow(aspect: hudImageAspect)
        guard let vertices = device.makeBuffer(bytes: mesh.vertices, length: mesh.vertices.count * MemoryLayout<Vertex>.stride),
              let hudVertices = device.makeBuffer(bytes: hudMesh.vertices, length: hudMesh.vertices.count * MemoryLayout<Vertex>.stride),
              let hudShadowVertices = device.makeBuffer(bytes: shadowMesh.vertices, length: shadowMesh.vertices.count * MemoryLayout<Vertex>.stride),
              let indices = device.makeBuffer(bytes: mesh.indices, length: mesh.indices.count * MemoryLayout<UInt32>.stride) else {
            throw RendererError("Unable to allocate the 180° screen mesh.")
        }
        self.vertices = vertices; self.hudVertices = hudVertices; self.hudShadowVertices = hudShadowVertices
        self.indices = indices; indexCount = mesh.indices.count
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: 1, height: 1, mipmapped: false)
        descriptor.usage = .shaderRead
        guard let black = device.makeTexture(descriptor: descriptor) else { throw RendererError("Unable to allocate the fallback frame texture.") }
        var pixel: UInt32 = 0xFF000000
        black.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: &pixel, bytesPerRow: 4)
        self.black = black
        let library = try device.makeLibrary(source: Self.shader, options: nil)
        var trackingFormat = MTLPixelFormat.invalid
        if #available(visionOS 26.0, *) { trackingFormat = layer.configuration.trackingAreasFormat }
        let trackingEnabled = trackingFormat == .r8Uint && sampleCount == 1
        self.trackingEnabled = trackingEnabled
        if trackingEnabled, let rows = library.makeFunction(name: "hud_rows"), let bands = library.makeFunction(name: "hud_bands") {
            rowsPipeline = try? device.makeComputePipelineState(function: rows)
            bandsPipeline = try? device.makeComputePipelineState(function: bands)
        }
        let pipelineDescriptor = MTLRenderPipelineDescriptor()
        pipelineDescriptor.vertexFunction = library.makeFunction(name: "curve_vertex")
        pipelineDescriptor.fragmentFunction = library.makeFunction(name: "curve_fragment")
        pipelineDescriptor.colorAttachments[0].pixelFormat = layer.configuration.colorFormat
        pipelineDescriptor.depthAttachmentPixelFormat = layer.configuration.depthFormat
        pipelineDescriptor.rasterSampleCount = sampleCount
        pipelineDescriptor.maxVertexAmplificationCount = max(1, layer.properties.viewCount)
        if trackingEnabled {
            // Only the interface writes identifiers; the other passes leave
            // the attachment alone.
            pipelineDescriptor.colorAttachments[1].pixelFormat = trackingFormat
            pipelineDescriptor.colorAttachments[1].writeMask = []
        }
        pipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
        pipelineDescriptor.colorAttachments[0].isBlendingEnabled = true
        pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
        pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = .one
        pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        panoramaPipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
        // Layer alignment is a second library built from the same source plus
        // its own functions, with the panorama's blending, so that anything
        // wrong with it can only switch alignment off, never the renderer.
        // It is compiled only once the setting is first on, in the background
        // (the whole base shader again: about 200 ms on a Mac), from a copy of
        // the panorama's descriptor as it stands here.
        alignedPipeline = EngineAlignedPipeline(device: device, source: Self.shader + EngineLayerAlignment.shaderSource,
                                                descriptor: pipelineDescriptor.copy() as! MTLRenderPipelineDescriptor)
        pipelineDescriptor.colorAttachments[0].isBlendingEnabled = false
        pipelineDescriptor.fragmentFunction = library.makeFunction(name: "backdrop_fragment")
        backdropPipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
        pipelineDescriptor.fragmentFunction = library.makeFunction(name: trackingEnabled ? "hud_fragment_tracked" : "hud_fragment")
        if trackingEnabled { pipelineDescriptor.colorAttachments[1].writeMask = .all }
        pipelineDescriptor.colorAttachments[0].isBlendingEnabled = true
        pipelineDescriptor.colorAttachments[0].sourceRGBBlendFactor = .one
        pipelineDescriptor.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        pipelineDescriptor.colorAttachments[0].sourceAlphaBlendFactor = .one
        pipelineDescriptor.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        overlayPipeline = try device.makeRenderPipelineState(descriptor: pipelineDescriptor)
        let overlayDescriptor = MTLDepthStencilDescriptor()
        overlayDescriptor.depthCompareFunction = .always
        overlayDescriptor.isDepthWriteEnabled = false
        guard let overlayDepth = device.makeDepthStencilState(descriptor: overlayDescriptor) else { throw RendererError("Unable to create HUD depth state.") }
        self.overlayDepth = overlayDepth
        let depthDescriptor = MTLDepthStencilDescriptor(); depthDescriptor.depthCompareFunction = .greater
        depthDescriptor.isDepthWriteEnabled = true
        guard let depth = device.makeDepthStencilState(descriptor: depthDescriptor) else { throw RendererError("Unable to create immersive depth state.") }
        self.depth = depth
        depthDescriptor.depthCompareFunction = .always
        guard let panoramaDepth = device.makeDepthStencilState(descriptor: depthDescriptor) else { throw RendererError("Unable to create panorama depth state.") }
        self.panoramaDepth = panoramaDepth
        control.configured("layout=\(layer.configuration.layout) foveation=\(layer.configuration.isFoveationEnabled) msaa=\(sampleCount) views=\(layer.properties.viewCount) tracking=\(trackingEnabled ? EngineImmersiveLayerConfiguration.trackingDecision : "off (\(EngineImmersiveLayerConfiguration.trackingDecision))")")
        // Attached last: the closure captures self, which must be whole first.
        layer.onSpatialEvent = { [weak self] events in self?.handle(spatial: events) }
    }

    func stopTracking() { arSession.stop() }

    func startTracking() async throws {
        guard WorldTrackingProvider.isSupported else { throw RendererError("World tracking is unavailable on this device.") }
        try await arSession.run([tracking])
    }

    private func handle(spatial events: SpatialEventCollection) {
        pinchLock.lock(); defer { pinchLock.unlock() }
        guard !control.read().stopped else {
            menuInput.setEnabled(false)
            return
        }
        // Keep consuming terminal events while a menu is hidden: they retire
        // suppressed IDs, which the system may reuse for later interactions.
        if enginevision_menu_active() == 0 { menuInput.setEnabled(false) }
        for event in events {
            // An indirect pinch is the system's look-and-pinch interaction.
            // A pointer event also supports a physical mouse / simulator.
            guard event.kind == .indirectPinch || event.kind == .pointer else { continue }
            var target: EngineMenuInput<SpatialEventCollection.Event.ID>.Target?
            if let ray = event.selectionRay {
                var value = EngineMenuInput<SpatialEventCollection.Event.ID>.Target(
                    origin: SIMD3(Float(ray.origin.x), Float(ray.origin.y), Float(ray.origin.z)),
                    direction: SIMD3(Float(ray.direction.x), Float(ray.direction.y), Float(ray.direction.z)))
                if #available(visionOS 26.0, *) { value.trackingArea = event.trackingAreaIdentifier.rawValue }
                target = value
            }
            switch event.phase {
            case .active: menuInput.update(id: event.id, phase: .active, target: target)
            case .ended: menuInput.update(id: event.id, phase: .ended, target: target)
            case .cancelled: menuInput.update(id: event.id, phase: .cancelled, target: nil)
            @unknown default: menuInput.update(id: event.id, phase: .cancelled, target: nil)
            }
        }
    }

    private func resetMenuInput() {
        pinchLock.lock(); menuInput.setEnabled(false); pinchLock.unlock()
        control.pointer(-1, -1, 0, -1) // HOST_POINTER_CANCEL
    }

    /// One tracking area per text row of the interface, registered every
    /// frame the menu is up. The identifier is the row's order from the top.
    private func registerTrackingAreas(drawable: LayerRenderer.Drawable, menuActive: Bool, panorama: Bool) {
        trackingActive = false
        for n in trackingValues.indices { trackingValues[n] = 0 }
        guard trackingEnabled, menuActive, panorama else { return }
        if #available(visionOS 26.0, *) {
            for band in 1...32 {
                let area = drawable.addTrackingArea(identifier: .init(rawValue: UInt64(band)))
                area.addHoverEffect(.automatic)
                trackingValues[band] = UInt32(area.renderValue.rawValue)
            }
            trackingActive = true
        }
    }

    struct BandInfo { var rowStart: UInt16 = 0, rowEnd: UInt16 = 0, xMin: UInt16 = 0, xMax: UInt16 = 0 }

    /// The rows of the interface as last measured: the band a pinch names,
    /// two frames old at most.
    private func trackingBand(_ identifier: UInt64) -> BandInfo? {
        guard identifier >= 1, identifier <= 32, trackingInfo.count == 2, trackingHeight > 0 else { return nil }
        let previous = trackingInfo[(trackingInfoIndex + 1) % 2]
        let info = previous.contents().bindMemory(to: BandInfo.self, capacity: 33)[Int(identifier)]
        guard info.rowEnd >= info.rowStart, info.xMax >= info.xMin, info.rowEnd > 0 else { return nil }
        return info
    }

    /// Find the interface's text rows on the GPU: per row, whether any pixel
    /// is lit and its extent; then runs of lit rows become bands 1...32.
    private func encodeBands(command: MTLCommandBuffer, hud: MTLTexture) {
        guard let rowsPipeline, let bandsPipeline else { trackingActive = false; return }
        let height = hud.height
        if trackingHeight != height || trackingRows == nil {
            trackingRows = layer.device.makeBuffer(length: max(1, height) * 4, options: .storageModePrivate)
            trackingBands = layer.device.makeBuffer(length: max(1, height) * 2, options: .storageModePrivate)
            trackingInfo = (0..<2).compactMap { _ in layer.device.makeBuffer(length: 33 * MemoryLayout<BandInfo>.stride, options: .storageModeShared) }
            trackingHeight = height
        }
        guard let rows = trackingRows, let bands = trackingBands, trackingInfo.count == 2,
              let encoder = command.makeComputeCommandEncoder() else { trackingActive = false; return }
        trackingInfoIndex = (trackingInfoIndex + 1) % 2
        encoder.setComputePipelineState(rowsPipeline)
        encoder.setTexture(hud, index: 0); encoder.setBuffer(rows, offset: 0, index: 0)
        let group = min(64, rowsPipeline.maxTotalThreadsPerThreadgroup)
        encoder.dispatchThreads(MTLSize(width: height, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: group, height: 1, depth: 1))
        encoder.setComputePipelineState(bandsPipeline)
        encoder.setBuffer(rows, offset: 0, index: 0); encoder.setBuffer(bands, offset: 0, index: 1)
        encoder.setBuffer(trackingInfo[trackingInfoIndex], offset: 0, index: 2)
        var rowCount = UInt32(height); encoder.setBytes(&rowCount, length: 4, index: 3)
        encoder.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
        encoder.endEncoding()
    }

    private var firstFrameSubmitted = false
    private var firstFrameMilestones: Set<String> = []
    private func firstFrameCheckpoint(_ event: String) {
        guard !firstFrameSubmitted, firstFrameMilestones.insert(event).inserted else { return }
        EngineImmersiveTrace.shared.record(event, id: control.lifecycleID, state: layer.state.rawValue)
    }

    func run() {
        EngineImmersiveTrace.shared.record("renderLoop.enter", id: control.lifecycleID, state: layer.state.rawValue)
        var previousState: UInt32?
        defer { arSession.stop() }
        while !control.read().stopped {
            let state = layer.state
            if previousState != state.rawValue {
                EngineImmersiveTrace.shared.record("layer.state", id: control.lifecycleID, state: state.rawValue)
                previousState = state.rawValue
            }
            switch state {
            case .invalidated: resetMenuInput(); return
            case .paused:
                resetMenuInput()
                EngineImmersiveTrace.shared.record("layer.wait.begin", id: control.lifecycleID, state: state.rawValue)
                layer.waitUntilRunning()
                EngineImmersiveTrace.shared.record("layer.wait.end", id: control.lifecycleID, state: layer.state.rawValue)
            case .running: autoreleasepool { drawFrame() }
            @unknown default: return
            }
        }
    }

    private func drawFrame() {
        firstFrameCheckpoint("frame.queryNextFrame")
        guard let layerFrame = layer.queryNextFrame() else { Thread.sleep(forTimeInterval: 0.001); return }
        firstFrameCheckpoint("frame.acquired")
        layerFrame.startUpdate(); layerFrame.endUpdate()
        firstFrameCheckpoint("frame.predictTiming")
        guard let timing = layerFrame.predictTiming() else { return }
        firstFrameCheckpoint("frame.optimalInputWait")
        LayerRenderer.Clock().wait(until: timing.optimalInputTime)
        guard let command = queue.makeCommandBuffer() else {
            report("Unable to allocate a Metal command buffer."); control.stop(); return
        }
        firstFrameCheckpoint("frame.startSubmission")
        layerFrame.startSubmission()
        firstFrameCheckpoint("frame.queryDrawables")
        let drawables: [LayerRenderer.Drawable]
        if #available(visionOS 26.0, *) { drawables = layerFrame.queryDrawables() }
        else { drawables = layerFrame.queryDrawable().map { [$0] } ?? [] }
        // queryDrawables returning [] cancels and invalidates the frame.
        // Per CompositorServices frame.h it MUST NOT be accessed again,
        // including calling endSubmission on it when immersion is dismissed.
        guard !drawables.isEmpty else { control.didCancelFrame(); return }
        firstFrameCheckpoint("frame.drawablesReady")
        let isFirstFrame = !firstFrameSubmitted
        defer {
            layerFrame.endSubmission()
            if isFirstFrame { EngineImmersiveTrace.shared.record("frame.endSubmission", id: control.lifecycleID, state: layer.state.rawValue) }
        }
        let controlState = control.read()
        var panoramaSnapshot: EnginePanoramaTexture.Snapshot?
        if !controlState.loading.show {
            panoramaSnapshot = panorama.refresh(leasedTo: command)
            if panoramaSnapshot == nil && panorama.allowsFlatFallback { _ = frame.refresh(expectedSequence: panorama.latestFlatSequence) }
        }
        panoramaActive = !controlState.loading.show && panoramaSnapshot?.textures.count == Int(HALO_PANORAMA_LAYERS)
        if panoramaActive, let panoramaSnapshot {
            // Every panel depends on every other, because each one's fade is
            // measured against what is already beneath it, so they are all
            // rebuilt together whenever any projection moves.
            // Six bearings round the ring, then the sky and the floor, in the
            // order the geometry indexes its cameras.
            let ring = EnginePanoramaTexture.Projections.ringLayers
            let projections = ring.map { panoramaSnapshot.projections[$0] }
                + [panoramaSnapshot.zenithProjection, panoramaSnapshot.nadirProjection]
            let viewports = ring.map { panoramaSnapshot.projections.viewport($0) }
                + [SIMD4<Float>(0, 0, 1, 1), SIMD4<Float>(0, 0, 1, 1)]
            // Both the projection and the whole viewport rectangle, origin
            // included: a viewport that moves without changing size would
            // otherwise keep the previous texture coordinates.
            let signature = zip(projections, viewports).flatMap { [SIMD4($0.x, $0.y, 0, 0), $1] }
            if signature != panoramaPanelSignature || panoramaPanels.count != ring.count + 2 {
                let sources = EngineImmersiveScreenGeometry.panoramaSources(projections: projections, viewports: viewports)
                let meshes = EngineImmersiveScreenGeometry.sphericalPanorama(
                    sources: sources, texelSize: panoramaSnapshot.projections.texelSize)
                var built: [(vertices: MTLBuffer, indices: MTLBuffer, count: Int)] = []
                for mesh in meshes {
                    guard let v = layer.device.makeBuffer(bytes: mesh.vertices, length: mesh.vertices.count * MemoryLayout<Vertex>.stride),
                          let i = layer.device.makeBuffer(bytes: mesh.indices, length: mesh.indices.count * MemoryLayout<UInt32>.stride)
                    else { built = []; break }
                    built.append((v, i, mesh.indices.count))
                }
                if built.count == ring.count + 2 { panoramaPanels = built; panoramaPanelSignature = signature; panoramaPanelSources = sources }
            }
            if panoramaPanels.count != ring.count + 2 { panoramaActive = false }
        }
        panoramaActive = panoramaActive && panoramaPanels.count == EngineImmersiveScreenGeometry.panoramaAngles.count
        // A failed world pass has an anamorphic center framebuffer and may
        // already have extracted its HUD. Only an explicit flat/menu state
        // may select it; otherwise retain a complete world or show black.
        let flatTexture = panorama.allowsFlatFallback ? (frame.view ?? frame.texture) : nil
        let sourceTexture = loadingTexture.updateIfNeeded(controlState.loading) ?? flatTexture ?? black
        let aspectTexture = panoramaActive ? panoramaSnapshot?.textures.first ?? sourceTexture : sourceTexture
        let aspect = Float(aspectTexture.width) / Float(aspectTexture.height)
        if aspect != imageAspect {
            let mesh = EngineImmersiveScreenGeometry.make(aspect: aspect)
            // Publish a new buffer; earlier GPU submissions may still own the
            // previous loading-card mesh while the first menu frame arrives.
            if let replacement = layer.device.makeBuffer(bytes: mesh.vertices, length: mesh.vertices.count * MemoryLayout<Vertex>.stride) {
                vertices = replacement; imageAspect = aspect
            }
        }
        if aspect != hudImageAspect {
            let mesh = EngineImmersiveScreenGeometry.hud(aspect: aspect)
            let shadow = EngineImmersiveScreenGeometry.hudShadow(aspect: aspect)
            if let replacement = layer.device.makeBuffer(bytes: mesh.vertices, length: mesh.vertices.count * MemoryLayout<Vertex>.stride),
               let shadowReplacement = layer.device.makeBuffer(bytes: shadow.vertices, length: shadow.vertices.count * MemoryLayout<Vertex>.stride) {
                hudVertices = replacement; hudShadowVertices = shadowReplacement; hudImageAspect = aspect
            }
        }
        // The surround follows whichever surface is on screen: the panorama
        // cylinder in game, the curved panel in the menus. Its texture is the
        // centre view (or the menu image), whose border colours it extends.
        let extent: SIMD2<Float> = panoramaActive
            ? SIMD2(Float.pi / 2, EngineImmersiveScreenGeometry.panoramaHalfHeight(
                maxProjectionY: panoramaSnapshot?.projections.maximumY ?? 1))
            : { let e = EngineImmersiveScreenGeometry.menuExtent(aspect: aspect); return SIMD2(e.halfSweep, e.halfHeight) }()
        if extent != backdropExtent || backdropMesh == nil {
            let mesh = EngineImmersiveScreenGeometry.backdrop(halfSweep: extent.x, halfHeight: extent.y)
            if let v = layer.device.makeBuffer(bytes: mesh.vertices, length: mesh.vertices.count * MemoryLayout<Vertex>.stride),
               let i = layer.device.makeBuffer(bytes: mesh.indices, length: mesh.indices.count * MemoryLayout<UInt32>.stride) {
                backdropMesh = (v, i, mesh.indices.count); backdropExtent = extent
            }
        }
        backdropTexture = panoramaActive ? panoramaSnapshot?.view(eye: 0, sector: 1) : sourceTexture
        backdropTextureRight = panoramaActive ? panoramaSnapshot?.view(eye: 1, sector: 1) : sourceTexture
        haptics.pump()
        var trackingMissing = false
        var submitted = EngineImmersiveSubmission()
        submitted.mode = controlState.loading.show ? "loading" : (panoramaActive ?
            ((panoramaSnapshot?.sourceEpoch == panorama.latestSourceEpoch && panorama.sourceStatus == 1) ? "panorama" : "retainedPanorama") :
            (panorama.allowsFlatFallback ? "flat" : "worldUnavailable"))
        submitted.sequence = panoramaActive ? panoramaSnapshot?.sequence ?? 0 : (submitted.mode == "flat" ? frame.sequence : 0)
        submitted.sourceEpoch = panoramaActive ? panoramaSnapshot?.sourceEpoch ?? 0 : 0
        submitted.sceneEpoch = panoramaActive ? panoramaSnapshot?.sceneEpoch ?? 0 : 0
        submitted.layerEpochs = panoramaActive ? panoramaSnapshot?.layerEpochs ?? [] : []
        // Layer alignment: planned once per engine frame (the snapshot's
        // sequence), not per display frame. With the setting off (the
        // default), or its pipeline not built, the panels are drawn exactly
        // as before and only the cameras' offsets are recorded, so a run with
        // it off gives the baseline for an A/B against one with it on.
        var alignmentDraw: (pipeline: MTLRenderPipelineState, constants: [EngineLayerAlignment.PanelConstants])?
        if panoramaActive, let panoramaSnapshot {
            if halo_settings_layer_align() != 0, let pipeline = alignedPipeline.pipeline(),
               let constants = alignment.update(sequence: panoramaSnapshot.sequence, signature: panoramaPanelSignature,
                                                sources: panoramaPanelSources, poses: panoramaSnapshot.layerPoses,
                                                epochs: panoramaSnapshot.layerEpochs, cutEpoch: panoramaSnapshot.cutEpoch) {
                alignmentDraw = (pipeline, constants)
            } else {
                alignment.measure(sequence: panoramaSnapshot.sequence, poses: panoramaSnapshot.layerPoses,
                                  epochs: panoramaSnapshot.layerEpochs, cutEpoch: panoramaSnapshot.cutEpoch)
            }
        } else {
            alignment.disable()
        }
        submitted.layerAlignment = alignment.statistics
        submitted.sourceFlatSequence = panoramaActive ? panoramaSnapshot?.flatSequence ?? 0 : (submitted.mode == "flat" ? frame.sequence : 0)
        submitted.producerEpoch = panorama.latestSourceEpoch
        submitted.latestFlatSequence = panorama.latestFlatSequence
        submitted.poolSlot = panoramaActive ? panoramaSnapshot?.poolSlot ?? -1 : -1
        submitted.sourceStatus = panorama.sourceStatus; submitted.failureReason = panorama.failureReason
        func elements(_ matrix: simd_float4x4) -> [Float] {
            [matrix.columns.0, matrix.columns.1, matrix.columns.2, matrix.columns.3].flatMap { [$0.x,$0.y,$0.z,$0.w] }
        }
        var didUpdateMenuInput = false
        for drawable in drawables {
            let duration = LayerRenderer.Clock.Instant.epoch.duration(to: drawable.frameTiming.presentationTime)
            let timestamp = Double(duration.components.seconds) + Double(duration.components.attoseconds) / 1.0e18
            guard let anchor = tracking.queryDeviceAnchor(atTimestamp: timestamp), anchor.isTracked else {
                // A valid acquired drawable still needs presentation when ARKit
                // has no pose. Present a cleared frame with no reprojection.
                trackingMissing = true
                if !didUpdateMenuInput { resetMenuInput(); didUpdateMenuInput = true }
                drawable.deviceAnchor = nil
                var unusedMatrices: [simd_float4x4] = []
                encode(drawable: drawable, matrices: &unusedMatrices, command: command,
                       texture: black, panoramaSnapshot: nil, drawContent: false)
                drawable.encodePresent(commandBuffer: command)
                continue
            }
            drawable.deviceAnchor = anchor
            let shouldAutoRecenter = panoramaActive && !didAutoRecenterPanorama
            if shouldAutoRecenter || lastRecenter != controlState.recenter {
                neutralOriginFromHead = anchor.originFromAnchorTransform; lastRecenter = controlState.recenter
                if shouldAutoRecenter { didAutoRecenterPanorama = true }
                report(frame.texture == nil ? "180° screen active; waiting for the original engine's first frame." : "Original engine active and centered for reclined viewing.")
            }
            let neutralOriginFromHead = self.neutralOriginFromHead ?? anchor.originFromAnchorTransform
            submitted.neutralOriginFromHead = elements(neutralOriginFromHead)
            // Where the viewer is looking inside the panorama, measured from the
            // recentred forward direction. The mixer pans against this so a
            // sound holds its place in the world when only the head turns.
            var neutralFromHead = simd_inverse(neutralOriginFromHead) * anchor.originFromAnchorTransform
            // HALO_SIM_HEAD=yaw,pitch (degrees) stands in for a head pose the
            // simulator cannot be scripted to take, so a capture can look at
            // any bearing or straight up. Never set on the device.
            if let pose = EngineImmersiveRenderer.scriptedHead {
                let yaw = simd_quatf(angle: pose.yaw, axis: SIMD3(0, 1, 0))
                let pitch = simd_quatf(angle: pose.pitch, axis: SIMD3(1, 0, 0))
                var turned = simd_float4x4(yaw * pitch)
                turned.columns.3 = neutralFromHead.columns.3
                neutralFromHead = turned
            }
            let gaze = -simd_normalize(simd_make_float3(neutralFromHead.columns.2))
            if gaze.x.isFinite && gaze.y.isFinite && gaze.z.isFinite {
                halo_settings_set_head(atan2(gaze.x, -gaze.z), asin(max(-1, min(1, gaze.y))))
            }
            // The head's own right axis is the line between the eyes; how far
            // it rises out of level is the roll the centre passes render with.
            let headRight = simd_normalize(simd_make_float3(neutralFromHead.columns.0))
            if headRight.y.isFinite {
                halo_settings_set_head_roll(asin(max(-1, min(1, headRight.y))))
            }
            let menuActive = enginevision_menu_active() != 0 && !controlState.loading.show
            registerTrackingAreas(drawable: drawable, menuActive: menuActive, panorama: panoramaActive && panoramaSnapshot != nil)
            if !didUpdateMenuInput {
                didUpdateMenuInput = true
                if !menuActive {
                    resetMenuInput()
                } else {
                    let extent = EngineImmersiveScreenGeometry.menuExtent(aspect: hudImageAspect)
                    let neutralFromOrigin = simd_inverse(neutralOriginFromHead)
                    func menuHit(_ target: EngineMenuInput<SpatialEventCollection.Event.ID>.Target) -> (u: Float, v: Float, onPanel: Bool)? {
                        let origin = simd_make_float3(neutralFromOrigin * SIMD4(target.origin, 1))
                        let direction = simd_normalize(simd_make_float3(neutralFromOrigin * SIMD4(target.direction, 0)))
                        var hit = EngineImmersiveScreenGeometry.menuHit(origin: origin, direction: direction,
                            halfSweep: extent.halfSweep, halfHeight: extent.halfHeight)
                        if let band = trackingBand(target.trackingArea), let hudTexture = panoramaSnapshot?.hud {
                            let width = Float(hudTexture.width), height = Float(trackingHeight)
                            let v = (Float(band.rowStart) + Float(band.rowEnd) + 1) / (2 * height)
                            let uMin = Float(band.xMin) / width, uMax = Float(band.xMax) / width
                            var u = hit?.u ?? 0.5
                            if !(hit?.onPanel ?? false) || u < uMin - 0.02 || u > uMax + 0.02 { u = (uMin + uMax) / 2 }
                            hit = (u: u, v: v, onPanel: true)
                        }
                        return hit
                    }
                    pinchLock.lock()
                    menuInput.setEnabled(true)
                    let input = menuInput.drain()
                    pinchLock.unlock()
                    for target in input.clicks {
                        if let hit = menuHit(target), hit.onPanel {
                            control.pointer(hit.u, hit.v, 1, 2) // HOST_POINTER_TAP, once per successful pinch
                        }
                    }
                    // There is no continuous eye ray between interactions. A
                    // pinch previews its gaze target; head-follow is optional.
                    var preview = input.preview.flatMap { menuHit($0) }
                    if preview == nil, halo_settings_gaze_pointer() != 0 {
                        preview = EngineImmersiveScreenGeometry.menuHit(origin: simd_make_float3(neutralFromHead.columns.3),
                            direction: gaze, halfSweep: extent.halfSweep, halfHeight: extent.halfHeight)
                    }
                    control.pointer(preview?.u ?? -1, preview?.v ?? -1, (preview?.onPanel ?? false) ? 1 : 0, 0)
                }
            }
            var matrices = drawable.views.enumerated().map { index, view -> simd_float4x4 in
                let neutralFromEye = neutralFromHead * view.transform
                let projection = drawable.computeProjection(convention: .rightUpBack, viewIndex: index)
                submitted.eyeProjections.append(elements(projection)); submitted.neutralFromEyes.append(elements(neutralFromEye))
                return projection * simd_inverse(neutralFromEye)
            }
            encode(drawable: drawable, matrices: &matrices, command: command, texture: sourceTexture,
                   panoramaSnapshot: panoramaActive ? panoramaSnapshot : nil, alignment: alignmentDraw)
            lastViewProjection = matrices[0]
            captureIfRequested(drawable: drawable, command: command, snapshot: panoramaActive ? panoramaSnapshot : nil)
            drawable.encodePresent(commandBuffer: command)
        }
        let markTrackingSucceeded = trackingEnabled && !trackingSucceededMarked
        if markTrackingSucceeded { trackingSucceededMarked = true }
        command.addCompletedHandler { [control, report] completed in
            if isFirstFrame {
                EngineImmersiveTrace.shared.record(completed.status == .completed ? "frame.firstGPUCompleted"
                    : "frame.firstGPUFailed: \(completed.error?.localizedDescription ?? "unknown error")", id: control.lifecycleID)
            }
            let error = completed.status == .completed ? nil : (completed.error?.localizedDescription ?? "Metal command buffer did not complete")
            control.didCompleteGPU(error: error, gpuMilliseconds: (completed.gpuEndTime - completed.gpuStartTime) * 1000)
            if let error { report("Immersive GPU error: \(error)") }
            // The first frame through with tracking areas: the next launch may
            // ask for them again.
            if markTrackingSucceeded && error == nil {
                UserDefaults.standard.set(true, forKey: EngineImmersiveLayerConfiguration.trackingSucceededKey)
            }
        }
        firstFrameCheckpoint("frame.commit")
        command.commit()
        firstFrameSubmitted = true
        control.didSubmitFrame(trackingMissing: trackingMissing, submission: submitted)
        if trackingMissing && trackingWasAvailable {
            report("Tracking unavailable; presenting a blank frame until tracking returns.")
        } else if !trackingMissing && !trackingWasAvailable {
            report("Immersive presentation active; tracking is available.")
        }
        trackingWasAvailable = !trackingMissing
    }

    /// HALO_SIM_CAPTURE=1: every sixtieth composited frame's left-eye colour
    /// target is copied out and written as a PNG under Documents/SimCaptures,
    /// so the sphere's joins can be inspected exactly as the compositor made
    /// them, on a simulator whose own screenshot does not show Metal layers.
    /// HALO_SIM_CAPTURE_LAYERS=1 also writes every panorama layer twice, so
    /// what each bearing's camera drew can be compared with its neighbours'.
    private static let captureRequested = ProcessInfo.processInfo.environment["HALO_SIM_CAPTURE"] == "1"
    private static let captureLayersRequested = ProcessInfo.processInfo.environment["HALO_SIM_CAPTURE_LAYERS"] == "1"
    private var captureCounter = 0
    private var lastViewProjection = matrix_identity_float4x4
    private func captureIfRequested(drawable: LayerRenderer.Drawable, command: MTLCommandBuffer,
                                    snapshot: EnginePanoramaTexture.Snapshot?) {
        guard EngineImmersiveRenderer.captureRequested else { return }
        captureCounter += 1
        let sequence = captureCounter
        if sequence % 60 == 0, let first = drawable.views.first {
            writePNG(texture: drawable.colorTextures[first.textureMap.textureIndex], slice: first.textureMap.sliceIndex,
                     name: String(format: "composite-%05d", sequence), command: command)
            let t = first.tangents, v = lastViewProjection
            NSLog("[immersive] view 0 tangents %.4f %.4f %.4f %.4f viewproj %@", t.x, t.y, t.z, t.w,
                  (0..<4).map { c in (0..<4).map { r in String(format: "%.6f", v[c][r]) }.joined(separator: ",") }.joined(separator: ";"))
        }
        if EngineImmersiveRenderer.captureLayersRequested, sequence == 120 || sequence % 600 == 0, let snapshot {
            for (index, texture) in snapshot.textures.enumerated() {
                writePNG(texture: texture, slice: 0, name: String(format: "layer%02d-%05d", index, sequence), command: command)
            }
        }
    }

    private func writePNG(texture source: MTLTexture, slice: Int, name: String, command: MTLCommandBuffer) {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: source.pixelFormat, width: source.width, height: source.height, mipmapped: false)
        descriptor.storageMode = .shared; descriptor.usage = .shaderRead
        guard let copy = layer.device.makeTexture(descriptor: descriptor), let blit = command.makeBlitCommandEncoder() else { return }
        blit.copy(from: source, sourceSlice: slice, sourceLevel: 0, sourceOrigin: MTLOrigin(), sourceSize: MTLSize(width: source.width, height: source.height, depth: 1),
                  to: copy, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
        blit.endEncoding()
        let head = EngineImmersiveRenderer.scriptedHead
        let bgra = source.pixelFormat == .bgra8Unorm || source.pixelFormat == .bgra8Unorm_srgb
        command.addCompletedHandler { _ in
            let width = copy.width, height = copy.height, stride = width * 4
            var bytes = [UInt8](repeating: 0, count: stride * height)
            copy.getBytes(&bytes, bytesPerRow: stride, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
            let info = bgra ? CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
                            : CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue)
            guard let provider = CGDataProvider(data: Data(bytes) as CFData),
                  let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: stride,
                                      space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: info,
                                      provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent) else { return }
            let directory = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appending(path: "SimCaptures")
            try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let label = head.map { String(format: "yaw%.0f-pitch%.0f", $0.yaw * 180 / .pi, $0.pitch * 180 / .pi) } ?? "head"
            let url = directory.appending(path: "\(name)-\(label).png")
            guard let sink = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil) else { return }
            CGImageDestinationAddImage(sink, image, nil); CGImageDestinationFinalize(sink)
            NSLog("[immersive] captured %@ (%dx%d %@)", url.lastPathComponent, width, height, bgra ? "bgra" : "rgba")
        }
    }

    private func encode(drawable: LayerRenderer.Drawable, matrices: inout [simd_float4x4], command: MTLCommandBuffer,
                        texture: MTLTexture, panoramaSnapshot: EnginePanoramaTexture.Snapshot?,
                        alignment: (pipeline: MTLRenderPipelineState, constants: [EngineLayerAlignment.PanelConstants])? = nil,
                        drawContent: Bool = true) {
        if trackingEnabled, let panoramaSnapshot, trackingActive { encodeBands(command: command, hud: panoramaSnapshot.hud) }
        let dedicated = layer.configuration.layout == .dedicated
        for passIndex in 0..<(dedicated ? drawable.views.count : 1) {
            let selected = dedicated ? [drawable.views[passIndex]] : drawable.views
            guard let first = selected.first else { continue }
            let textureIndex = first.textureMap.textureIndex
            let slice = dedicated ? first.textureMap.sliceIndex : 0
            let arrayLength = layer.configuration.layout == .layered ? drawable.views.count : 1
            var trackingTexture: MTLTexture? = nil
            if #available(visionOS 26.0, *), trackingEnabled, textureIndex < drawable.trackingAreasTextures.count {
                trackingTexture = drawable.trackingAreasTextures[textureIndex]
            }
            let pass: MTLRenderPassDescriptor
            var renderContent = drawContent
            do {
                pass = try EngineImmersiveRenderTargets.makePass(device: layer.device,
                    color: drawable.colorTextures[textureIndex], depth: drawable.depthTextures[textureIndex],
                    slice: slice, arrayLength: arrayLength, sampleCount: sampleCount, tracking: trackingTexture)
            } catch {
                // Even allocation failure presents a known cleared drawable.
                pass = try! EngineImmersiveRenderTargets.makePass(device: layer.device,
                    color: drawable.colorTextures[textureIndex], depth: drawable.depthTextures[textureIndex],
                    slice: slice, arrayLength: arrayLength, sampleCount: 1)
                renderContent = false
                report("Unable to allocate immersive antialiasing textures."); control.stop()
            }
            if !drawable.rasterizationRateMaps.isEmpty { pass.rasterizationRateMap = drawable.rasterizationRateMaps[min(textureIndex, drawable.rasterizationRateMaps.count - 1)] }
            guard let encoder = command.makeRenderCommandEncoder(descriptor: pass) else { continue }
            if !renderContent { encoder.endEncoding(); continue }
            encoder.setRenderPipelineState(pipeline); encoder.setDepthStencilState(depth); encoder.setCullMode(.none)
            encoder.setViewports(selected.map { $0.textureMap.viewport })
            var mappings = selected.enumerated().map { index, view in
                MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32(index),
                    renderTargetArrayIndexOffset: dedicated ? 0 : UInt32(view.textureMap.sliceIndex))
            }
            encoder.setVertexAmplificationCount(selected.count, viewMappings: &mappings)
            encoder.setVertexBuffer(vertices, offset: 0, index: 0)
            let transforms = dedicated ? [matrices[passIndex]] : matrices
            transforms.withUnsafeBytes { storage in
                encoder.setVertexBytes(storage.baseAddress!, length: storage.count, index: 1)
            }
            var eyeBase = UInt32(dedicated ? passIndex : 0)
            encoder.setVertexBytes(&eyeBase, length: MemoryLayout<UInt32>.size, index: 2)
            // The surround is drawn first, farther out than every content
            // surface, so the picture overwrites it wherever there is one.
            // With the whole sphere covered by the eight panels there is no
            // direction left for it to show through, and it cost thirteen
            // texture reads per fragment over the entire view, for both eyes,
            // before the panels were drawn over the top of every one of them.
            // The headset was presenting at half the display rate; this was
            // one of the reasons.
            if panoramaSnapshot == nil, let backdrop = backdropMesh, let surround = backdropTexture {
                encoder.setRenderPipelineState(backdropPipeline)
                encoder.setVertexBuffer(backdrop.vertices, offset: 0, index: 0)
                encoder.setFragmentTexture(surround, index: 0)
                encoder.setFragmentTexture(backdropTextureRight ?? surround, index: 1)
                var brightness = halo_settings_backdrop_brightness()
                encoder.setFragmentBytes(&brightness, length: MemoryLayout<Float>.size, index: 0)
                encoder.drawIndexedPrimitives(type: .triangle, indexCount: backdrop.count, indexType: .uint32,
                                              indexBuffer: backdrop.indices, indexBufferOffset: 0)
                encoder.setRenderPipelineState(pipeline)
                encoder.setVertexBuffer(vertices, offset: 0, index: 0)
            }
            if let panoramaSnapshot {
                // Five pictures as panels on one sphere. The sky and the
                // floor go down first and solid, then the three forward bands
                // dissolve onto them and into each other, so every join is a
                // gradient and the sharp forward picture wins wherever it
                // reaches. Over blending keeps the result a proper mix, so no
                // join can come out bright or dim.
                // Aligned, every panel is turned by its layer's rotation to the
                // newest centre in the vertex stage, and its fade is measured
                // against where the panels beneath it now are.
                let aligned = alignment?.constants.count == panoramaPanels.count ? alignment : nil
                encoder.setRenderPipelineState(aligned?.pipeline ?? panoramaPipeline)
                encoder.setDepthStencilState(panoramaDepth)
                let ringLayers = EnginePanoramaTexture.Projections.ringLayers
                for source in EngineImmersiveScreenGeometry.panoramaPanelOrder {
                    let mesh = panoramaPanels[source]
                    encoder.setVertexBuffer(mesh.vertices, offset: 0, index: 0)
                    if let aligned {
                        aligned.constants[source].vertex.withUnsafeBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 3) }
                        aligned.constants[source].fragment.withUnsafeBytes { encoder.setFragmentBytes($0.baseAddress!, length: $0.count, index: 2) }
                    }
                    // Cubic reconstruction is nine texture reads per fragment
                    // and it is only worth them where the eye can resolve the
                    // difference: the forward bearing. The other seven panels
                    // are peripheral, and at nine reads each across the whole
                    // view they were most of what held the compositor to half
                    // the display rate. Source 1 is the centre of the ring.
                    var filter: UInt32 = source == 1 ? 1 : 0
                    encoder.setFragmentBytes(&filter, length: MemoryLayout<UInt32>.size, index: 1)
                    if source == ringLayers.count {
                        encoder.setFragmentTexture(panoramaSnapshot.zenith, index: 0)
                        encoder.setFragmentTexture(panoramaSnapshot.zenith, index: 1)
                    } else if source == ringLayers.count + 1 {
                        encoder.setFragmentTexture(panoramaSnapshot.nadir, index: 0)
                        encoder.setFragmentTexture(panoramaSnapshot.nadir, index: 1)
                    } else {
                        let layer = ringLayers[source]
                        encoder.setFragmentTexture(panoramaSnapshot.view(eye: 0, sector: layer), index: 0)
                        encoder.setFragmentTexture(panoramaSnapshot.view(eye: 1, sector: layer), index: 1)
                    }
                    encoder.drawIndexedPrimitives(type: .triangle, indexCount: mesh.count, indexType: .uint32,
                        indexBuffer: mesh.indices, indexBufferOffset: 0)
                }
                encoder.setRenderPipelineState(overlayPipeline)
                encoder.setDepthStencilState(overlayDepth)
                encoder.setFragmentTexture(panoramaSnapshot.hud, index: 0)
                if trackingEnabled {
                    var zero: UInt16 = 0
                    if let bands = trackingBands, trackingHeight == panoramaSnapshot.hud.height { encoder.setFragmentBuffer(bands, offset: 0, index: 3) }
                    else { encoder.setFragmentBytes(&zero, length: 2, index: 3) }
                    encoder.setFragmentBytes(&trackingValues, length: trackingValues.count * 4, index: 4)
                    var tracking = SIMD2<UInt32>(UInt32(max(1, trackingHeight)), trackingActive && trackingHeight == panoramaSnapshot.hud.height ? 1 : 0)
                    encoder.setFragmentBytes(&tracking, length: 8, index: 5)
                }
                // The interface's shadow first, on its farther surface, then
                // the interface itself: the menu's text and the reticle stand
                // off their own shadow in depth.
                var shade: Float = 0.55
                encoder.setFragmentBytes(&shade, length: MemoryLayout<Float>.size, index: 2)
                encoder.setVertexBuffer(hudShadowVertices, offset: 0, index: 0)
                encoder.drawIndexedPrimitives(type: .triangle, indexCount: indexCount, indexType: .uint32,
                                              indexBuffer: indices, indexBufferOffset: 0)
                shade = 1
                encoder.setFragmentBytes(&shade, length: MemoryLayout<Float>.size, index: 2)
                encoder.setVertexBuffer(hudVertices, offset: 0, index: 0)
            } else {
                encoder.setFragmentTexture(texture, index: 0); encoder.setFragmentTexture(texture, index: 1)
                // One flat panel: the kernel is affordable here, and the
                // constant must be bound or the shader reads nothing.
                var filter: UInt32 = 1
                encoder.setFragmentBytes(&filter, length: MemoryLayout<UInt32>.size, index: 1)
            }
            encoder.drawIndexedPrimitives(type: .triangle, indexCount: indexCount, indexType: .uint32,
                                          indexBuffer: indices, indexBufferOffset: 0)
            encoder.endEncoding()
        }
    }

    private static let shader = """
    #include <metal_stdlib>
    using namespace metal;
    struct Vertex { float4 position; float2 uv; float2 weight; };
    struct Raster { float4 position [[position]]; float2 uv; uint eye [[flat]]; float weight; };
    /* eyeBase is 0 when both eyes are amplified from one pass and the eye
     * index when the layout gives each eye its own pass, so the fragment
     * stage always knows which eye it is shading. */
    vertex Raster curve_vertex(uint id [[vertex_id]], uint eye [[amplification_id]],
        const device Vertex *vertices [[buffer(0)]], constant float4x4 *matrices [[buffer(1)]],
        constant uint &eyeBase [[buffer(2)]]) {
        return { matrices[eye] * vertices[id].position, vertices[id].uv, eyeBase + eye, vertices[id].weight.x };
    }
    /* Mitchell-Netravali reconstruction (B = C = 1/3), in bilinear taps.
     *
     * The headset has far more pixels across the forward view than the engine
     * renders into it, so every panorama texel is magnified several times
     * over, and vertically three to four times more than across. Straight
     * bilinear magnification is what makes that read as soft and slightly
     * blocky. A cubic kernel reconstructs the edges between texels instead of
     * ramping across them. Catmull-Rom did that sharply, and its overshoot
     * outlined every stair-step of a magnified edge; Mitchell's small lobes
     * keep the edges without drawing the steps. Taking it as bilinear taps
     * rather than sixteen point samples keeps it cheap, and pixels are not
     * what limits this port. */
    static inline float4 cubic_sample(texture2d<float> source, float2 uv) {
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        float2 size = float2(source.get_width(), source.get_height());
        float2 position = uv * size;
        float2 centre = floor(position - 0.5) + 0.5;
        float2 f = position - centre;
        // Mitchell-Netravali, B = C = 1/3: the near lobe for |x| < 1, the far
        // lobe for 1 <= |x| < 2; the four weights sum to one. The middle two
        // taps of each axis are folded into one bilinear read, which is valid
        // because their weights are both positive; the outer two keep their
        // own reads because theirs change sign.
        float2 n1 = f, n2 = 1.0 - f, x0 = 1.0 + f, x3 = 2.0 - f;
        float2 w0 = ((-7.0 / 18.0 * x0 + 2.0) * x0 - 10.0 / 3.0) * x0 + 16.0 / 9.0;
        float2 w1 = n1 * n1 * (7.0 / 6.0 * n1 - 2.0) + 8.0 / 9.0;
        float2 w2 = n2 * n2 * (7.0 / 6.0 * n2 - 2.0) + 8.0 / 9.0;
        float2 w3 = ((-7.0 / 18.0 * x3 + 2.0) * x3 - 10.0 / 3.0) * x3 + 16.0 / 9.0;
        float2 w12 = w1 + w2;
        float2 p0 = (centre - 1.0) / size;
        float2 p3 = (centre + 2.0) / size;
        float2 p12 = (centre + w2 / w12) / size;
        float4 total = float4(0.0);
        total += source.sample(s, float2(p0.x,  p0.y))  * (w0.x  * w0.y);
        total += source.sample(s, float2(p12.x, p0.y))  * (w12.x * w0.y);
        total += source.sample(s, float2(p3.x,  p0.y))  * (w3.x  * w0.y);
        total += source.sample(s, float2(p0.x,  p12.y)) * (w0.x  * w12.y);
        total += source.sample(s, float2(p12.x, p12.y)) * (w12.x * w12.y);
        total += source.sample(s, float2(p3.x,  p12.y)) * (w3.x  * w12.y);
        total += source.sample(s, float2(p0.x,  p3.y))  * (w0.x  * w3.y);
        total += source.sample(s, float2(p12.x, p3.y))  * (w12.x * w3.y);
        total += source.sample(s, float2(p3.x,  p3.y))  * (w3.x  * w3.y);
        return total;
    }
    /* In stereo the two eyes were rendered from different camera positions, so
     * each samples its own view; in mono both slots hold the same texture. */
    static inline float4 pick(Raster in, texture2d<float> left, texture2d<float> right, float2 uv) {
        return in.eye == 0 ? cubic_sample(left, uv) : cubic_sample(right, uv);
    }
    /* The interface, premultiplied. With shade below one this is its shadow:
     * the same shapes, black, at a fraction of their opacity, drawn on a
     * farther surface so the text and reticle stand off it in depth. */
    static inline float4 hud_shade(float4 picture, float shade) {
        if (shade < 1.0) return float4(0.0, 0.0, 0.0, picture.a * shade);
        // The interface is premultiplied sRGB bytes. The target is sRGB and
        // blends in linear light, so unpremultiply, decode, premultiply.
        // Halo draws the reticle and other glows additively, with colour
        // but no alpha; dividing by alpha alone made those vanish (they
        // only showed behind the pause menu's backdrop, which gave them
        // alpha), so the cover is the larger of alpha and the brightest
        // channel, and an additive pixel decodes on its own brightness.
        float cover = max(picture.a, max(picture.r, max(picture.g, picture.b)));
        float3 straight = cover > 0.0005 ? picture.rgb / cover : float3(0.0);
        float3 linear = select(pow((straight + 0.055) / 1.055, 2.4), straight / 12.92, straight <= 0.04045);
        return float4(linear * cover, picture.a);
    }
    fragment float4 hud_fragment(Raster in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                 constant float &shade [[buffer(2)]]) {
        constexpr sampler sample(filter::linear, address::clamp_to_edge);
        return hud_shade(frame.sample(sample, in.uv), shade);
    }
    /* The same, writing each lit interface pixel's row band as its tracking
     * area, so the system's hover glow follows the text row the eyes are on.
     * tracking.x is the interface's height in rows, tracking.y whether the
     * menu is up; the shadow pass writes no area. */
    struct HudTracked { float4 colour [[color(0)]]; uint area [[color(1)]]; };
    fragment HudTracked hud_fragment_tracked(Raster in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                             constant float &shade [[buffer(2)]],
                                             const device ushort *bands [[buffer(3)]],
                                             constant uint *values [[buffer(4)]],
                                             constant uint2 &tracking [[buffer(5)]]) {
        constexpr sampler sample(filter::linear, address::clamp_to_edge);
        float4 picture = frame.sample(sample, in.uv);
        HudTracked out; out.colour = hud_shade(picture, shade); out.area = 0u;
        if (tracking.y != 0u && shade >= 1.0) {
            uint row = min(uint(clamp(in.uv.y, 0.0, 0.9999) * float(tracking.x)), tracking.x - 1u);
            uint band = uint(bands[row]);
            float cover = max(picture.a, max(picture.r, max(picture.g, picture.b)));
            if (band > 0u && band < 33u && cover > 0.05) out.area = values[band];
        }
        return out;
    }
    /* Per row of the interface: the first and last lit column, or 0xFFFF. */
    kernel void hud_rows(texture2d<float> hud [[texture(0)]], device ushort *rows [[buffer(0)]],
                         uint row [[thread_position_in_grid]]) {
        uint width = hud.get_width(), height = hud.get_height();
        if (row >= height) return;
        uint first = 0xFFFFu, last = 0u;
        for (uint x = 0u; x < width; x += 2u) {
            float4 p = hud.read(uint2(x, row));
            if (max(p.a, max(p.r, max(p.g, p.b))) > 0.05) { if (first == 0xFFFFu) first = x; last = x; }
        }
        rows[row * 2u] = ushort(first); rows[row * 2u + 1u] = ushort(last);
    }
    /* Runs of lit rows, a gap of up to two rows allowed, become bands 1...32
     * from the top; each band's rows and columns go to info for the pinch. */
    struct BandInfo { ushort row_start, row_end, x_min, x_max; };
    kernel void hud_bands(device const ushort *rows [[buffer(0)]], device ushort *bands [[buffer(1)]],
                          device BandInfo *info [[buffer(2)]], constant uint &height [[buffer(3)]],
                          uint id [[thread_position_in_grid]]) {
        if (id != 0u) return;
        for (uint b = 0u; b < 33u; b++) { info[b].row_start = 0; info[b].row_end = 0; info[b].x_min = 0; info[b].x_max = 0; }
        uint band = 0u, gap = 99u; ushort xmin = 0xFFFF, xmax = 0;
        for (uint r = 0u; r < height; r++) {
            bool lit = rows[r * 2u] != 0xFFFFu;
            if (!lit) { gap++; bands[r] = 0; continue; }
            if (gap > 2u && band < 32u) { band++; info[band].row_start = ushort(r); xmin = 0xFFFF; xmax = 0; }
            gap = 0u;
            if (band > 0u) {
                info[band].row_end = ushort(r);
                xmin = min(xmin, rows[r * 2u]); xmax = max(xmax, rows[r * 2u + 1u]);
                info[band].x_min = xmin; info[band].x_max = xmax;
            }
            bands[r] = ushort(band);
        }
    }
    /* The surround. Texture coordinates outside 0...1 mean this direction is
     * past the edge of the rendered picture; clamp to the border texel and
     * fade with angular distance so the space around the image holds a dim
     * continuation of its border rather than hard black. Several taps soften
     * the border texels into a glow instead of visible streaks. */
    fragment float4 backdrop_fragment(Raster in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                      texture2d<float> other [[texture(1)]],
                                      constant float &brightness [[buffer(0)]]) {
        constexpr sampler sample(filter::linear, address::clamp_to_edge);
        texture2d<float> source = in.eye == 0 ? frame : other;
        float2 clamped = clamp(in.uv, 0.0, 1.0);
        float2 outside = abs(in.uv - clamped);
        // Beyond the picture, the nearest texel smears into radial streaks and
        // a visible cap. Cross-fade towards the average of that whole border
        // instead, so above and below the view reads as a flat wash of the
        // right colour, and let it fall off gently enough to leave no ring.
        float3 local = float3(0.0);
        local += source.sample(sample, clamped).rgb * 0.36;
        local += source.sample(sample, clamped + float2( 0.035, 0.0)).rgb * 0.16;
        local += source.sample(sample, clamped + float2(-0.035, 0.0)).rgb * 0.16;
        local += source.sample(sample, clamped + float2(0.0,  0.035)).rgb * 0.16;
        local += source.sample(sample, clamped + float2(0.0, -0.035)).rgb * 0.16;
        float3 wash = float3(0.0);
        for (int i = 0; i < 8; ++i) {
            float u = (float(i) + 0.5) / 8.0;
            wash += source.sample(sample, float2(u, clamped.y)).rgb;
        }
        wash /= 8.0;
        float spread = clamp((outside.x + outside.y) * 2.6, 0.0, 1.0);
        float3 colour = mix(local, wash, spread);
        // Smooth, wide falloff: no hard edge where the picture stops.
        float distance = length(outside);
        float fade = 1.0 - smoothstep(0.0, 0.85, distance);
        fade *= fade;
        return float4(colour * fade * brightness, 1.0);
    }
    /* filter: 1 reconstructs with the Mitchell kernel above, 0 with one
     * bilinear read. The forward bearing gets the kernel; the peripheral
     * panels do not, because nine reads per fragment across the whole view
     * for both eyes is what held the compositor to half the display rate. */
    fragment float4 curve_fragment(Raster in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                   texture2d<float> other [[texture(1)]],
                                   constant uint &filter [[buffer(1)]]) {
        // D3D's game back buffer has no meaningful display alpha. Submit an
        // opaque picture so those bytes cannot expose stale compositor tiles.
        float2 clamped = clamp(in.uv, 0.0, 1.0);
        float2 outside = abs(in.uv - clamped);
        constexpr sampler bilinear(filter::linear, address::clamp_to_edge);
        float3 colour = filter == 1 ? pick(in, frame, other, clamped).rgb
                                    : (in.eye == 0 ? frame : other).sample(bilinear, clamped).rgb;
        // Inside an overlap this is below one and the band blends into the
        // band already drawn there; everywhere else it is exactly one and the
        // result is the same opaque picture as before. The curve is applied
        // here so the fade is smooth however coarsely the mesh is divided.
        float alpha = clamp(in.weight, 0.0, 1.0);
        if (outside.y <= 0.0 && outside.x <= 0.0) return float4(colour, alpha);
        // Rows past what the source covers. They continue the picture rather
        // than ending it: the border blends into the average of its own row
        // and dims away, starting from exactly the edge colour so there is no
        // step in brightness where the coverage stops.
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        float3 wash = float3(0.0);
        for (int i = 0; i < 8; ++i) {
            float u = (float(i) + 0.5) / 8.0;
            wash += (in.eye == 0 ? frame : other).sample(s, float2(u, clamped.y)).rgb;
        }
        wash /= 8.0;
        float distance = outside.x + outside.y;
        colour = mix(colour, wash, clamp(distance * 3.0, 0.0, 1.0));
        float fade = 1.0 - smoothstep(0.0, 0.55, distance);
        return float4(colour * fade, alpha);
    }
    """

    struct RendererError: LocalizedError {
        let message: String
        init(_ message: String) { self.message = message }
        var errorDescription: String? { message }
    }
}
