import Metal

final class EngineFrameTexture: @unchecked Sendable {
    private let device: MTLDevice
    private(set) var texture: MTLTexture?
    /// The same bytes seen as sRGB, for sampling (see EnginePanoramaTexture.Snapshot.views).
    private(set) var view: MTLTexture?
    private(set) var sequence: UInt64 = 0

    init(device: MTLDevice) { self.device = device }

    @discardableResult
    func refresh(expectedSequence: UInt64? = nil) -> Bool {
        var hint = EngineVisionFrameInfo()
        guard enginevision_frame_info(&hint), hint.sequence != sequence,
              hint.width > 0, hint.height > 0, hint.byte_count > 0,
              hint.byte_count <= 8192 * 8192 * 4 else { return false }
        var bytes = [UInt8](repeating: 0, count: Int(hint.byte_count))
        var copied = EngineVisionFrameInfo()
        let ok = bytes.withUnsafeMutableBytes { storage in
            enginevision_copy_latest_frame(storage.baseAddress, storage.count, &copied)
        }
        guard ok, copied.width > 0, copied.height > 0,
              copied.byte_count == bytes.count else { return false }
        guard expectedSequence == nil || copied.sequence == expectedSequence else { return false }
        guard copied.sequence != sequence,
              Int(copied.width) * Int(copied.height) * 4 == bytes.count else { return false }
        // Publish an immutable snapshot. A command buffer from the previous
        // compositor frame can still be sampling the old texture; replace()
        // on that same resource would race the GPU and tear the picture.
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                                       width: Int(copied.width), height: Int(copied.height),
                                                                       mipmapped: false)
        descriptor.usage = [.shaderRead]
        guard let nextTexture = device.makeTexture(descriptor: descriptor) else { return false }
        nextTexture.label = "Original Halo engine BGRA frame \(copied.sequence)"
        bytes.withUnsafeBytes { source in
            nextTexture.replace(region: MTLRegionMake2D(0, 0, nextTexture.width, nextTexture.height), mipmapLevel: 0,
                            withBytes: source.baseAddress!, bytesPerRow: nextTexture.width * 4)
        }
        texture = nextTexture
        view = nextTexture.makeTextureView(pixelFormat: .bgra8Unorm_srgb) ?? nextTexture
        sequence = copied.sequence
        return true
    }
}
