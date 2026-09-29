import Foundation
import Metal
import simd

/// A bounded pool of coherent four-layer snapshots. A pool slot is returned
/// only by the completion handler of every command buffer that samples it.
final class EnginePanoramaTexture {
    private static let layerCount = Int(HALO_PANORAMA_LAYERS)
    private static let maximumSlots = 3

    /// Every layer's projection and source rectangle, indexed by layer so it
    /// lines up with the bridge rather than with a separate notion of sector.
    /// The ring is layers 0,1,2 and 7,8,9; the sky and floor are 5 and 6.
    struct Projections: Equatable {
        /// The six bearings, in the order they sit around the ring starting
        /// at the left of the forward view.
        static let ringLayers = [0, 1, 2, 7, 8, 9]
        static let skyLayer = Int(HALO_PANORAMA_UP), floorLayer = Int(HALO_PANORAMA_DOWN)
        var perLayer: [SIMD2<Float>]
        var viewports: [SIMD4<Float>]
        var texelSize = SIMD2<Float>.zero

        init(perLayer: [SIMD2<Float>]? = nil, viewports: [SIMD4<Float>]? = nil) {
            let n = Int(HALO_PANORAMA_LAYERS)
            self.perLayer = perLayer ?? [SIMD2<Float>](repeating: .zero, count: n)
            self.viewports = viewports ?? [SIMD4<Float>](repeating: SIMD4(0, 0, 1, 1), count: n)
        }

        func viewport(_ layer: Int) -> SIMD4<Float> { viewports[layer] }
        subscript(layer: Int) -> SIMD2<Float> { perLayer[layer] }

        // Intrinsic projection relative to the valid source rectangle, not
        // to clear-colored bands outside a reduced cinematic viewport.
        var maximumY: Float {
            Self.ringLayers.map { self[$0].y / (viewport($0).w - viewport($0).y) }.max()!
        }
        fileprivate var isValid: Bool {
            func valid(_ value: Float) -> Bool { value.isFinite && value > 0 && value < 3 }
            // Every bearing and both caps have to be believable, or the frame
            // is not a complete sphere and must not be presented as one.
            return (Self.ringLayers + [Self.skyLayer, Self.floorLayer]).allSatisfy {
                let rect = viewport($0), projection = self[$0]
                return valid(projection.x) && valid(projection.y) &&
                    rect.x.isFinite && rect.y.isFinite && rect.z.isFinite && rect.w.isFinite &&
                    rect.x >= 0 && rect.y >= 0 && rect.z <= 1 && rect.w <= 1 &&
                    rect.z > rect.x && rect.w > rect.y &&
                    projection.x / (rect.z - rect.x) <= 1.7321
            }
        }
    }

    fileprivate final class Slot {
        let index: Int
        var width = 0
        var height = 0
        var textures: [MTLTexture] = []
        var projections = Projections()
        var sequence: UInt64 = 0
        var stereo = false
        var zenithProjection = SIMD2<Float>(1, 1)
        var nadirProjection = SIMD2<Float>(1, 1)
        var sourceEpoch: UInt64 = 0
        var sceneEpoch: UInt64 = 0
        var layerEpochs: [UInt64] = []
        var layerPoses: [EngineLayerPose?] = []
        var cutEpoch: UInt64 = 0
        var flatSequence: UInt64 = 0
        var inFlight = 0
        var reserved = false

        init(index: Int) { self.index = index }
    }

