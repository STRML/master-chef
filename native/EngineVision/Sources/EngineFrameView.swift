import SwiftUI
import MetalKit
import GameController

struct EngineFrameView: UIViewRepresentable {
    func makeCoordinator() -> Renderer { Renderer() }
    func makeUIView(context: Context) -> MTKView {
        let view = MTKView(frame: .zero, device: MTLCreateSystemDefaultDevice())
        view.colorPixelFormat = .bgra8Unorm_srgb
        view.clearColor = MTLClearColor(red: 0.003, green: 0.008, blue: 0.014, alpha: 1)
        view.preferredFramesPerSecond = 60
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        let controllerEvents = GCEventInteraction()
        controllerEvents.handledEventTypes = .gamepad
        controllerEvents.receivesEventsInView = false
        view.addInteraction(controllerEvents)
        context.coordinator.configure(view)
        return view
    }
    func updateUIView(_ uiView: MTKView, context: Context) {}
    static func dismantleUIView(_ uiView: MTKView, coordinator: Renderer) { uiView.delegate = nil }

    final class Renderer: NSObject, MTKViewDelegate {
        private var queue: MTLCommandQueue?
        private var pipeline: MTLRenderPipelineState?
        private var frame: EngineFrameTexture?

        func configure(_ view: MTKView) {
            guard let device = view.device else { return }
            queue = device.makeCommandQueue(); frame = EngineFrameTexture(device: device)
            do {
                let library = try device.makeLibrary(source: Self.shader, options: nil)
                let descriptor = MTLRenderPipelineDescriptor()
                descriptor.vertexFunction = library.makeFunction(name: "window_vertex")
                descriptor.fragmentFunction = library.makeFunction(name: "frame_fragment")
                descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
                pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
                view.delegate = self
            } catch { view.isPaused = true }
        }
        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}
        func draw(in view: MTKView) {
            _ = frame?.refresh()
            guard let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
                  let queue, let command = queue.makeCommandBuffer() else { return }
            if let pipeline, let texture = frame?.texture, let encoder = command.makeRenderCommandEncoder(descriptor: pass) {
                encoder.setRenderPipelineState(pipeline); encoder.setFragmentTexture(texture, index: 0)
                var aspect = SIMD2<Float>(Float(texture.width) / Float(texture.height), Float(view.drawableSize.width / max(1, view.drawableSize.height)))
                encoder.setVertexBytes(&aspect, length: MemoryLayout<SIMD2<Float>>.stride, index: 0)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3); encoder.endEncoding()
            }
            command.present(drawable); command.commit()
        }

        private static let shader = """
        #include <metal_stdlib>
        using namespace metal;
        struct Raster { float4 position [[position]]; float2 uv; };
        vertex Raster window_vertex(uint id [[vertex_id]], constant float2 &aspect [[buffer(0)]]) {
            const float2 positions[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };
            const float2 uv[3] = { float2(0,1), float2(2,1), float2(0,-1) };
            float2 p = positions[id];
            float source = aspect.x, target = aspect.y;
            if (target > source) p.x *= source / target; else p.y *= target / source;
            return { float4(p, 0, 1), uv[id] };
        }
        fragment float4 frame_fragment(Raster in [[stage_in]], texture2d<float> frame [[texture(0)]]) {
            constexpr sampler sample(filter::linear, address::clamp_to_zero);
            return frame.sample(sample, in.uv);
        }
        """
    }
}
