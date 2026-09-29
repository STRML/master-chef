// Run on the Mac's actual Metal GPU. Shader source is extracted verbatim
// from EngineImmersive.swift so this checks the shipped stereo shader.
import Foundation
import Metal
import simd

let source = try String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8)
let shader = source.components(separatedBy: "private static let shader = \"\"\"")[1]
    .components(separatedBy: "\"\"\"")[0]
let device = MTLCreateSystemDefaultDevice()!
let queue = device.makeCommandQueue()!
let library = try device.makeLibrary(source: shader, options: nil)
struct Vertex { var position: SIMD4<Float>; var uv: SIMD2<Float>; var padding = SIMD2<Float>(0, 0) }
let vertices: [Vertex] = [
    .init(position: [-0.5,-0.5,0.5,1], uv: [0,1]),
    .init(position: [0.5,-0.5,0.5,1], uv: [1,1]),
    .init(position: [-0.5,0.5,0.5,1], uv: [0,0]),
    .init(position: [0.5,0.5,0.5,1], uv: [1,0])]
let indices: [UInt32] = [0,1,2,2,1,3]
let vb = device.makeBuffer(bytes: vertices, length: vertices.count * MemoryLayout<Vertex>.stride)!
let ib = device.makeBuffer(bytes: indices, length: indices.count * 4)!
let inputDesc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: 1, height: 1, mipmapped: false)
inputDesc.usage = .shaderRead
let input = device.makeTexture(descriptor: inputDesc)!
// Legacy D3D display alpha is undefined: even zero must remain opaque.
var white: UInt32 = 0x00ffffff
input.replace(region: MTLRegionMake2D(0,0,1,1), mipmapLevel: 0, withBytes: &white, bytesPerRow: 4)
let pd = MTLRenderPipelineDescriptor()
pd.vertexFunction = library.makeFunction(name: "curve_vertex")
pd.fragmentFunction = library.makeFunction(name: "curve_fragment")
pd.colorAttachments[0].pixelFormat = .bgra8Unorm_srgb
pd.depthAttachmentPixelFormat = .depth32Float
pd.maxVertexAmplificationCount = 2
let dd = MTLDepthStencilDescriptor(); dd.depthCompareFunction = .greater; dd.isDepthWriteEnabled = true
let depthState = device.makeDepthStencilState(descriptor: dd)!