    struct Snapshot {
        /// The pictures as the engine wrote them: BGRA8 bytes carrying
        /// Halo's display-referred (sRGB-encoded) colour, as on a monitor.
        let textures: [MTLTexture]
        /// The same storage seen as bgra8Unorm_srgb, which is what the
        /// shaders sample. The compositor's drawable is sRGB too, so a sample
        /// decoded here and re-encoded on write lands on the display as the
        /// byte the engine produced. Sampled raw, every byte was taken as
        /// linear light and re-encoded on top: a black of 8 became 28, a
        /// nebula of 34 became 90, and the whole sphere read as washed grey.
        let views: [MTLTexture]
        let projections: Projections
        let sequence: UInt64
        let sourceEpoch: UInt64
        let sceneEpoch: UInt64
        let layerEpochs: [UInt64]
        /// The engine camera each layer was drawn with (nil when unknown),
        /// and the first epoch after the last camera cut. Layer alignment
        /// turns older layers by these to meet the newest centre.
        let layerPoses: [EngineLayerPose?]
        let cutEpoch: UInt64
        let flatSequence: UInt64
        /// Runtime pool slot for a zero-copy frame; -1 for a byte-copied one.
        let gpuSlot: Int32
        /// True when the centre bearing was also drawn for the right eye.
        let stereo: Bool
        /// Projections of the zenith and nadir views.
        let zenithProjection: SIMD2<Float>
        let nadirProjection: SIMD2<Float>
        /// The texture for one forward bearing as seen by one eye.
        ///
        /// Only the centre bearing is drawn once per eye; the sides are drawn
        /// once and shown to both, so they return the same texture either way.
        func view(eye: Int, sector: Int) -> MTLTexture {
            let index = (stereo && eye == 1 && sector == 1) ? Int(HALO_PANORAMA_CENTRE_RIGHT) : sector
            return views[index < views.count ? index : sector]
        }
        /// The zenith or nadir view, drawn once and shown to both eyes.
        var zenith: MTLTexture { views[Int(HALO_PANORAMA_UP)] }
        var nadir: MTLTexture { views[Int(HALO_PANORAMA_DOWN)] }
        /// The interface is premultiplied, so its shader decodes it itself.
        var hud: MTLTexture { textures[Int(HALO_PANORAMA_HUD_LAYER)] }
        var poolSlot: Int { slot?.index ?? Int(gpuSlot) }
        fileprivate let slot: Slot?
    }

    private let device: MTLDevice
    private let lock = NSLock()
    private var slots: [Slot] = []
    private var current: Slot?
    private var stagingBytes: [UInt8] = []
    private(set) var sourceStatus: UInt32 = 0
    private(set) var failureReason: UInt32 = 0
    private(set) var latestFlatSequence: UInt64 = 0
    private(set) var latestSourceEpoch: UInt64 = 0
    var allowsFlatFallback: Bool { sourceStatus == 0 }

    init(device: MTLDevice) { self.device = device }

    /// Refreshes from one atomic bridge copy and leases the returned snapshot
    /// until `commandBuffer` completes. If three submissions are still using
    /// the pool, the latest complete snapshot remains published and the newer
    /// source sequence is retried on the next compositor frame.
    func refresh(leasedTo commandBuffer: MTLCommandBuffer) -> Snapshot? {
        if enginevision_panorama_gpu_enabled(), let snapshot = refreshGPU(leasedTo: commandBuffer) { return snapshot }
        guard let snapshot = refresh() else { return nil }
        guard let slot = snapshot.slot else { return snapshot }
        lock.lock()
        slot.inFlight += 1
        lock.unlock()
        commandBuffer.addCompletedHandler { [weak self, weak slot] _ in
            guard let self, let slot else { return }
            self.lock.lock()
            precondition(slot.inFlight > 0)
            slot.inFlight -= 1
            self.lock.unlock()
        }
        return snapshot
    }

    /// Zero-copy path: the engine blits its views into runtime-owned textures.
    /// The lease taken by `latest` is returned when this command buffer
    /// completes, so the runtime does not render into the slot again before
    /// the compositor has finished sampling it.
    private func refreshGPU(leasedTo commandBuffer: MTLCommandBuffer) -> Snapshot? {
        var lent = EngineVisionPanoramaGPUSnapshot()
        guard enginevision_panorama_gpu_latest(&lent) else { return nil }
        updateSourceState(lent.info)
        heldFrames = 0
        let slot = lent.slot
        let pointers = EnginePanoramaLayerArray.pointers(of: lent.textures)
        var textures: [MTLTexture] = []
        for pointer in pointers {
            if let pointer, let texture = Unmanaged<AnyObject>.fromOpaque(pointer).takeUnretainedValue() as? MTLTexture {
                textures.append(texture)
            }
        }
        guard textures.count == Self.layerCount,
              textures[0].width == Int(lent.info.width), textures[0].height == Int(lent.info.height),
              let projections = Self.projections(from: lent.info) else {
            enginevision_panorama_gpu_release(slot)
            sourceStatus = 2; failureReason = 5
            return nil
        }
        commandBuffer.addCompletedHandler { _ in enginevision_panorama_gpu_release(slot) }
        return Snapshot(textures: textures, views: textures.map(srgbView), projections: projections, sequence: lent.info.sequence,
                        sourceEpoch: lent.info.source_epoch, sceneEpoch: lent.info.scene_epoch,
                        layerEpochs: Self.layerEpochs(lent.info), layerPoses: Self.layerPoses(lent.info),
                        cutEpoch: lent.info.cut_epoch, flatSequence: lent.info.flat_sequence,
                        gpuSlot: slot, stereo: lent.info.stereo != 0,
                        zenithProjection: Self.projection(of: lent.info, layer: Int(HALO_PANORAMA_UP)),
                        nadirProjection: Self.projection(of: lent.info, layer: Int(HALO_PANORAMA_DOWN)),
                        slot: nil)
    }

