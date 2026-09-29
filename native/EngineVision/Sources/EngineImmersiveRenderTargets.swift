import Metal

/// Shared by the compositor and the actual-GPU stereo/edge regression test.
enum EngineImmersiveRenderTargets {
    /// One sample. Everything this compositor draws is a texture-mapped panel
    /// whose joins are dissolved in the shader, so multisampling had nothing
    /// to smooth but the panel silhouettes, which the feather already hides.
    /// What it did have was a cost: four samples of colour and depth per
    /// pixel per eye, resolved every frame, on a headset that was presenting
    /// at half its display rate.
    static func sampleCount(device: MTLDevice) -> Int {
        _ = device
        return 1
    }

    static func makePass(device: MTLDevice, color: MTLTexture, depth: MTLTexture,
                         slice: Int = 0, arrayLength: Int = 1,
                         sampleCount: Int, tracking: MTLTexture? = nil) throws -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.renderTargetArrayLength = arrayLength
        // The system's tracking-area identifiers, one sample only: cleared
        // to "no area" and written by the interface pass alone.
        if let tracking, sampleCount == 1 {
            pass.colorAttachments[1].texture = tracking
            pass.colorAttachments[1].slice = slice
            pass.colorAttachments[1].loadAction = .clear
            pass.colorAttachments[1].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[1].storeAction = .store
        }
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].loadAction = .clear
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 0
        if sampleCount == 1 {
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].slice = slice
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.slice = slice
            pass.depthAttachment.storeAction = .store
            return pass
        }
        func multisample(_ target: MTLTexture) throws -> MTLTexture {
            let descriptor = MTLTextureDescriptor()
            descriptor.textureType = target.textureType == .type2DArray ? .type2DMultisampleArray : .type2DMultisample
            descriptor.pixelFormat = target.pixelFormat
            descriptor.width = target.width; descriptor.height = target.height
            descriptor.arrayLength = target.arrayLength; descriptor.sampleCount = sampleCount
            // Tile-local samples resolve into the compositor's single-sample
            // textures; never preserve or reuse uninitialized MSAA contents.
            descriptor.storageMode = .memoryless; descriptor.usage = .renderTarget
            guard let texture = device.makeTexture(descriptor: descriptor) else {
                throw TargetError.allocationFailed
            }
            return texture
        }
        pass.colorAttachments[0].texture = try multisample(color)
        pass.colorAttachments[0].slice = slice
        pass.colorAttachments[0].resolveTexture = color
        pass.colorAttachments[0].resolveSlice = slice
        pass.colorAttachments[0].storeAction = .multisampleResolve
        pass.depthAttachment.texture = try multisample(depth)
        pass.depthAttachment.slice = slice
        pass.depthAttachment.resolveTexture = depth
        pass.depthAttachment.resolveSlice = slice
        pass.depthAttachment.storeAction = .multisampleResolve
        // CompositorServices uses reversed depth: nearest covered sample wins.
        pass.depthAttachment.depthResolveFilter = .max
        return pass
    }

    enum TargetError: Error { case allocationFailed }
}
