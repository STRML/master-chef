import Foundation
import simd

struct EngineImmersiveScreenVertex {
    var position: SIMD4<Float>
    var uv: SIMD2<Float>
    /// x is how opaque this vertex is. It is 1 everywhere except along an
    /// edge where one panorama panel dissolves into another. y is unused and
    /// keeps the stride aligned.
    var weight = SIMD2<Float>(1, 0)
}

enum EngineImmersiveScreenGeometry {
    /// How far away the whole 180-degree picture stands. Every panel sits on
    /// this one sphere, so no two of them can meet at a step in depth.
    static let panoramaRadius: Float = 4.25
    /// Half of a forward band's own share of the bearing, sixty degrees wide.
    /// The panels overlap well past this; it is only used to size the
    /// surround that fills the space behind the picture.
    static let panoramaHalfAngle: Float = Float.pi / 6
    /// The interface sits nearer than the picture so it cannot intersect it.
    private static let menuRadius: Float = 3.1
    /// How tall the panel appears, as a half angle. Driving it from an angle
    /// keeps its apparent size unchanged when its distance changes.
    private static let menuHalfAngle: Float = 19 * Float.pi / 180
    private static var menuPreferredHalfHeight: Float { menuRadius * tan(menuHalfAngle) }

    /// Fit a complete image with square pixels measured along the surface.
    /// Keep the surrounding immersive space black instead of distorting the
    /// source aspect ratio. This same readable scale is used by the front-end
    /// menu and the in-game pause/HUD surface.
    static func make(aspect: Float) -> (vertices: [EngineImmersiveScreenVertex], indices: [UInt32]) {
        make(aspect: aspect, preferredHalfHeight: menuPreferredHalfHeight)
    }

    /// A larger version of the same 4:3 surface for the native HUD/menu. The
    /// menu remains a single proportioned panel so text and health indicators
    /// remain readable in the headset. It intentionally matches make(aspect:)
    /// so front-end and pause menus have consistent sizing.
    static func hud(aspect: Float) -> (vertices: [EngineImmersiveScreenVertex], indices: [UInt32]) {
        make(aspect: aspect, preferredHalfHeight: menuPreferredHalfHeight)
    }

    /// The interface's shadow: the same panel at the same apparent size on a
    /// farther surface, a third of a degree down and to the right, so text,
    /// buttons and the reticle stand off it in depth for both eyes.
    private static let shadowRadius: Float = 3.7
    static func hudShadow(aspect: Float) -> (vertices: [EngineImmersiveScreenVertex], indices: [UInt32]) {
        let offset: Float = 0.33 * Float.pi / 180
        return make(aspect: aspect, preferredHalfHeight: shadowRadius * tan(menuHalfAngle),
                    radius: shadowRadius, thetaOffset: offset, yOffset: -offset * shadowRadius)
    }

    /// A full sphere that surrounds the viewer, carrying the texture
    /// coordinates the content surface would have if it continued past its
    /// edges. Coordinates outside 0...1 are deliberate: the fragment shader
    /// clamps them and fades out with distance, so the space around the
    /// picture holds a dim continuation of its border instead of hard black.
    /// `halfSweep` and `halfHeight` describe where the content sits on the
    /// cylinder of the given radius, so the glow lines up with its edges.
    static func backdrop(halfSweep: Float, halfHeight: Float, radius: Float = panoramaRadius)
        -> (vertices: [EngineImmersiveScreenVertex], indices: [UInt32]) {
        let columns = 72, rows = 36
        let sweep = halfSweep.isFinite && halfSweep > 0.01 ? halfSweep : Float.pi / 2
        let height = halfHeight.isFinite && halfHeight > 0.01 ? halfHeight : 1
        let shell = radius * 1.9   // behind every content surface
        var vertices: [EngineImmersiveScreenVertex] = [], indices: [UInt32] = []
        for row in 0...rows {
            // Stop just short of the poles: tan() is unbounded there and the
            // exact pole adds no coverage a viewer can distinguish.
            let phi = (Float(row) / Float(rows) - 0.5) * Float.pi * 0.995
            let cosPhi = cos(phi), sinPhi = sin(phi)
            let rise = min(64, max(-64, tan(phi))) * radius
            let v = 0.5 - rise / (2 * height)
            for column in 0...columns {
                let theta = (Float(column) / Float(columns) - 0.5) * 2 * Float.pi
                let u = 0.5 + theta / (2 * sweep)
                vertices.append(.init(position: SIMD4(shell * sin(theta) * cosPhi, shell * sinPhi,
                                                      -shell * cos(theta) * cosPhi, 1),
                                      uv: SIMD2(u, v)))
            }
        }
        for row in 0..<rows { for column in 0..<columns {
            let a = UInt32(row * (columns + 1) + column), b = a + UInt32(columns + 1)
            indices += [a, b, a+1, a+1, b, b+1]
        } }
        return (vertices, indices)
    }