    /// Projection of one layer, for the cap meshes.
    static func layerEpochs(_ info: EngineVisionPanoramaInfo) -> [UInt64] {
        withUnsafeBytes(of: info.layer_epoch) { Array($0.bindMemory(to: UInt64.self)) }
    }

    /// Each layer's camera: nine floats per layer, zero when unknown.
    static func layerPoses(_ info: EngineVisionPanoramaInfo) -> [EngineLayerPose?] {
        let floats = withUnsafeBytes(of: info.layer_pose) { Array($0.bindMemory(to: Float.self)) }
        let stride = Int(HALO_PANORAMA_POSE_FLOATS)
        return (0..<Int(HALO_PANORAMA_LAYERS)).map { layer in
            let start = layer * stride
            return start + stride <= floats.count ? EngineLayerPose(floats: Array(floats[start..<start + stride])) : nil
        }
    }

    static func projection(of info: EngineVisionPanoramaInfo, layer: Int) -> SIMD2<Float> {
        withUnsafePointer(to: info.projection_x) { x in
            withUnsafePointer(to: info.projection_y) { y in
                x.withMemoryRebound(to: Float.self, capacity: Int(HALO_PANORAMA_LAYERS)) { px in
                    y.withMemoryRebound(to: Float.self, capacity: Int(HALO_PANORAMA_LAYERS)) { py in
                        SIMD2(px[layer], py[layer])
                    }
                }
            }
        }
    }

    /// Read one of the bridge's per-layer float arrays. Swift imports them as
    /// tuples, so they are walked through memory rather than by field.
    private static func perLayer<T>(_ tuple: T) -> [Float] {
        let n = Int(HALO_PANORAMA_LAYERS)
        return withUnsafePointer(to: tuple) { pointer in
            pointer.withMemoryRebound(to: Float.self, capacity: n) { Array(UnsafeBufferPointer(start: $0, count: n)) }
        }
    }

    private static func projections(from info: EngineVisionPanoramaInfo) -> Projections? {
        let n = Int(HALO_PANORAMA_LAYERS)
        let px = perLayer(info.projection_x), py = perLayer(info.projection_y)
        let uMin = perLayer(info.viewport_u_min), vMin = perLayer(info.viewport_v_min)
        let uMax = perLayer(info.viewport_u_max), vMax = perLayer(info.viewport_v_max)
        var projections = Projections(
            perLayer: (0..<n).map { SIMD2(px[$0], py[$0]) },
            viewports: (0..<n).map { SIMD4(uMin[$0], vMin[$0], uMax[$0], vMax[$0]) })
        guard info.width > 0, info.height > 0 else { return nil }
        projections.texelSize = SIMD2(1 / Float(info.width), 1 / Float(info.height))
        return projections.isValid ? projections : nil
    }

