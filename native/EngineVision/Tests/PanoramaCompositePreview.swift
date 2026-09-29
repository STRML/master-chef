import Foundation
import Metal
import simd

/// Render what the headset would show, on the Mac, from real captured layers.
///
/// The five panorama panels are composited exactly as the compositor does,
/// through the production shader and the production geometry, from a virtual
/// eye that can be pointed anywhere. That makes the joins and the poles
/// something to look at rather than something to argue about.
///
///   swiftc -O native/EngineVision/Sources/EngineImmersiveScreenGeometry.swift \
///     native/EngineVision/Tests/PanoramaCompositePreview.swift -o /tmp/preview
///   /tmp/preview <capture dir> <frame> <yaw deg> <pitch deg> <out.ppm> [capHalfFov]
@main struct PanoramaCompositePreview {
static func main() throws {
    setbuf(stdout, nil)
    let args = CommandLine.arguments
    guard args.count >= 6 else { fputs("usage: preview <dir> <frame> <yaw> <pitch> <out.ppm> [capHalfFov]\n", stderr); exit(2) }
    let dir = args[1], frame = args[2]
    let lookYaw = Float(args[3])! * .pi / 180, lookPitch = Float(args[4])! * .pi / 180
    let outPath = args[5]
    let capHalf = args.count > 6 ? Float(args[6])! : 48.0

    let sourceFile = "native/EngineVision/Sources/EngineImmersive.swift"
    var shader = try String(contentsOfFile: sourceFile, encoding: .utf8)
        .components(separatedBy: "private static let shader = \"\"\"")[1]
        .components(separatedBy: "\"\"\"")[0]
    // An optional last argument of "bilinear" strips the cubic reconstruction
    // back to a single bilinear tap, so the two filters can be compared on
    // the same frame from the same layers.
    if CommandLine.arguments.last == "bilinear" {
        let start = shader.range(of: "static inline float4 cubic_sample")!
        let end = shader.range(of: "/* In stereo the two eyes")!
        shader.replaceSubrange(start.lowerBound..<end.lowerBound, with: """
        static inline float4 cubic_sample(texture2d<float> source, float2 uv) {
            constexpr sampler s(filter::linear, address::clamp_to_edge);
            return source.sample(s, uv);
        }
        """)
    }
    guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
        fputs("no Metal device\n", stderr); exit(2)
    }
    // The captured layers are 1280x960 BGRA, one file per layer.
    let layerW = 1280, layerH = 960
    func loadLayer(_ index: Int) throws -> MTLTexture {
        let path = "\(dir)/panorama-\(frame)-\(index).bgra"
        let data = try Data(contentsOf: URL(fileURLWithPath: path))
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
            width: layerW, height: layerH, mipmapped: false)
        d.usage = .shaderRead
        let t = device.makeTexture(descriptor: d)!
        data.withUnsafeBytes { raw in
            t.replace(region: MTLRegionMake2D(0, 0, layerW, layerH), mipmapLevel: 0,
                      withBytes: raw.baseAddress!, bytesPerRow: layerW * 4)
        }
        return t
    }
    // Source order is the six bearings round the ring, then sky and floor.
    let layerIndex = [0, 1, 2, 7, 8, 9, 5, 6]
    // A control: every panel handed the same flat grey. Whatever texture the
    // cameras disagree about is gone, so anything left that is not flat grey
    // is the blend itself, not the pictures.
    let flat = ProcessInfo.processInfo.environment["PREVIEW_FLAT"] != nil
    func greyLayer() -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
            width: layerW, height: layerH, mipmapped: false)
        d.usage = .shaderRead
        let t = device.makeTexture(descriptor: d)!
        var pixels = [UInt32](repeating: 0xff808080, count: layerW * layerH)
        pixels.withUnsafeBytes {
            t.replace(region: MTLRegionMake2D(0, 0, layerW, layerH), mipmapLevel: 0,
                      withBytes: $0.baseAddress!, bytesPerRow: layerW * 4)
        }
        return t
    }
    let textures = try flat ? layerIndex.map { _ in greyLayer() } : layerIndex.map { try loadLayer($0) }

    let bandX: Float = 1 / tan(32.0 * .pi / 180)
    let capX: Float = 1 / tan(capHalf * .pi / 180)
    let projY: Float = 1 / tan(52.5 * .pi / 180)
    let projections: [SIMD2<Float>] = Array(repeating: SIMD2(bandX, projY), count: 6)
                                    + [SIMD2(capX, projY), SIMD2(capX, projY)]
    let viewports = [SIMD4<Float>](repeating: SIMD4(0, 0, 1, 1), count: 8)
    let sources = EngineImmersiveScreenGeometry.panoramaSources(projections: projections, viewports: viewports)
    let meshes = EngineImmersiveScreenGeometry.sphericalPanorama(
        sources: sources, texelSize: SIMD2(1.0 / Float(layerW), 1.0 / Float(layerH)))

    let pd = MTLRenderPipelineDescriptor()
    pd.vertexFunction = library(device, shader).makeFunction(name: "curve_vertex")
    pd.fragmentFunction = library(device, shader).makeFunction(name: "curve_fragment")
    pd.colorAttachments[0].pixelFormat = .bgra8Unorm
    pd.colorAttachments[0].isBlendingEnabled = true
    pd.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
    pd.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
    pd.colorAttachments[0].sourceAlphaBlendFactor = .one
    pd.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
    pd.depthAttachmentPixelFormat = .depth32Float
    let pipeline = try device.makeRenderPipelineState(descriptor: pd)
    let dd = MTLDepthStencilDescriptor(); dd.depthCompareFunction = .always; dd.isDepthWriteEnabled = true
    let depthState = device.makeDepthStencilState(descriptor: dd)!

    let W = 900, H = 900
    let cd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: W, height: H, mipmapped: false)
    cd.usage = [.renderTarget, .shaderRead]
    let colour = device.makeTexture(descriptor: cd)!
    let zd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float, width: W, height: H, mipmapped: false)
    zd.usage = .renderTarget
    let depthTex = device.makeTexture(descriptor: zd)!

    // The virtual eye's half field. Narrowing it magnifies the source the
    // way the headset does, which is the only condition under which the
    // reconstruction filter matters at all.
    let eyeHalfFov = ProcessInfo.processInfo.environment["PREVIEW_HALF_FOV"].flatMap { Float($0) } ?? 55.0
    let f: Float = 1 / tan(eyeHalfFov * .pi / 180)
    var proj = matrix_identity_float4x4
    proj.columns.0 = SIMD4(f, 0, 0, 0); proj.columns.1 = SIMD4(0, f, 0, 0)
    proj.columns.2 = SIMD4(0, 0, 100 / (0.01 - 100), -1)
    proj.columns.3 = SIMD4(0, 0, 100 * 0.01 / (0.01 - 100), 0)
    let rot = simd_float4x4(simd_quatf(angle: lookYaw, axis: SIMD3(0, 1, 0)))
          * simd_float4x4(simd_quatf(angle: lookPitch, axis: SIMD3(1, 0, 0)))
    var matrices = [proj * simd_inverse(rot)]

    let command = queue.makeCommandBuffer()!
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = colour
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1)   // magenta = nothing drawn
    pass.depthAttachment.texture = depthTex
    pass.depthAttachment.loadAction = .clear
    pass.depthAttachment.clearDepth = 0
    let encoder = command.makeRenderCommandEncoder(descriptor: pass)!
    encoder.setRenderPipelineState(pipeline)
    encoder.setDepthStencilState(depthState)
    encoder.setCullMode(.none)
    matrices.withUnsafeBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 1) }
    var eyeBase: UInt32 = 0
    encoder.setVertexBytes(&eyeBase, length: 4, index: 2)
    for source in EngineImmersiveScreenGeometry.panoramaPanelOrder {
        let mesh = meshes[source]
        let vb = device.makeBuffer(bytes: mesh.vertices, length: mesh.vertices.count * MemoryLayout<EngineImmersiveScreenVertex>.stride)!
        let ib = device.makeBuffer(bytes: mesh.indices, length: mesh.indices.count * 4)!
        encoder.setVertexBuffer(vb, offset: 0, index: 0)
        encoder.setFragmentTexture(textures[source], index: 0)
        encoder.setFragmentTexture(textures[source], index: 1)
        encoder.drawIndexedPrimitives(type: .triangle, indexCount: mesh.indices.count,
                                      indexType: .uint32, indexBuffer: ib, indexBufferOffset: 0)
    }
    encoder.endEncoding()
    let readback = device.makeBuffer(length: W * H * 4, options: .storageModeShared)!
    let blit = command.makeBlitCommandEncoder()!
    blit.copy(from: colour, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
              sourceSize: MTLSize(width: W, height: H, depth: 1), to: readback, destinationOffset: 0,
              destinationBytesPerRow: W * 4, destinationBytesPerImage: W * H * 4)
    blit.endEncoding(); command.commit(); command.waitUntilCompleted()

    let px = readback.contents().bindMemory(to: UInt8.self, capacity: W * H * 4)
    var ppm = Data("P6\n\(W) \(H)\n255\n".utf8)
    var uncovered = 0
    for i in 0..<(W * H) {
        let b = px[i*4], g = px[i*4+1], r = px[i*4+2]
        if r > 250 && b > 250 && g < 5 { uncovered += 1 }
        ppm.append(contentsOf: [r, g, b])
    }
    try ppm.write(to: URL(fileURLWithPath: outPath))
    print("wrote \(outPath); pixels nothing drew on: \(uncovered) of \(W*H)")
}
static func library(_ device: MTLDevice, _ source: String) -> MTLLibrary {
    try! device.makeLibrary(source: source, options: nil)
}
}