    /// How tall the forward picture stands, so the surround behind it can be
    /// sized to match. The panels themselves are on a sphere; this is only
    /// used to place the backdrop that fills the space behind the viewer.
    static func panoramaHalfHeight(maxProjectionY: Float) -> Float {
        let y = maxProjectionY.isFinite && maxProjectionY > 0.0001 ? maxProjectionY : 1
        return panoramaRadius * cos(panoramaHalfAngle) / y
    }

    /// Where a line of sight from the viewer meets the interface cylinder,
    /// as the panel's texture coordinates (0,0 top left). Off the panel the
    /// coordinates still say which way the gaze went and `onPanel` is false.
    static func menuHit(origin p: SIMD3<Float>, direction d: SIMD3<Float>,
                        halfSweep: Float, halfHeight: Float) -> (u: Float, v: Float, onPanel: Bool)? {
        let r = menuRadius
        let a = d.x * d.x + d.z * d.z, b = 2 * (p.x * d.x + p.z * d.z), c = p.x * p.x + p.z * p.z - r * r
        guard a > 1e-6, halfSweep > 0.001, halfHeight > 0.001 else { return nil }
        let discriminant = b * b - 4 * a * c
        guard discriminant >= 0 else { return nil }
        // From inside the cylinder the far root is the one ahead of the viewer.
        let t = (-b + discriminant.squareRoot()) / (2 * a)
        guard t > 0 else { return nil }
        let q = p + t * d
        let theta = atan2(q.x, -q.z)
        let u = 0.5 + theta / (2 * halfSweep), v = 0.5 - q.y / (2 * halfHeight)
        guard u.isFinite, v.isFinite else { return nil }
        return (u, v, u >= 0 && u <= 1 && v >= 0 && v <= 1)
    }

    /// The menu panel's half sweep and half height, so the backdrop can match it.
    static func menuExtent(aspect: Float) -> (halfSweep: Float, halfHeight: Float) {
        let aspect = aspect.isFinite && aspect > 0 ? aspect : 4 / 3
        let sweep = min(Float.pi, 2 * menuPreferredHalfHeight * aspect / menuRadius)
        return (sweep / 2, menuRadius * sweep / (2 * aspect))
    }

    private static func make(aspect: Float, preferredHalfHeight: Float, radius: Float = menuRadius,
                             thetaOffset: Float = 0, yOffset: Float = 0)
        -> (vertices: [EngineImmersiveScreenVertex], indices: [UInt32]) {
        let aspect = aspect.isFinite && aspect > 0 ? aspect : 4 / 3
        let segments = 96
        let sweep = min(Float.pi, 2 * preferredHalfHeight * aspect / radius)
        let halfHeight = radius * sweep / (2 * aspect)
        var vertices: [EngineImmersiveScreenVertex] = [], indices: [UInt32] = []
        for n in 0...segments {
            let u = Float(n) / Float(segments), theta = (u - 0.5) * sweep + thetaOffset
            let x = radius * sin(theta), z = -radius * cos(theta)
            vertices.append(.init(position: SIMD4(x, halfHeight + yOffset, z, 1), uv: SIMD2(u, 0)))
            vertices.append(.init(position: SIMD4(x, -halfHeight + yOffset, z, 1), uv: SIMD2(u, 1)))
        }
        for n in 0..<segments {
            let top = UInt32(n * 2), bottom = top + 1, nextTop = top + 2, nextBottom = top + 3
            indices += [top, bottom, nextTop, nextTop, bottom, nextBottom]
        }
        return (vertices, indices)
    }