    /// Unleased refresh is useful for validation and callers that submit no GPU
    /// work. Rendering code must use `refresh(leasedTo:)`.
    func refresh() -> Snapshot? {
        var hint = EngineVisionPanoramaInfo()
        let available = enginevision_panorama_info(&hint)
        updateSourceState(hint)
        guard available else {
            if sourceStatus == 2 { return currentSnapshot() }
            // Crossing between the front end and a mission, the engine can
            // report a plain frame for a moment before the next world frame
            // lands. Dropping the surrounding view for those few frames reads
            // as a flash, so hold the last complete one briefly and only give
            // up if the plain frames keep coming.
            if let held = holdDuringTransition() { return held }
            clearCurrent()
            return nil
        }
        guard let byteCount = validatedByteCount(hint) else {
            sourceStatus = 2; failureReason = 5
            return currentSnapshot()
        }
        if let snapshot = currentSnapshot(), snapshot.sequence == hint.sequence { return snapshot }

        if stagingBytes.count != byteCount {
            stagingBytes = [UInt8](repeating: 0, count: byteCount)
        }
        var copied = EngineVisionPanoramaInfo()
        let copiedOK = stagingBytes.withUnsafeMutableBytes {
            enginevision_copy_panorama($0.baseAddress, $0.count, &copied)
        }
        guard copiedOK, validatedByteCount(copied) == byteCount,
              copied.width == hint.width, copied.height == hint.height else {
            // A menu transition can race the info/copy split. Respect its
            // explicit state instead of retaining gameplay over the menu.
            if !copiedOK && copied.status == 0 && copied.flat_sequence > 0 {
                updateSourceState(copied); clearCurrent(); return nil
            }
            if copied.flat_sequence > 0 { updateSourceState(copied) }
            return currentSnapshot()
        }
        updateSourceState(copied)
        heldFrames = 0
        if let snapshot = currentSnapshot(), snapshot.sequence == copied.sequence { return snapshot }

        let copiedStereo = copied.stereo != 0
        let n = Int(HALO_PANORAMA_LAYERS)
        let cpx = Self.perLayer(copied.projection_x), cpy = Self.perLayer(copied.projection_y)
        let cuMin = Self.perLayer(copied.viewport_u_min), cvMin = Self.perLayer(copied.viewport_v_min)
        let cuMax = Self.perLayer(copied.viewport_u_max), cvMax = Self.perLayer(copied.viewport_v_max)
        var projections = Projections(
            perLayer: (0..<n).map { SIMD2(cpx[$0], cpy[$0]) },
            viewports: (0..<n).map { SIMD4(cuMin[$0], cvMin[$0], cuMax[$0], cvMax[$0]) })
        projections.texelSize = SIMD2(1 / Float(copied.width), 1 / Float(copied.height))
        guard projections.isValid else {
            sourceStatus = 2; failureReason = 5
            return currentSnapshot()
        }

        let width = Int(copied.width), height = Int(copied.height)
        guard let slot = reserveSlot(width: width, height: height) else { return currentSnapshot() }
        guard prepare(slot: slot, width: width, height: height) else {
            releaseReservation(slot)
            return currentSnapshot()
        }

        let layerBytes = byteCount / Self.layerCount
        stagingBytes.withUnsafeBytes { source in
            for layer in 0..<Self.layerCount {
                slot.textures[layer].replace(
                    region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                    withBytes: source.baseAddress!.advanced(by: layer * layerBytes),
                    bytesPerRow: width * 4)
            }
        }

        lock.lock()
        slot.projections = projections
        slot.sequence = copied.sequence
        slot.sourceEpoch = copied.source_epoch
        slot.sceneEpoch = copied.scene_epoch
        slot.layerEpochs = Self.layerEpochs(copied)
        slot.layerPoses = Self.layerPoses(copied)
        slot.cutEpoch = copied.cut_epoch
        slot.flatSequence = copied.flat_sequence
        slot.stereo = copiedStereo
        slot.zenithProjection = Self.projection(of: copied, layer: Int(HALO_PANORAMA_UP))
        slot.nadirProjection = Self.projection(of: copied, layer: Int(HALO_PANORAMA_DOWN))
        slot.reserved = false
        current = slot
        let snapshot = makeSnapshot(slot)
        lock.unlock()
        return snapshot
    }

    private func validatedByteCount(_ info: EngineVisionPanoramaInfo) -> Int? {
        guard info.width > 0, info.height > 0, info.width <= 4096, info.height <= 4096 else { return nil }
        let (pixels, pixelOverflow) = Int(info.width).multipliedReportingOverflow(by: Int(info.height))
        let (bytes, byteOverflow) = pixels.multipliedReportingOverflow(by: 4 * Self.layerCount)
        guard !pixelOverflow, !byteOverflow, info.byte_count == bytes else { return nil }
        return bytes
    }

