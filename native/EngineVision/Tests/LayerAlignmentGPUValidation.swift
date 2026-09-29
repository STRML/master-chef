// Layer alignment on the Mac's Metal GPU. The renderer's shader is extracted
// verbatim from EngineImmersive.swift (argument 1) and the aligned functions
// appended, exactly as the renderer builds its second library. Four checks:
//  1. With nothing turned, the aligned pipeline draws the baked panorama.
//  2. Under planned turns the eight panels still cover every pixel fully.
//  3. A synthetic world drawn into each layer from an older camera and shown
//     through the aligned pipeline matches the world seen from the newest
//     camera; shown as drawn it does not. This checks the constants' layout,
//     the vertex turn, the texture orientation and the per-fragment fades
//     end to end. (Synthetic only: no parallax, animation or weapon.)
//  4. The renderer's lazily built pipeline: nothing compiled until asked,
//     then built off the caller's thread; a broken source stays off.
import Foundation
import Metal
import simd

typealias Source = EngineImmersiveScreenGeometry.PanoramaSource

@main struct LayerAlignmentGPUValidation {
    static func main() throws {
        setbuf(stdout, nil)
        let source = try String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8)
        let shader = source.components(separatedBy: "private static let shader = \"\"\"")[1].components(separatedBy: "\"\"\"")[0]
        let device = MTLCreateSystemDefaultDevice()!
        let queue = device.makeCommandQueue()!
        let library = try device.makeLibrary(source: shader + EngineLayerAlignment.shaderSource, options: nil)
        let format = MTLPixelFormat.rgba16Float
        func pipeline(_ vertex: String, _ fragment: String) throws -> MTLRenderPipelineState {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: vertex)!
            d.fragmentFunction = library.makeFunction(name: fragment)!
            d.colorAttachments[0].pixelFormat = format
            d.maxVertexAmplificationCount = 1
            // The panorama's blending (EngineImmersiveRenderer.panoramaPipeline).
            d.colorAttachments[0].isBlendingEnabled = true
            d.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
            d.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
            d.colorAttachments[0].sourceAlphaBlendFactor = .one
            d.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
            return try device.makeRenderPipelineState(descriptor: d)
        }
        let baked = try pipeline("curve_vertex", "curve_fragment")
        let aligned = try pipeline("aligned_vertex", "aligned_fragment")

        let ring = SIMD2<Float>(1.5847, 0.7593), cap = SIMD2<Float>(0.8910, 0.7593)
        let sources: [Source] = EngineImmersiveScreenGeometry.panoramaSources(
            projections: Array(repeating: ring, count: 6) + [cap, cap], viewports: Array(repeating: SIMD4<Float>(0, 0, 1, 1), count: 8))
        let textureSize = (width: 256, height: 192)
        let meshes = EngineImmersiveScreenGeometry.sphericalPanorama(
            sources: sources, texelSize: SIMD2(1 / Float(textureSize.width), 1 / Float(textureSize.height)))
        let buffers = meshes.map { mesh -> (MTLBuffer, MTLBuffer, Int) in
            (device.makeBuffer(bytes: mesh.vertices, length: mesh.vertices.count * MemoryLayout<EngineImmersiveScreenVertex>.stride)!,
             device.makeBuffer(bytes: mesh.indices, length: mesh.indices.count * 4)!, mesh.indices.count)
        }

        func radians(_ d: Float) -> Float { d * .pi / 180 }
        func pose(yaw: Float, pitch: Float) -> EngineLayerPose {
            let y = radians(yaw), p = radians(pitch)
            return EngineLayerPose(position: .zero, forward: SIMD3(cos(p) * cos(y), cos(p) * sin(y), sin(p)),
                                   up: SIMD3(-sin(p) * cos(y), -sin(p) * sin(y), cos(p)))
        }
        /// A smooth colour for every world direction: a panel that shows the wrong
        /// direction shows the wrong colour.
        func world(_ w: SIMD3<Float>) -> SIMD4<Float> {
            SIMD4(0.5 + 0.5 * sin(6 * w.x + 1), 0.5 + 0.5 * sin(6 * w.y + 2), 0.5 + 0.5 * sin(6 * w.z + 3), 1)
        }
        /// Each panel's picture, drawn from `cameras[panel]`: texel (u, v) shows the
        /// world direction the host's camera for that layer would put there.
        func pictures(_ cameras: [EngineLayerPose]?) -> [MTLTexture] {
            (0..<8).map { panel in
                let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: textureSize.width,
                                                                 height: textureSize.height, mipmapped: false)
                d.usage = .shaderRead
                let texture = device.makeTexture(descriptor: d)!
                var texels = [SIMD4<Float>](repeating: SIMD4(1, 1, 1, 1), count: textureSize.width * textureSize.height)
                if let cameras {
                    let m = cameras[panel].presenterFromWorld!, (right, up, forward) = EngineImmersiveScreenGeometry.basis(of: sources[panel])
                    for y in 0..<textureSize.height { for x in 0..<textureSize.width {
                        let s = (Float(x) + 0.5) / Float(textureSize.width) * 2 - 1, t = (Float(y) + 0.5) / Float(textureSize.height) * 2 - 1
                        let d = simd_normalize(right * (s / sources[panel].projection.x) - up * (t / sources[panel].projection.y) + forward)
                        texels[y * textureSize.width + x] = world(m.transpose * d)
                    } }
                }
                texture.replace(region: MTLRegionMake2D(0, 0, textureSize.width, textureSize.height), mipmapLevel: 0,
                                withBytes: texels, bytesPerRow: textureSize.width * 16)
                return texture
            }
        }

        /// A 90-degree view from the sphere's centre towards (azimuth, elevation).
        struct View { let azimuth: Float, elevation: Float }
        let size = 160
        func viewMatrix(_ v: View) -> (matrix: simd_float4x4, forward: SIMD3<Float>, right: SIMD3<Float>, up: SIMD3<Float>) {
            let a = radians(v.azimuth), e = radians(v.elevation)
            let forward = SIMD3(sin(a) * cos(e), sin(e), -cos(a) * cos(e))
            let right = simd_normalize(simd_cross(forward, abs(e) > 89 ? SIMD3(0, 0, -1) : SIMD3(0, 1, 0)))
            let up = simd_cross(right, forward)
            let view = simd_float4x4(rows: [SIMD4(right, 0), SIMD4(up, 0), SIMD4(-forward, 0), SIMD4(0, 0, 0, 1)])
            let near: Float = 0.1, far: Float = 100
            let projection = simd_float4x4(rows: [SIMD4(1, 0, 0, 0), SIMD4(0, 1, 0, 0),
                                                  SIMD4(0, 0, far / (near - far), near * far / (near - far)), SIMD4(0, 0, -1, 0)])
            return (projection * view, forward, right, up)
        }
        func pixelDirection(_ v: View, _ x: Int, _ y: Int) -> SIMD3<Float> {
            let (_, forward, right, up) = viewMatrix(v)
            let sx = (Float(x) + 0.5) / Float(size) * 2 - 1, sy = 1 - (Float(y) + 0.5) / Float(size) * 2
            return simd_normalize(forward + right * sx + up * sy)
        }

        func render(_ v: View, pipeline: MTLRenderPipelineState, pictures: [MTLTexture],
                    constants: [EngineLayerAlignment.PanelConstants]?) -> [SIMD4<Float>] {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: size, height: size, mipmapped: false)
            d.usage = [.renderTarget, .shaderRead]; d.storageMode = .shared
            let target = device.makeTexture(descriptor: d)!
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            let command = queue.makeCommandBuffer()!, encoder = command.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline); encoder.setCullMode(.none)
            var mapping = [MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: 0, renderTargetArrayIndexOffset: 0)]
            encoder.setVertexAmplificationCount(1, viewMappings: &mapping)
            var matrix = viewMatrix(v).matrix, eyeBase: UInt32 = 0
            encoder.setVertexBytes(&matrix, length: MemoryLayout<simd_float4x4>.size, index: 1)
            encoder.setVertexBytes(&eyeBase, length: 4, index: 2)
            for panel in EngineImmersiveScreenGeometry.panoramaPanelOrder {
                encoder.setVertexBuffer(buffers[panel].0, offset: 0, index: 0)
                if let constants {
                    constants[panel].vertex.withUnsafeBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 3) }
                    constants[panel].fragment.withUnsafeBytes { encoder.setFragmentBytes($0.baseAddress!, length: $0.count, index: 2) }
                }
                var filter: UInt32 = panel == 1 ? 1 : 0
                encoder.setFragmentBytes(&filter, length: 4, index: 1)
                encoder.setFragmentTexture(pictures[panel], index: 0); encoder.setFragmentTexture(pictures[panel], index: 1)
                encoder.drawIndexedPrimitives(type: .triangle, indexCount: buffers[panel].2, indexType: .uint32,
                                              indexBuffer: buffers[panel].1, indexBufferOffset: 0)
            }
            encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
            precondition(command.status == .completed, "GPU error: \(String(describing: command.error))")
            var halves = [Float16](repeating: 0, count: size * size * 4)
            target.getBytes(&halves, bytesPerRow: size * 8, from: MTLRegionMake2D(0, 0, size, size), mipmapLevel: 0)
            var pixels = [SIMD4<Float>]()
            pixels.reserveCapacity(size * size)
            for i in 0..<size * size {
                let r = Float(halves[i * 4]), g = Float(halves[i * 4 + 1]), b = Float(halves[i * 4 + 2]), a = Float(halves[i * 4 + 3])
                pixels.append(SIMD4(r, g, b, a))
            }
            return pixels
        }

        let views = [View(azimuth: 0, elevation: 0), View(azimuth: 30, elevation: 10), View(azimuth: -30, elevation: -20),
                     View(azimuth: 90, elevation: 0), View(azimuth: -150, elevation: 30), View(azimuth: 30, elevation: 45),
                     View(azimuth: -60, elevation: -45), View(azimuth: 10, elevation: 89.5), View(azimuth: 200, elevation: -89.5)]
        let identityPlan = EngineLayerAlignment.plan(sources: sources, desired: Array(repeating: EngineLayerAlignment.identityQuaternion, count: 8))
        let identityConstants = EngineLayerAlignment.constants(sources: sources, rotations: identityPlan.rotations)

        // 1. Nothing turned: the aligned pipeline draws what the baked one draws.
        do {
            let still = Array(repeating: pose(yaw: 20, pitch: -10), count: 8)
            let picture = pictures(still)
            var worst: Float = 0, total: Float = 0, count = 0
            for v in views {
                let a = render(v, pipeline: baked, pictures: picture, constants: nil)
                let b = render(v, pipeline: aligned, pictures: picture, constants: identityConstants)
                for i in a.indices {
                    let difference = simd_reduce_max(abs(SIMD3(a[i].x, a[i].y, a[i].z) - SIMD3(b[i].x, b[i].y, b[i].z)))
                    worst = max(worst, difference); total += difference; count += 1
                }
            }
            print(String(format: "unturned: aligned vs baked colour differs by at most %.4f (mean %.6f)", worst, total / Float(count)))
            // Only the baked mesh's per-vertex fade against the exact per-fragment one.
            precondition(worst < 0.02 && total / Float(count) < 0.0005, "with nothing turned the aligned pipeline must draw the baked panorama")
        }

        // Plans from what the headset does: full stick (2.3 degrees a frame)
        // with layers of Build75's ages, level, then pitched with the aim
        // moving and the rear and caps most of a second old.
        func planFor(rate: Float, pitch: Float, pitchRate: Float, ages: [Int]) -> (plan: EngineLayerAlignment.Plan, cameras: [EngineLayerPose], centre: EngineLayerPose) {
            var poses = [EngineLayerPose?](repeating: nil, count: 10), epochs = [UInt64](repeating: 1000, count: 10)
            var cameras = [EngineLayerPose]()
            for (panel, layer) in EngineLayerAlignment.panelLayers.enumerated() {
                let camera = pose(yaw: -rate * Float(ages[panel]), pitch: pitch - pitchRate * Float(ages[panel]))
                poses[layer] = camera; epochs[layer] = 1000 - UInt64(ages[panel]); cameras.append(camera)
            }
            poses[3] = poses[1]; poses[4] = poses[1]
            let desired = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 1)
            return (EngineLayerAlignment.plan(sources: sources, desired: desired.rotations), cameras, poses[1]!)
        }
        let scenarios = [planFor(rate: 2.3, pitch: 0, pitchRate: 0, ages: [1, 0, 2, 5, 9, 3, 7, 11]),
                         planFor(rate: -2.3, pitch: -30, pitchRate: 0.8, ages: [2, 0, 1, 18, 20, 19, 17, 22]),
                         planFor(rate: 2.3, pitch: 25, pitchRate: -0.6, ages: [3, 0, 3, 17, 18, 16, 20, 19])]

        // 2. Turned, every pixel is still fully covered: white pictures composite
        // to white, with no hole and no dim band.
        do {
            let white = pictures(nil)
            var darkest: Float = 1
            for scenario in scenarios {
                let constants = EngineLayerAlignment.constants(sources: sources, rotations: scenario.plan.rotations)
                for v in views {
                    for pixel in render(v, pipeline: aligned, pictures: white, constants: constants) { darkest = min(darkest, pixel.x, pixel.w) }
                }
            }
            print(String(format: "turned: darkest composited pixel %.4f", darkest))
            precondition(darkest > 0.995, "a turned plan left a hole or a dim band")
        }

        // 3. The world lines up. Each layer is drawn from its own older camera;
        // through the aligned pipeline every pixel shows what the newest camera
        // sees there. Drawn as they are, the older layers show the world displaced.
        do {
            for (index, scenario) in scenarios.enumerated() {
                let picture = pictures(scenario.cameras)
                let constants = EngineLayerAlignment.constants(sources: sources, rotations: scenario.plan.rotations)
                let newest = scenario.centre.presenterFromWorld!
                var alignedError: Float = 0, bakedError: Float = 0, samples = 0, perView: [String] = [], worse = false
                for v in views {
                    let a = render(v, pipeline: aligned, pictures: picture, constants: constants)
                    let b = render(v, pipeline: baked, pictures: picture, constants: nil)
                    var viewAligned: Float = 0, viewBaked: Float = 0
                    for y in 0..<size { for x in 0..<size {
                        let truth = world(newest.transpose * pixelDirection(v, x, y))
                        let i = y * size + x
                        viewAligned += simd_reduce_max(abs(SIMD3(a[i].x, a[i].y, a[i].z) - SIMD3(truth.x, truth.y, truth.z)))
                        viewBaked += simd_reduce_max(abs(SIMD3(b[i].x, b[i].y, b[i].z) - SIMD3(truth.x, truth.y, truth.z)))
                        samples += 1
                    } }
                    alignedError += viewAligned; bakedError += viewBaked
                    worse = worse || viewAligned > viewBaked + 0.005 * Float(size * size)
                    perView.append(String(format: "%.0f/%.0f %.3f<%.3f", v.azimuth, v.elevation, viewAligned / Float(size * size), viewBaked / Float(size * size)))
                }
                print("  per view (azimuth/elevation aligned<drawn): " + perView.joined(separator: ", "))
                // However far the joins held a layer back, turning it never
                // makes any view worse than showing it as drawn.
                precondition(!worse, "alignment made a view worse")
                alignedError /= Float(samples); bakedError /= Float(samples)
                let applied = scenario.plan.appliedDegrees.map { String(format: "%.1f", $0) }.joined(separator: " ")
                print(String(format: "scenario %d (turns %@): mean colour error %.4f aligned, %.4f as drawn", index, applied, alignedError, bakedError))
                // The stick's own case (scenario 0) is mostly within the joins'
                // reach; layers a second old with the aim moving are held back
                // by the overlaps and gain less.
                precondition(alignedError < bakedError * (index == 0 ? 0.3 : 0.95), "alignment did not move the older layers towards the newest camera")
            }
            // A plan whose every turn is complete lines up exactly (to filtering).
            let exact = planFor(rate: 2.3, pitch: 0, pitchRate: 0, ages: [1, 0, 1, 1, 1, 1, 3, 5])
            precondition(exact.plan.level == .full && zip(exact.plan.appliedDegrees, exact.plan.desiredDegrees).allSatisfy { abs($0 - $1) < 0.01 })
            // Against the same world drawn with every layer from the newest
            // camera (nothing to turn): only sampling separates the two.
            let picture = pictures(exact.cameras), newest = exact.centre.presenterFromWorld!
            let reference = pictures(Array(repeating: exact.centre, count: 8))
            let constants = EngineLayerAlignment.constants(sources: sources, rotations: exact.plan.rotations)
            var worstExcess: Float = 0, worstMean: Float = 0
            for v in views {
                let a = render(v, pipeline: aligned, pictures: picture, constants: constants)
                let r = render(v, pipeline: baked, pictures: reference, constants: nil)
                var error: Float = 0, floor: Float = 0, counted = 0
                for y in 0..<size { for x in 0..<size {
                    // The side bands' weapon region is left as drawn by design.
                    let p = pixelDirection(v, x, y)
                    if EngineLayerAlignment.correctionWeight(p) < 1 && abs(p.x) > 0.4 { continue }
                    let truth = world(newest.transpose * p), i = y * size + x
                    error += simd_reduce_max(abs(SIMD3(a[i].x, a[i].y, a[i].z) - SIMD3(truth.x, truth.y, truth.z)))
                    floor += simd_reduce_max(abs(SIMD3(r[i].x, r[i].y, r[i].z) - SIMD3(truth.x, truth.y, truth.z)))
                    counted += 1
                } }
                guard counted > 0 else { continue }
                print(String(format: "  view %.0f/%.0f: %.4f, floor %.4f", v.azimuth, v.elevation, error / Float(counted), floor / Float(counted)))
                worstMean = max(worstMean, error / Float(counted))
                worstExcess = max(worstExcess, (error - floor) / Float(counted))
            }
            print(String(format: "fully turned plan: worst view's mean colour error %.4f, %.4f above drawing every layer from the newest camera", worstMean, worstExcess))
            precondition(worstExcess < 0.003, "a complete turn must show the newest camera's world")
        }
        // Cost, for the report: both pipelines drawing all eight panels into
        // a headset-sized eye (1920 x 1824, no foveation), best of several.
        do {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: 1920, height: 1824, mipmapped: false)
            d.usage = .renderTarget; d.storageMode = .private
            let target = device.makeTexture(descriptor: d)!
            let picture = pictures(scenarios[0].cameras)
            let constants = EngineLayerAlignment.constants(sources: sources, rotations: scenarios[0].plan.rotations)
            func time(_ pipeline: MTLRenderPipelineState, _ constants: [EngineLayerAlignment.PanelConstants]?) -> Double {
                var best = Double.infinity
                for _ in 0..<12 {
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target; pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].storeAction = .dontCare
                    let command = queue.makeCommandBuffer()!, encoder = command.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(pipeline); encoder.setCullMode(.none)
                    var mapping = [MTLVertexAmplificationViewMapping(viewportArrayIndexOffset: 0, renderTargetArrayIndexOffset: 0)]
                    encoder.setVertexAmplificationCount(1, viewMappings: &mapping)
                    // A wide eye (about 100 by 95 degrees) looking at the join.
                    var matrix = viewMatrix(View(azimuth: 20, elevation: 15)).matrix, eyeBase: UInt32 = 0
                    let wide = simd_float4x4(diagonal: SIMD4(0.84, 0.84, 1, 1))
                    matrix = wide * matrix
                    encoder.setVertexBytes(&matrix, length: MemoryLayout<simd_float4x4>.size, index: 1)
                    encoder.setVertexBytes(&eyeBase, length: 4, index: 2)
                    for panel in EngineImmersiveScreenGeometry.panoramaPanelOrder {
                        encoder.setVertexBuffer(buffers[panel].0, offset: 0, index: 0)
                        if let constants {
                            constants[panel].vertex.withUnsafeBytes { encoder.setVertexBytes($0.baseAddress!, length: $0.count, index: 3) }
                            constants[panel].fragment.withUnsafeBytes { encoder.setFragmentBytes($0.baseAddress!, length: $0.count, index: 2) }
                        }
                        var filter: UInt32 = panel == 1 ? 1 : 0
                        encoder.setFragmentBytes(&filter, length: 4, index: 1)
                        encoder.setFragmentTexture(picture[panel], index: 0); encoder.setFragmentTexture(picture[panel], index: 1)
                        encoder.drawIndexedPrimitives(type: .triangle, indexCount: buffers[panel].2, indexType: .uint32,
                                                      indexBuffer: buffers[panel].1, indexBufferOffset: 0)
                    }
                    encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
                    best = min(best, (command.gpuEndTime - command.gpuStartTime) * 1000)
                }
                return best
            }
            let bakedTime = time(baked, nil), alignedTime = time(aligned, constants)
            print(String(format: "one eye, 1920x1824: baked panels %.3f ms, aligned %.3f ms on this Mac's GPU", bakedTime, alignedTime))
        }

        // 4. The renderer's pipeline is built on first request, in the
        // background, from a copy of the panorama descriptor; nothing is
        // compiled until then, and a source that does not build leaves it
        // nil for good rather than failing the renderer.
        do {
            func descriptor() -> MTLRenderPipelineDescriptor {
                let d = MTLRenderPipelineDescriptor()
                d.vertexFunction = library.makeFunction(name: "curve_vertex")!
                d.fragmentFunction = library.makeFunction(name: "curve_fragment")!
                d.colorAttachments[0].pixelFormat = format
                d.maxVertexAmplificationCount = 1
                d.colorAttachments[0].isBlendingEnabled = true
                d.colorAttachments[0].sourceRGBBlendFactor = .sourceAlpha
                d.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
                d.colorAttachments[0].sourceAlphaBlendFactor = .one
                d.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
                return d
            }
            func settle(_ build: EngineAlignedPipeline) -> Double {
                let start = Date()
                while !build.settled && Date().timeIntervalSince(start) < 30 { usleep(2000) }
                precondition(build.settled, "the aligned pipeline build never finished")
                return Date().timeIntervalSince(start) * 1000
            }
            let original = descriptor()
            let lazy = EngineAlignedPipeline(device: device, source: shader + EngineLayerAlignment.shaderSource,
                                             descriptor: original.copy() as! MTLRenderPipelineDescriptor)
            usleep(50_000)
            precondition(!lazy.settled, "nothing is compiled before the first request")
            let started = Date()
            precondition(lazy.pipeline() == nil, "the first request only starts the build")
            let requestMilliseconds = Date().timeIntervalSince(started) * 1000
            let buildMilliseconds = settle(lazy)
            precondition(lazy.pipeline() != nil, "the aligned pipeline builds from the shipped shader")
            precondition(original.vertexFunction?.name == "curve_vertex", "the renderer's own descriptor is left alone")
            print(String(format: "aligned pipeline: request returned in %.2f ms, built in the background in %.0f ms", requestMilliseconds, buildMilliseconds))
            precondition(requestMilliseconds < 20, "the request must not compile on the caller's thread")
            print("expected next: a deliberately broken shader source fails to build and alignment stays off")
            let broken = EngineAlignedPipeline(device: device, source: shader + "\nthis does not compile\n", descriptor: descriptor())
            _ = broken.pipeline(); _ = settle(broken)
            precondition(broken.pipeline() == nil, "a pipeline that does not build stays off")
        }
        print("PASS layer alignment on the GPU: shipped shader builds, unturned output unchanged, no holes, older layers meet the newest camera")
    }
}
