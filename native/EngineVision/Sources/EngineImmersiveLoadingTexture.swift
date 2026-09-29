import Foundation
import CoreGraphics
import CoreText
import Metal

struct EngineLoadingState: Equatable, Sendable {
    let title: String
    let detail: String
    let progress: Double?
    let show: Bool
}

/// The loading card is rasterized only when the locked state changes. The
/// compositor samples this texture on the same curved mesh as engine frames.
final class EngineImmersiveLoadingTexture: @unchecked Sendable {
    private let device: MTLDevice
    private var lastState: EngineLoadingState?
    private var texture: MTLTexture?
    private let width = 1024
    private let height = 640

    init(device: MTLDevice) { self.device = device }

    func updateIfNeeded(_ state: EngineLoadingState) -> MTLTexture? {
        guard state != lastState else { return texture }
        lastState = state
        guard state.show else { texture = nil; return nil }
        // Drawn by CoreGraphics as sRGB bytes; sampled as sRGB so the
        // compositor shows the colours it was drawn in.
        texture = makeTexture(state).map { $0.makeTextureView(pixelFormat: .bgra8Unorm_srgb) ?? $0 }
        return texture
    }

    private func makeTexture(_ state: EngineLoadingState) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm,
                                                                    width: width, height: height,
                                                                    mipmapped: false)
        descriptor.usage = [.shaderRead]
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        texture.label = "HALO immersive loading status"

        var pixels = [UInt8](repeating: 0, count: width * height * 4)
        pixels.withUnsafeMutableBytes { storage in
            guard let base = storage.baseAddress,
                  let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
                  let context = CGContext(data: base, width: width, height: height,
                                          bitsPerComponent: 8, bytesPerRow: width * 4,
                                          space: colorSpace,
                                          bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue |
                                            CGBitmapInfo.byteOrder32Little.rawValue) else { return }
            context.setFillColor(CGColor(red: 0.004, green: 0.008, blue: 0.016, alpha: 1))
            context.fill(CGRect(x: 0, y: 0, width: width, height: height))

            // Core Text uses the flipped, top-left coordinate system below so
            // the status copy reads naturally on the curved screen.
            context.saveGState()
            context.translateBy(x: 0, y: CGFloat(height))
            context.scaleBy(x: 1, y: -1)
            drawText("HALO", in: CGRect(x: 96, y: 58, width: 832, height: 80),
                     fontSize: 58, color: CGColor(red: 0.08, green: 0.86, blue: 1, alpha: 1), context: context)
            drawText(state.title, in: CGRect(x: 96, y: 145, width: 832, height: 78),
                     fontSize: 40, color: CGColor(red: 0.95, green: 0.98, blue: 1, alpha: 1), context: context)
            drawText(state.detail.isEmpty ? "Preparing the original engine." : state.detail,
                     in: CGRect(x: 96, y: 245, width: 832, height: 120),
                     fontSize: 27, color: CGColor(red: 0.70, green: 0.78, blue: 0.88, alpha: 1), context: context)

            if let progress = state.progress {
                let value = min(1, max(0, progress))
                let bar = CGRect(x: 96, y: 438, width: 832, height: 20)
                context.setFillColor(CGColor(red: 0.08, green: 0.14, blue: 0.21, alpha: 1))
                context.fill(bar)
                context.setFillColor(CGColor(red: 0.08, green: 0.86, blue: 1, alpha: 1))
                context.fill(CGRect(x: bar.minX, y: bar.minY, width: bar.width * CGFloat(value), height: bar.height))
                drawText("\(Int((value * 100).rounded()))%", in: CGRect(x: 96, y: 480, width: 832, height: 46),
                         fontSize: 25, color: CGColor(red: 0.72, green: 0.92, blue: 1, alpha: 1), context: context)
            }
            context.restoreGState()
        }
        pixels.withUnsafeBytes { storage in
            texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                            withBytes: storage.baseAddress!, bytesPerRow: width * 4)
        }
        return texture
    }

    private func drawText(_ text: String, in rect: CGRect, fontSize: CGFloat, color: CGColor,
                          context: CGContext) {
        let font = CTFontCreateWithName("HelveticaNeue" as CFString, fontSize, nil)
        var alignment = CTTextAlignment.left
        var lineBreakMode = CTLineBreakMode.byWordWrapping
        let paragraph = withUnsafeBytes(of: &alignment) { alignmentBytes in
            withUnsafeBytes(of: &lineBreakMode) { lineBreakBytes in
                var settings = [
                    CTParagraphStyleSetting(spec: .alignment, valueSize: alignmentBytes.count, value: alignmentBytes.baseAddress!),
                    CTParagraphStyleSetting(spec: .lineBreakMode, valueSize: lineBreakBytes.count, value: lineBreakBytes.baseAddress!)
                ]
                return CTParagraphStyleCreate(&settings, settings.count)
            }
        }
        let attributes: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): font,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String): color,
            NSAttributedString.Key(kCTParagraphStyleAttributeName as String): paragraph
        ]
        let attributed = NSAttributedString(string: text, attributes: attributes)
        context.saveGState()
        // The bitmap context is already flipped to a top-left UI coordinate
        // system. Core Text lays out in y-up coordinates, so flip just this
        // text box back and draw it at a local origin.
        context.translateBy(x: rect.minX, y: rect.maxY)
        context.scaleBy(x: 1, y: -1)
        context.textMatrix = .identity
        let path = CGPath(rect: CGRect(x: 0, y: 0, width: rect.width, height: rect.height), transform: nil)
        let framesetter = CTFramesetterCreateWithAttributedString(attributed)
        let frame = CTFramesetterCreateFrame(framesetter, CFRange(location: 0, length: attributed.length), path, nil)
        CTFrameDraw(frame, context)
        context.restoreGState()
    }
}