    private func updateSourceState(_ info: EngineVisionPanoramaInfo) {
        sourceStatus = info.status; failureReason = info.failure_reason
        latestFlatSequence = info.flat_sequence; latestSourceEpoch = info.source_epoch
    }

    private func reserveSlot(width: Int, height: Int) -> Slot? {
        lock.lock()
        defer { lock.unlock() }
        var matching: Slot?, recyclable: Slot?
        for candidate in slots where candidate !== current && candidate.inFlight == 0 && !candidate.reserved {
            if recyclable == nil { recyclable = candidate }
            if candidate.width == width, candidate.height == height,
               candidate.textures.count == Self.layerCount {
                matching = candidate
                break
            }
        }
        let slot: Slot?
        if let matching {
            slot = matching
        } else if let recyclable {
            slot = recyclable
        } else if slots.count < Self.maximumSlots {
            let created = Slot(index: slots.count)
            slots.append(created)
            slot = created
        } else {
            slot = nil
        }
        slot?.reserved = true
        return slot
    }

    private func prepare(slot: Slot, width: Int, height: Int) -> Bool {
        if slot.width == width, slot.height == height, slot.textures.count == Self.layerCount { return true }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = .shaderRead
        var replacements: [MTLTexture] = []
        for layer in 0..<Self.layerCount {
            guard let texture = device.makeTexture(descriptor: descriptor) else { return false }
            texture.label = "Halo panorama pool \(slot.index), layer \(layer)"
            replacements.append(texture)
        }
        slot.textures = replacements
        slot.width = width
        slot.height = height
        return true
    }

    private func releaseReservation(_ slot: Slot) {
        lock.lock()
        slot.reserved = false
        lock.unlock()
    }

    /// Compositor frames the last complete panorama may outlive a plain
    /// frame. About a quarter of a second at 90Hz.
    private static let transitionHoldFrames = 22
    private var heldFrames = 0
    private func holdDuringTransition() -> Snapshot? {
        guard let snapshot = currentSnapshot() else { heldFrames = 0; return nil }
        heldFrames += 1
        if heldFrames > Self.transitionHoldFrames { heldFrames = 0; return nil }
        return snapshot
    }

    private func clearCurrent() {
        lock.lock()
        current = nil
        lock.unlock()
    }

    private func currentSnapshot() -> Snapshot? {
        lock.lock()
        defer { lock.unlock() }
        return current.map(makeSnapshot)
    }

    /// One sRGB view per texture, kept for as long as the texture lives:
    /// the zero-copy pool lends the same few textures every frame.
    private var srgbViews: [ObjectIdentifier: (texture: MTLTexture, view: MTLTexture)] = [:]
    private var srgbViewFailures = 0
    private func srgbView(_ texture: MTLTexture) -> MTLTexture {
        let key = ObjectIdentifier(texture)
        if let cached = srgbViews[key], cached.texture === texture { return cached.view }
        let format: MTLPixelFormat = texture.pixelFormat == .rgba8Unorm ? .rgba8Unorm_srgb : .bgra8Unorm_srgb
        guard texture.pixelFormat == .bgra8Unorm || texture.pixelFormat == .rgba8Unorm,
              let view = texture.makeTextureView(pixelFormat: format) else {
            srgbViewFailures += 1
            if srgbViewFailures == 1 { NSLog("[panorama] no sRGB view for pixel format %lu; sampling raw", UInt(texture.pixelFormat.rawValue)) }
            return texture
        }
        view.label = (texture.label ?? "panorama layer") + " (sRGB view)"
        if srgbViews.count > 64 { srgbViews.removeAll() }
        srgbViews[key] = (texture, view)
        return view
    }

    private func makeSnapshot(_ slot: Slot) -> Snapshot {
        Snapshot(textures: slot.textures, views: slot.textures.map(srgbView), projections: slot.projections,
                 sequence: slot.sequence, sourceEpoch: slot.sourceEpoch, sceneEpoch: slot.sceneEpoch, layerEpochs: slot.layerEpochs,
                 layerPoses: slot.layerPoses, cutEpoch: slot.cutEpoch,
                 flatSequence: slot.flatSequence, gpuSlot: -1, stereo: slot.stereo,
                 zenithProjection: slot.zenithProjection, nadirProjection: slot.nadirProjection, slot: slot)
    }
}
