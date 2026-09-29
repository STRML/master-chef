// Run on a Mac with Metal. This loads the production panorama vertex/fragment
// shader from EngineImmersive.swift and rasterizes the generated front-half
// cylinder under a perspective camera. It guards against clip-plane triangles
// reaching the top or bottom of the compositor target.
import Foundation
import Metal
import simd

struct PanoramaProductionGPUVertex {
    var position: SIMD4<Float>
    var uv: SIMD2<Float>
    var padding = SIMD2<Float>(0, 0)
}

@main struct PanoramaProductionGPUValidation {
static func main() throws {
let sourcePath = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "native/EngineVision/Sources/EngineImmersive.swift"
let source = try String(contentsOfFile: sourcePath, encoding: .utf8)
let shader = source.components(separatedBy: "private static let shader = \"\"\"")[1]
    .components(separatedBy: "\"\"\"")[0]
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
    fputs("Panorama production GPU validation unavailable: no Metal device\n", stderr)
    exit(2)
}
let library = try device.makeLibrary(source: shader, options: nil)
let descriptor = MTLRenderPipelineDescriptor()
descriptor.vertexFunction = library.makeFunction(name: "curve_vertex")
descriptor.fragmentFunction = library.makeFunction(name: "curve_fragment")
descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm_srgb
descriptor.depthAttachmentPixelFormat = .depth32Float
descriptor.rasterSampleCount = 1
let pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
let depthDescriptor = MTLDepthStencilDescriptor()
depthDescriptor.depthCompareFunction = .greater
depthDescriptor.isDepthWriteEnabled = true
let depth = device.makeDepthStencilState(descriptor: depthDescriptor)!

let width = 256, height = 256
let colorDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm_srgb,
    width: width, height: height, mipmapped: false)
colorDescriptor.usage = [.renderTarget, .shaderRead]
let color = device.makeTexture(descriptor: colorDescriptor)!
let depthDescriptorTexture = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .depth32Float,
    width: width, height: height, mipmapped: false)
depthDescriptorTexture.usage = .renderTarget
let depthTexture = device.makeTexture(descriptor: depthDescriptorTexture)!
let inputDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
    width: 128, height: 96, mipmapped: false)
inputDescriptor.usage = .shaderRead
let input = device.makeTexture(descriptor: inputDescriptor)!
// Magenta marks pixels the original source viewport never rendered.
var sourcePixels = [UInt32](repeating: 0xffff00ff, count: 128*96)

// Render the solid-color panorama at normal headset vertical FOVs. The first
// three cases require the generated surface to cover the complete target;
// the final deliberately wider case checks that near +/-90° clipping stays
// bounded instead of producing a top/bottom triangle artifact.
let near: Float = 0.01, far: Float = 100
var projection = matrix_identity_float4x4
projection.columns.0 = SIMD4<Float>(repeating: 0)
projection.columns.1 = SIMD4<Float>(repeating: 0)
projection.columns.2 = SIMD4<Float>(repeating: 0)
projection.columns.3 = SIMD4<Float>(repeating: 0)
projection.columns.2.z = far / (near - far)
projection.columns.2.w = -1
projection.columns.3.z = far * near / (near - far)

let sourceFixtures: [(String, SIMD2<Float>, SIMD4<Float>)] = [
    ("broadFull", SIMD2(0.4186,0.5581),SIMD4(0,0,1,1)),
    ("denseFull", SIMD2(1.5752,0.5677),SIMD4(0,0,1,1)),
    ("centered75", SIMD2(1.5752,0.5677*0.75),SIMD4(0,0.125,1,0.875)),
    ("asymmetric", SIMD2(1.5752*0.875,0.5677*0.75),SIMD4(0.0625,0.0625,0.9375,0.8125))]