for sampleCount in [1, 4] where device.supportsTextureSampleCount(sampleCount) {
pd.rasterSampleCount = sampleCount
let pipeline = try device.makeRenderPipelineState(descriptor: pd)
for layout in ["layered", "shared", "dedicated"] {
    let width = layout == "shared" ? 256 : 128, height = 128
    let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm_srgb, width: width, height: height, mipmapped: false)
    desc.textureType = layout == "layered" ? .type2DArray : .type2D
    desc.arrayLength = layout == "layered" ? 2 : 1
    desc.storageMode = .private; desc.usage = .renderTarget
    let colors = (0..<(layout == "dedicated" ? 2 : 1)).map { _ in device.makeTexture(descriptor: desc)! }
    desc.pixelFormat = .depth32Float
    let depths = colors.map { _ in device.makeTexture(descriptor: desc)! }
    for blank in [false, true] {
    let command = queue.makeCommandBuffer()!
    for passIndex in 0..<colors.count {
        let pass = try EngineImmersiveRenderTargets.makePass(device: device, color: colors[passIndex],
            depth: depths[passIndex], arrayLength: layout == "layered" ? 2 : 1, sampleCount: sampleCount)
        let encoder = command.makeRenderCommandEncoder(descriptor: pass)!
        if blank { encoder.endEncoding(); continue }
        encoder.setRenderPipelineState(pipeline); encoder.setDepthStencilState(depthState); encoder.setCullMode(.none)
        let eyes = layout == "dedicated" ? 1 : 2
        encoder.setViewports((0..<eyes).map { MTLViewport(originX: layout == "shared" ? Double($0 * 128) : 0, originY: 0, width: 128, height: 128, znear: 0, zfar: 1) })
        var mappings = (0..<eyes).map { MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: UInt32($0), renderTargetArrayIndexOffset: layout == "layered" ? UInt32($0) : 0) }
        encoder.setVertexAmplificationCount(eyes, viewMappings: &mappings)
        encoder.setVertexBuffer(vb, offset: 0, index: 0)
        var transforms = [matrix_identity_float4x4, matrix_identity_float4x4]
        // Distinct eye positions expose writes to the wrong eye/layer.
        for eye in 0..<2 {
            transforms[eye].columns.0 = [cos(0.2),sin(0.2),0,0]
            transforms[eye].columns.1 = [-sin(0.2),cos(0.2),0,0]
        }
        transforms[0].columns.3.x = -0.2; transforms[1].columns.3.x = 0.2
        let matrices = layout == "dedicated" ? [transforms[passIndex]] : transforms
        matrices.withUnsafeBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 1) }
        encoder.setFragmentTexture(input, index: 0)
        encoder.drawIndexedPrimitives(type: .triangle, indexCount: indices.count, indexType: .uint32, indexBuffer: ib, indexBufferOffset: 0)
        encoder.endEncoding()
    }
    let buffer = device.makeBuffer(length: 128 * 128 * 4 * 2, options: .storageModeShared)!
    let blit = command.makeBlitCommandEncoder()!
    for eye in 0..<2 {
        blit.copy(from: colors[layout == "dedicated" ? eye : 0], sourceSlice: layout == "layered" ? eye : 0, sourceLevel: 0,
                  sourceOrigin: MTLOrigin(x: layout == "shared" ? eye * 128 : 0, y: 0, z: 0), sourceSize: MTLSize(width: 128, height: 128, depth: 1),
                  to: buffer, destinationOffset: eye * 128 * 128 * 4, destinationBytesPerRow: 512, destinationBytesPerImage: 128 * 128 * 4)
    }
    blit.endEncoding(); command.commit(); command.waitUntilCompleted()
    guard command.status == .completed else { fatalError("GPU failed: \(String(describing: command.error))") }
    let pixels = buffer.contents().bindMemory(to: UInt32.self, capacity: 128 * 128 * 2)
    for eye in 0..<2 {
        let offset = eye * 128 * 128
        let lit = (0..<(128 * 128)).filter { pixels[offset + $0] & 0xffffff != 0 }
        if blank {
            precondition((0..<(128*128)).allSatisfy { pixels[offset + $0] == 0xff000000 }, "stale tracking-loss frame")
            print("GPU_CLEAR_PASS \(layout) MSAA=\(sampleCount) eye\(eye)"); continue
        }
        let meanX = Double(lit.reduce(0) { $0 + $1 % 128 }) / Double(max(1, lit.count))
        precondition((4000...4300).contains(lit.count), "\(layout) eye\(eye): wrong lit pixel count \(lit.count)")
        precondition(abs(meanX - (eye == 0 ? 50.5 : 76.5)) < 1, "\(layout) eye\(eye): wrong eye position \(meanX)")
        let partial = lit.filter { pixels[offset + $0] & 0xff < 255 }.count
        precondition(sampleCount == 1 ? partial == 0 : partial > 100, "antialiasing did not cover diagonal edges")
        precondition((0..<(128*128)).allSatisfy { pixels[offset + $0] >> 24 == 255 }, "transparent output from legacy game alpha")
        for y in [0, 15, 112, 127] {
            for x in 0..<128 { precondition(pixels[offset + y*128 + x] == 0xff000000, "uncleared background") }
        }
        print("GPU_PASS \(layout) MSAA=\(sampleCount) eye\(eye) lit=\(lit.count) antialiasedPixels=\(partial) centerX=\(meanX) opaqueBlackBackground=true")
    }
}
}
}
print("IMMERSIVE_STEREO_GPU_VALIDATION_PASS device=\(device.name)")