    // MARK: - One sphere for the whole 180-degree view

    /// Where one source camera points and how it projects.
    ///
    /// The engine renders five cameras from a single eye position: three
    /// bearings across the front and one each straight up and down. The
    /// orientations here mirror the schedule in
    /// `native/EngineHost/panorama_hooks.inc`; the hook pitches the whole
    /// basis rather than just the forward vector, so at ninety degrees up the
    /// camera's own up axis becomes the direction the head was facing.
    struct PanoramaSource {
        var yaw: Float
        var pitch: Float
        var projection: SIMD2<Float>
        var viewport: SIMD4<Float>
    }

    /// Where each camera points, matching the host's schedule: six bearings
    /// sixty degrees apart all the way round, then straight up and straight
    /// down. Given in the order the panels are indexed, which is the order
    /// the caller supplies projections in.
    static let panoramaAngles: [(yaw: Float, pitch: Float)] = [
        (-Float.pi / 3, 0), (0, 0), (Float.pi / 3, 0),           // forward three
        (2 * Float.pi / 3, 0), (Float.pi, 0), (-2 * Float.pi / 3, 0),  // the three behind
        (0, Float.pi / 2), (0, -Float.pi / 2),                   // sky and floor
    ]

    static func panoramaSources(projections: [SIMD2<Float>], viewports: [SIMD4<Float>])
        -> [PanoramaSource] {
        precondition(projections.count == panoramaAngles.count && viewports.count == panoramaAngles.count,
                     "one projection and one viewport per camera")
        return panoramaAngles.indices.map {
            PanoramaSource(yaw: panoramaAngles[$0].yaw, pitch: panoramaAngles[$0].pitch,
                           projection: projections[$0], viewport: viewports[$0])
        }
    }

    /// right, up and forward for a camera, exactly as the host builds it.
    private static func cameraBasis(_ s: PanoramaSource) -> (SIMD3<Float>, SIMD3<Float>, SIMD3<Float>) {
        let cy = cos(s.yaw), sy = sin(s.yaw)
        var forward = SIMD3<Float>(sy, 0, -cy)          // head forward is -Z
        var up = SIMD3<Float>(0, 1, 0)
        if s.pitch != 0 {
            let c = cos(s.pitch), p = sin(s.pitch)
            let f = forward * c + up * p
            up = up * c - forward * p                   // the basis turns as a whole
            forward = f
        }
        return (simd_normalize(simd_cross(forward, up)), up, forward)
    }

    /// Where a direction lands in a camera's image, in -1...1 on each axis.
    /// Returns nil behind the camera.
    private static func imagePoint(_ s: PanoramaSource, _ d: SIMD3<Float>) -> SIMD2<Float>? {
        let (right, up, forward) = cameraBasis(s)
        let f = simd_dot(forward, d)
        guard f > 1e-5 else { return nil }
        let px = s.projection.x > 0.0001 ? s.projection.x : 1
        let py = s.projection.y > 0.0001 ? s.projection.y : 1
        return SIMD2(px * simd_dot(right, d) / f, -py * simd_dot(up, d) / f)
    }

    /// How far into a panel its edge fade reaches, as a fraction of the half
    /// width. A fade only ever runs along an edge that lies over a panel
    /// already drawn, so the picture underneath is always complete there.
    ///
    /// 0.15 is the most this blend can carry, and it was measured, not
    /// guessed: 0.18 already leaves a 2 per cent dim band and 0.30 leaves 19.
    ///
    /// The limit is structural. Each panel gets one alpha, so its sideways
    /// fade and its vertical fade share a single measure of what already lies
    /// beneath. Where a band is fading into its neighbour sideways but is the
    /// only cover vertically, one number cannot be right for both, and a
    /// partial prior under-delivers: at prior 0.5 with a full fade the
    /// composite reaches 0.75, not 1.
    ///
    /// Widening the fade is worth doing, because the visible join is not a
    /// blend error but the two cameras shading the same direction
    /// differently, and only a longer cross-fade hides that. Doing it needs
    /// accumulating colour times weight and weight itself, then dividing at
    /// the end, which is exact for any fade width and any draw order. That is
    /// the next structural step here, not a constant to turn up.
    private static let panelFeatherAcross: Float = 0.15
    private static let panelFeatherUpDown: Float = 0.15