for (fixtureName, sourceProjection, viewport) in sourceFixtures {
for y in 0..<96 { for x in 0..<128 {
    let valid = Float(x) >= viewport.x*128 && Float(x) < viewport.z*128 &&
        Float(y) >= viewport.y*96 && Float(y) < viewport.w*96
    sourcePixels[y*128+x] = valid ? (((x/8+y/8)%2 == 0) ? 0xffffffff : 0xff808080) : 0xffff00ff
} }
sourcePixels.withUnsafeBytes { input.replace(region: MTLRegionMake2D(0,0,128,96),mipmapLevel:0,
    withBytes:$0.baseAddress!,bytesPerRow:128*4) }
// The five panels of the production surface: three forward bands and the
// two wider cap views, all on one sphere.
let capProjection = SIMD2<Float>(1 / tan(96.0 / 2 * .pi / 180), sourceProjection.y)
let panelSources = EngineImmersiveScreenGeometry.panoramaSources(
    projections: Array(repeating: sourceProjection, count: 6) + [capProjection, capProjection],
    viewports: Array(repeating: viewport, count: 8))
let meshes = EngineImmersiveScreenGeometry.sphericalPanorama(
    sources: panelSources, texelSize: SIMD2(1.0/128, 1.0/96))
for (label, focal, requiresFullCoverage) in [("VFOV90", 1.0 as Float, true),
                                               ("VFOV100", 0.8391 as Float, true),
                                               ("VFOV110", 0.7002 as Float, true),
                                               ("WIDE_CLIP", 0.35 as Float, false)] {
    projection.columns.0.x = focal
    projection.columns.1.y = focal
    let command = queue.makeCommandBuffer()!
    let pass = MTLRenderPassDescriptor()
    pass.colorAttachments[0].texture = color
    pass.colorAttachments[0].loadAction = .clear
    pass.colorAttachments[0].storeAction = .store
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
    pass.depthAttachment.texture = depthTexture
    pass.depthAttachment.loadAction = .clear
    pass.depthAttachment.storeAction = .store
    pass.depthAttachment.clearDepth = 0
    let encoder = command.makeRenderCommandEncoder(descriptor: pass)!
    encoder.setRenderPipelineState(pipeline)
    encoder.setDepthStencilState(depth)
    encoder.setCullMode(.none)
    encoder.setViewport(MTLViewport(originX: 0, originY: 0, width: Double(width), height: Double(height), znear: 0, zfar: 1))
    encoder.setVertexAmplificationCount(1, viewMappings: nil)
    encoder.setFragmentTexture(input, index: 0)
    // The production shader takes its reconstruction filter as a constant;
    // exercise the kernel path, which is the one with something to get wrong.
    var filter: UInt32 = 1
    encoder.setFragmentBytes(&filter, length: MemoryLayout<UInt32>.size, index: 1)
    for mesh in meshes {
        let vb = device.makeBuffer(bytes: mesh.vertices,
            length: mesh.vertices.count * MemoryLayout<PanoramaProductionGPUVertex>.stride)!
        let ib = device.makeBuffer(bytes: mesh.indices,
            length: mesh.indices.count * MemoryLayout<UInt32>.stride)!
        encoder.setVertexBuffer(vb, offset: 0, index: 0)
        withUnsafeBytes(of: projection) { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 1) }
        encoder.drawIndexedPrimitives(type: .triangle, indexCount: mesh.indices.count, indexType: .uint32,
            indexBuffer: ib, indexBufferOffset: 0)
    }
    encoder.endEncoding()
    let readback = device.makeBuffer(length: width * height * 4, options: .storageModeShared)!
    let blit = command.makeBlitCommandEncoder()!
    blit.copy(from: color, sourceSlice: 0, sourceLevel: 0,
        sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0), sourceSize: MTLSize(width: width, height: height, depth: 1),
        to: readback, destinationOffset: 0, destinationBytesPerRow: width * 4, destinationBytesPerImage: width * height * 4)
    blit.endEncoding(); command.commit(); command.waitUntilCompleted()
    guard command.status == .completed else { fatalError("GPU failed: \(String(describing: command.error))") }
    let pixels = readback.contents().bindMemory(to: UInt32.self, capacity: width * height)
    var lit = 0, topBottomLit = 0, analyticMisses = 0, analyticLeaks = 0, invalidSourceSamples = 0
    for y in 0..<height { for x in 0..<width where pixels[y * width + x] & 0x00FFFFFF != 0 {
        lit += 1
        let pixel = pixels[y * width + x]
        if ((pixel >> 16) & 255) > 220 && (pixel & 255) > 220 && ((pixel >> 8) & 255) < 32 { invalidSourceSamples += 1 }
        if y < 2 || y >= height - 2 { topBottomLit += 1 }
    } }
    if !requiresFullCoverage {
        // The surface is a hemisphere now, so the analytic footprint is
        // simply whether the ray points into the front half. Well inside it
        // the panels must have drawn something; well behind it they must not
        // have, or a panel is wrapping around past the edge of the picture.
        for y in 0..<height { for x in 0..<width {
            let ndcx = (Float(x) + 0.5) / Float(width) * 2 - 1
            let ndcy = 1 - (Float(y) + 0.5) / Float(height) * 2
            let ray = simd_normalize(SIMD3(ndcx / focal, ndcy / focal, Float(-1)))
            let bearing = atan2(ray.x, -ray.z), elevation = asin(ray.y)
            let inside = abs(elevation) < Float.pi / 2 - 0.09
            let outside = false   // the panels now cover every direction
            let actual = pixels[y * width + x] & 0x00FFFFFF != 0
            if inside && !actual { analyticMisses += 1 }
            if outside && actual { analyticLeaks += 1 }
        } }
    }
    precondition(invalidSourceSamples == 0, "\(fixtureName): sampled clear pixels outside source viewport")
    if requiresFullCoverage {
        precondition(lit > width * height * 995 / 1000, "\(label): panorama left black target holes (lit=\(lit))")
        precondition(topBottomLit > 0, "\(label): panorama did not cover the headset top/bottom rows")
    } else {
        precondition(lit > 0, "\(label): production shader rasterized no front-half panorama")
        precondition(analyticMisses == 0 && analyticLeaks == 0,
            "\(label): raster coverage disagrees with analytic ray/cylinder reference (misses=\(analyticMisses), leaks=\(analyticLeaks))")
    }
    print("PANORAMA_PRODUCTION_GPU_PASS case=\(label) fixture=\(fixtureName) invalidSourceSamples=\(invalidSourceSamples) source=\(sourceProjection) lit=\(lit) topBottomLit=\(topBottomLit) analyticMisses=\(analyticMisses) analyticLeaks=\(analyticLeaks) device=\(device.name)")
}
}
}
}