    /// Which edges of a panel dissolve into whatever was drawn before it.
    /// An edge facing a panel drawn later stays solid, because that panel
    /// fades itself in over this one.
    struct PanelFade {
        var left = false, right = false, bottom = false, top = false
        static let none = PanelFade()
        static let leftOnly = PanelFade(left: true)
        static let all = PanelFade(left: true, right: true, bottom: true, top: true)
    }

    /// The order the panels are drawn in, by source index. The ring goes down
    /// first, walking round so every band meets one already drawn and the
    /// last closes against the first. The sky and the floor go last and
    /// dissolve into it.
    ///
    /// Drawing the caps first was tried and is worse. A cap laid down first
    /// has nothing beneath it, so it has to stay solid right out to its own
    /// frame edge, and that edge is a step from fully opaque to nothing. Two
    /// of those steps cross the ring at the corners near ninety degrees of
    /// bearing, and left a three per cent dim seam there. Drawn last the cap
    /// edge is a fade instead, and near the poles, where the ring does not
    /// reach and nothing lies beneath, the cap stays solid on its own.
    static let panoramaPanelOrder = [0, 1, 2, 3, 4, 5, 6, 7]

    /// Grid divisions per panel edge. Each panel interpolates its own alpha
    /// across its own triangles and the eight do not share a tessellation, so
    /// too coarse a grid leaves a dim band where two of them hand over. 48
    /// divisions measured a 0.6 per cent dip; 64 measures none at all.
    /// Vertices are free here and dim seams are not.
    static var panoramaPanelDivisions =
        ProcessInfo.processInfo.environment["PANEL_DIVISIONS"].flatMap { Int($0) } ?? 64

    /// Which edges of each panel may dissolve, indexed by source. Permission
    /// is all this grants: `panelAlphas` only spends it where something has
    /// already been laid down underneath, so a band at the edge of what any
    /// camera covers stays solid.
    static let panoramaPanelFades: [PanelFade] = [
        .all, .all, .all, .all, .all, .all,   // the six bearings
        .all, .all,                            // sky and floor
    ]

    static func panelAlpha(_ s: Float, _ t: Float, _ edges: PanelFade) -> Float {
        func ramp(_ distance: Float, _ feather: Float) -> Float {
            let x = min(1, max(0, distance / feather))
            return x * x * (3 - 2 * x)
        }
        var a: Float = 1
        if edges.left { a *= ramp(s + 1, panelFeatherAcross) }
        if edges.right { a *= ramp(1 - s, panelFeatherAcross) }
        if edges.bottom { a *= ramp(t + 1, panelFeatherUpDown) }
        if edges.top { a *= ramp(1 - t, panelFeatherUpDown) }
        return a
    }

    /// Exposed for the coverage test: where a direction lands in a panel.
    static func imagePointForTest(_ s: PanoramaSource, _ d: SIMD3<Float>) -> SIMD2<Float>? { imagePoint(s, d) }
    /// Where a direction lands in a panel, for layer alignment
    /// (EngineLayerAlignment), which recomputes the same fades on the GPU.
    static func imagePoint(of s: PanoramaSource, _ d: SIMD3<Float>) -> SIMD2<Float>? { imagePoint(s, d) }
    /// right, up and forward of a panel's camera in the presenter's frame.
    static func basis(of s: PanoramaSource) -> (right: SIMD3<Float>, up: SIMD3<Float>, forward: SIMD3<Float>) {
        let (right, up, forward) = cameraBasis(s)
        return (right, up, forward)
    }

    /// How opaque each panel is at one direction, in draw order.
    ///
    /// A panel may only dissolve where something already lies beneath it. Out
    /// at the sides of the hemisphere a band is the only thing covering a
    /// direction, and fading there would leave a hole, so the edge fade is
    /// applied in proportion to what has already been laid down: full fade
    /// over solid ground, no fade at all over nothing.
    static func panelAlphas(sources: [PanoramaSource], direction: SIMD3<Float>) -> [Float] {
        var laid: Float = 0
        var result = [Float](repeating: 0, count: panoramaPanelOrder.count)
        for (position, index) in panoramaPanelOrder.enumerated() {
            guard index < sources.count else { continue }
            var alpha: Float = 0
            if let p = imagePoint(sources[index], direction), abs(p.x) <= 1, abs(p.y) <= 1 {
                let edge = panelAlpha(p.x, p.y, panoramaPanelFades[index])
                alpha = 1 - (1 - edge) * laid
            }
            result[position] = alpha
            laid += alpha * (1 - laid)
        }
        return result
    }

    /// Exposed for the coverage test: where a direction lands in a panel and
    /// how opaque that panel is there.
    static func panelCoverage(_ s: PanoramaSource, _ d: SIMD3<Float>, _ edges: PanelFade)
        -> (inside: Bool, alpha: Float)? {
        guard let p = imagePoint(s, d) else { return nil }
        let inside = abs(p.x) <= 1 && abs(p.y) <= 1
        return (inside, inside ? panelAlpha(p.x, p.y, edges) : 0)
    }

    /// One panel per source camera, all mapped onto a single sphere so there
    /// is no step where two of them meet, each carrying its edge fade in the
    /// vertex alpha. Drawn in order with ordinary over blending, every panel
    /// dissolves into the one beneath it. That blend is its own normaliser:
    /// whatever the alphas come out as the result stays a proper mix of the
    /// pictures, so a coarse mesh cannot leave a bright or dark band at a
    /// join the way a hand-normalised weight can.
    static func sphericalPanorama(sources: [PanoramaSource], texelSize: SIMD2<Float>)
        -> [(vertices: [EngineImmersiveScreenVertex], indices: [UInt32])] {
        // Dense enough that interpolating each panel's alpha across its own
        // triangles cannot leave a visible dip where two of them hand over.
        let columns = panoramaPanelDivisions, rows = panoramaPanelDivisions, radius = panoramaRadius
        return sources.indices.map { index in
            let source = sources[index]
            let orderPosition = panoramaPanelOrder.firstIndex(of: index) ?? 0
            let px = source.projection.x > 0.0001 ? source.projection.x : 1
            let py = source.projection.y > 0.0001 ? source.projection.y : 1
            let (right, up, forward) = cameraBasis(source)
            let uvMin = SIMD2(source.viewport.x, source.viewport.y) + texelSize / 2
            let uvMax = SIMD2(source.viewport.z, source.viewport.w) - texelSize / 2
            var vertices: [EngineImmersiveScreenVertex] = []
            vertices.reserveCapacity((rows + 1) * (columns + 1))
            for row in 0...rows {
                for column in 0...columns {
                    let s = Float(column) / Float(columns) * 2 - 1
                    let t = Float(row) / Float(rows) * 2 - 1
                    let ray = right * (s / px) - up * (t / py) + forward
                    let d = simd_normalize(ray)
                    let alpha = panelAlphas(sources: sources, direction: d)[orderPosition]
                    let u = min(uvMax.x, max(uvMin.x, (source.viewport.x + source.viewport.z) / 2 + s / 2 * (source.viewport.z - source.viewport.x)))
                    let v = min(uvMax.y, max(uvMin.y, (source.viewport.y + source.viewport.w) / 2 + t / 2 * (source.viewport.w - source.viewport.y)))
                    vertices.append(.init(position: SIMD4(d.x * radius, d.y * radius, d.z * radius, 1),
                                          uv: SIMD2(u, v), weight: SIMD2(alpha, 0)))
                }
            }
            var indices: [UInt32] = []
            indices.reserveCapacity(rows * columns * 6)
            for row in 0..<rows { for column in 0..<columns {
                let a = UInt32(row * (columns + 1) + column), b = a + UInt32(columns + 1)
                indices += [a, b, a + 1, a + 1, b, b + 1]
            } }
            return (vertices, indices)
        }
    }
}
