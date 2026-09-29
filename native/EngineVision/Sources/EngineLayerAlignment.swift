import Foundation
import Metal
import simd

/// The engine camera a panorama layer was drawn with, as the host exports it
/// (panorama.h, layer_pose): position in world units, then forward and up, in
/// Halo's world (z up), before the pass turned it to the layer's bearing.
struct EngineLayerPose: Equatable {
    var position: SIMD3<Float>
    var forward: SIMD3<Float>
    var up: SIMD3<Float>

    init(position: SIMD3<Float>, forward: SIMD3<Float>, up: SIMD3<Float>) {
        self.position = position; self.forward = forward; self.up = up
    }

    /// Nine floats in the bridge's order; nil for anything shorter or all zero
    /// (the host writes zeros for a camera it did not know).
    init?(floats: [Float]) {
        guard floats.count >= 9, floats.contains(where: { $0 != 0 }) else { return nil }
        position = SIMD3(floats[0], floats[1], floats[2])
        forward = SIMD3(floats[3], floats[4], floats[5])
        up = SIMD3(floats[6], floats[7], floats[8])
    }

    /// World directions to this camera's presenter frame: x its right, y its
    /// up, z behind it, the frame every panel of the sphere is laid out in.
    /// Halo's right is forward x up (panorama_hooks.inc builds the bearings
    /// from that same cross product). Up is made exactly perpendicular to
    /// forward first, so the result is a proper rotation. nil when the
    /// vectors are unusable.
    var presenterFromWorld: simd_float3x3? {
        let values = [position.x, position.y, position.z, forward.x, forward.y, forward.z, up.x, up.y, up.z]
        guard values.allSatisfy({ $0.isFinite }), simd_length(forward) > 1e-4, simd_length(up) > 1e-4 else { return nil }
        let f = simd_normalize(forward)
        let level = up - simd_dot(up, f) * f
        guard simd_length(level) > 1e-4 else { return nil }
        let u = simd_normalize(level)
        let r = simd_cross(f, u)
        return simd_float3x3(rows: [r, u, -f])
    }
}

/// Lines up panorama layers drawn in different engine frames.
///
/// The ring and the caps are not all redrawn every frame: the budget redraws
/// the centre every frame and the rest in rotation, so a side band is often a
/// frame or two old and the sky, the floor and the bearings behind up to a
/// second or more. The stick turns the camera up to 2.3 degrees every frame,
/// so while the player turns, neighbouring pictures meet at a join with the
/// world doubled across it: the ghosted edges at the panel joins.
///
/// Each layer carries the camera it was drawn with. Turning the layer by the
/// rotation from that camera to the newest centre camera puts every distant
/// thing it shows where the newest camera sees it, which is exact for static
/// geometry far away under a pure rotation, and removes the rotation part of
/// the error for everything else. The layer is turned rigidly in the vertex
/// stage; its edge fades are recomputed per fragment on the GPU against where
/// its neighbours actually are, so no mesh is rebuilt on the CPU.
///
/// Three things limit it:
///  * Turning one layer against a neighbour opens its far join. Every join is
///    kept inside its overlap (ring bands overlap by 4.5 degrees, bands and
///    caps by 6.4) and every panel edge is checked to lie inside another
///    panel before a plan is used; if any does not, smaller turns are tried.
///    The caps may twist about their own axis freely: that cannot uncover
///    anything, so the sky and floor are aligned in full.
///  * The first-person weapon is world geometry in the two side bands
///    (HALO_PANORAMA_WORLD_FP): it is attached to the camera, so it is already
///    where it belongs and turning it would tear it from the centre's copy.
///    The side bands keep their lower inner region, where the weapon can
///    reach, as drawn, and bend smoothly into the turn above and outside it.
///  * Layers from before a camera cut (host cut_epoch) show another shot and
///    are left alone.
///
/// Off by default (HaloSettings.layer_align: HALO_LAYER_ALIGN=1 or the
/// settings toggle). It has only been checked on synthetic pictures: pure
/// rotation, no parallax, no real weapon. While the stick turns it also has
/// side effects. A b30 stick-turn A/B on the headset has to show that the
/// joins it fixes outweigh them:
///  * The weapon zone's blend (-4 to +8 degrees of elevation, 56 to 68 of
///    azimuth) shears the side bands at eye level. Verticals tilt up to 16
///    degrees at a 2.3 degree turn and 27 at the 4 degree limit. Below the
///    blend, content at 56-68 degrees is squashed to 0.49 across. A 7 degree
///    pitch correction squashes the blend band to 0.23 of its height. Each
///    of these comes and goes as the side bands are redrawn.
///  * A band turned away from its neighbour narrows the cross-fade at that
///    join, from about 3.5 degrees to 0.5 at the 4 degree limit. Parallax,
///    animation and lighting differences then show as a harder edge.
///  * The zone's bounds are estimates. They have not been checked against
///    real weapon renders or third-person vehicles, and the zone applies
///    even when no weapon is drawn.
/// With it off, the panels are drawn as baked and each layer's camera
/// offset is still recorded (EngineLayerAlignmentState.measure) as the A/B
/// baseline.
enum EngineLayerAlignment {
    /// Presenter panel (EngineImmersiveScreenGeometry source index) to the
    /// engine layer it shows.
    static let panelLayers = [0, 1, 2, 7, 8, 9, 5, 6]
    static let panelCount = 8
    static let centrePanel = 1
    static let ringPanels = [0, 1, 2, 3, 4, 5]
    static let skyPanel = 6, floorPanel = 7
    /// The side bands at -60 and +60, the only ones besides the centre the
    /// weapon is drawn into (panorama_hooks.inc, the weapon's reach).
    static let weaponPanels: Set<Int> = [0, 2]
    /// For each panel, the panels drawn before it that it can overlap
    /// (panoramaPanelOrder is 0...7): its fade is measured against these.
    /// The cover taken is the largest of theirs, so the order only decides
    /// how soon the search can stop; rigid panels come first because the
    /// side bands need a short solve.
    static let beneath: [[Int]] = [[], [0], [1], [2], [3], [4, 0], [1, 3, 4, 5, 0, 2], [1, 3, 4, 5, 0, 2]]
    /// A side band turns a few degrees at most, so a direction this far
    /// outside it unturned (in image units) is outside it turned as well.
    static let sideBandReject: Float = -0.25
    /// How far inside an earlier panel's edge its cover takes to reach full,
    /// in image units. Small enough that with nothing turned the fades are the
    /// ones EngineImmersiveScreenGeometry.panelAlphas bakes (a band's fade
    /// ends 0.017 inside its neighbour), large enough that a neighbour's edge
    /// crossing a turned fade leaves a ramp rather than a line.
    static let laidSoftness: Float = 0.015
    /// Below this nothing is turned: the pictures came from the same camera.
    static let identityRadians: Float = 0.02 * .pi / 180
    /// The stick turns 2.3 degrees a frame at most; a rotation faster than
    /// this per frame of age is a cut the host did not see.
    static let maximumRadiansPerFrame: Float = 12 * .pi / 180
    /// A ring band further round than this from the newest camera follows
    /// its neighbour nearer the centre rather than its own camera.
    static let farRadians: Float = .pi / 2

    /// Where the weapon can be in a side band, as sines of the direction's
    /// angle from straight ahead and of its elevation: nothing is turned
    /// below -4 degrees of elevation within 56 degrees of straight ahead, all
    /// of the turn applies above +8 degrees or beyond 68, and it blends
    /// smoothly between. The weapon and hands sit low and inside that; the
    /// bands start at 27.75 degrees.
    static let weaponZone = SIMD4<Float>(sin(56 * .pi / 180), sin(68 * .pi / 180),
                                         sin(-4 * .pi / 180), sin(8 * .pi / 180))

    /// Limits on how far one panel may turn against its neighbours, in
    /// radians. A ring join opens by the yaw part plus 1.12 times the part
    /// about the join's own direction (tan 48.1 degrees, the band's height at
    /// its edge); the bands overlap by 4.5 degrees. A cap's axis may tilt
    /// against a band's up by less than their 6.4 degrees of overlap.
    struct Limits: Equatable {
        var ringJoin: Float
        var capTilt: Float
        static let full = Limits(ringJoin: 4.0 * .pi / 180, capTilt: 4.5 * .pi / 180)
        static let half = Limits(ringJoin: 2.0 * .pi / 180, capTilt: 2.25 * .pi / 180)
        /// The ring as drawn and the caps twisted about their own axis only.
        static let capsTwistOnly = Limits(ringJoin: 0, capTilt: 0)
    }

    enum Level: Int { case full = 0, half = 1, capsTwistOnly = 2, none = 3 }

    struct Plan {
        /// The turn applied to each panel (source index order).
        var rotations: [simd_quatf]
        /// How far each panel's camera is from the newest centre's, and how
        /// far it was turned, in degrees.
        var desiredDegrees: [Float]
        var appliedDegrees: [Float]
        var level: Level
        /// Panels left as drawn because their layer predates a camera cut.
        var cutPanels: Int
        static func identity(desired: [Float] = Array(repeating: 0, count: panelCount), cut: Int = 0) -> Plan {
            Plan(rotations: Array(repeating: identityQuaternion, count: panelCount), desiredDegrees: desired,
                 appliedDegrees: Array(repeating: 0, count: panelCount), level: .none, cutPanels: cut)
        }
    }

    static let identityQuaternion = simd_quatf(ix: 0, iy: 0, iz: 0, r: 1)
    private static let yAxis = SIMD3<Float>(0, 1, 0)

    // MARK: - Rotations

    /// The same rotation with a non-negative real part: the short way round.
    static func shortest(_ q: simd_quatf) -> simd_quatf {
        let n = simd_normalize(q.vector)
        return simd_quatf(vector: n.w < 0 ? -n : n)
    }
    static func radians(_ q: simd_quatf) -> Float {
        let s = shortest(q)
        return 2 * atan2(simd_length(s.imag), s.real)
    }
    /// Axis and angle as the shaders take them; the identity is (0,1,0,0).
    static func axisAngle(_ q: simd_quatf) -> SIMD4<Float> {
        let s = shortest(q), length = simd_length(s.imag)
        guard length > 1e-9 else { return SIMD4(0, 1, 0, 0) }
        return SIMD4(s.imag / length, 2 * atan2(length, s.real))
    }
    /// The same axis, the angle scaled.
    static func scaled(_ q: simd_quatf, _ factor: Float) -> simd_quatf {
        let a = axisAngle(q)
        guard a.w > 0, factor != 1 else { return factor == 1 ? q : identityQuaternion }
        return simd_quatf(angle: a.w * factor, axis: SIMD3(a.x, a.y, a.z))
    }
    /// Rotation vector (axis times angle) and back.
    private static func log(_ q: simd_quatf) -> SIMD3<Float> { let a = axisAngle(q); return SIMD3(a.x, a.y, a.z) * a.w }
    private static func exp(_ w: SIMD3<Float>) -> simd_quatf {
        let angle = simd_length(w)
        return angle > 1e-9 ? simd_quatf(angle: angle, axis: w / angle) : identityQuaternion
    }

    // MARK: - The weapon zone

    static func smoothstep(_ e0: Float, _ e1: Float, _ x: Float) -> Float {
        let t = min(1, max(0, (x - e0) / (e1 - e0)))
        return t * t * (3 - 2 * t)
    }
    /// How much of a side band's turn applies at presenter direction d: 0
    /// where the weapon can be, 1 clear of it. Mirrors align_weight.
    static func correctionWeight(_ direction: SIMD3<Float>) -> Float {
        let d = simd_normalize(direction)
        let horizontal = (d.x * d.x + d.z * d.z).squareRoot()
        let side = horizontal > 1e-6 ? abs(d.x) / horizontal : 0
        let across = d.z > 0 ? 1 : smoothstep(weaponZone.x, weaponZone.y, side)
        let up = smoothstep(weaponZone.z, weaponZone.w, d.y)
        return 1 - (1 - across) * (1 - up)
    }
    /// One panel's turn as the shaders apply it: an axis, an angle, and
    /// whether the weapon zone scales it (align_rotate, align_weight).
    struct Turn {
        var axis: SIMD3<Float>
        var angle: Float
        var zoned: Bool
        init(_ q: simd_quatf, panel: Int) {
            let a = axisAngle(q)
            axis = SIMD3(a.x, a.y, a.z); angle = a.w; zoned = weaponPanels.contains(panel)
        }
        /// Rodrigues, exactly as align_rotate: v itself when the angle is 0.
        func rotate(_ v: SIMD3<Float>, _ scale: Float) -> SIMD3<Float> {
            let a = angle * scale, c = cos(a), s = sin(a)
            return v * c + simd_cross(axis, v) * s + axis * simd_dot(axis, v) * (1 - c)
        }
        /// Where the panel puts its own base direction d.
        func place(_ d: SIMD3<Float>) -> SIMD3<Float> { rotate(d, zoned ? correctionWeight(d) : 1) }
        /// Which base direction lands at presenter direction p. Exact for a
        /// rigid panel. A side band is turned by weight w of its angle, where
        /// w depends on the base direction, so the w that maps p back to a
        /// direction of that same weight is solved for: h(w) = weight(d(w)) - w
        /// falls from h(0) >= 0 to h(1) <= 0 and regula falsi (Illinois) pins
        /// it down in a few steps. Mirrors align_base.
        func base(_ p: SIMD3<Float>) -> SIMD3<Float> {
            guard zoned, angle > 0 else { return rotate(p, -1) }
            var a: Float = 0, fa = correctionWeight(p)
            var b: Float = 1, fb = correctionWeight(rotate(p, -1)) - 1
            if fa <= 0 { return p }
            if fb >= 0 { return rotate(p, -1) }
            for _ in 0..<weightSteps {
                let c = b - fb * (b - a) / (fb - fa)
                let fc = correctionWeight(rotate(p, -c)) - c
                if fc * fb < 0 { a = b; fa = fb } else { fa *= 0.5 }
                b = c; fb = fc
            }
            return rotate(p, -b)
        }
    }
    static let weightSteps = 5
    /// The turn a panel applies at its own base direction d.
    static func effective(_ q: simd_quatf, panel: Int, at d: SIMD3<Float>) -> simd_quatf {
        weaponPanels.contains(panel) ? scaled(q, correctionWeight(d)) : q
    }
    static func base(of p: SIMD3<Float>, panel: Int, rotation q: simd_quatf) -> SIMD3<Float> {
        Turn(q, panel: panel).base(p)
    }

    // MARK: - What each layer needs

    /// The rotation that takes each panel's pictures from the camera its
    /// layer was drawn with to the newest centre camera, in the presenter's
    /// frame: a pixel that was straight ahead of camera k is shown where
    /// camera 1 sees the same world direction. Identity for the centre, for
    /// any layer without a usable camera, and for layers from before a cut.
    static func desired(poses: [EngineLayerPose?], epochs: [UInt64], cutEpoch: UInt64)
        -> (rotations: [simd_quatf], cut: Int) {
        var rotations = Array(repeating: identityQuaternion, count: panelCount)
        guard poses.count > 9, epochs.count > 9, let centre = poses[1]?.presenterFromWorld else { return (rotations, 0) }
        let newest = epochs[1]
        var cut = 0
        for panel in 0..<panelCount where panel != centrePanel {
            let layer = panelLayers[panel]
            guard epochs[layer] > 0, epochs[layer] <= newest else { continue }
            if cutEpoch > 0 && epochs[layer] < cutEpoch { cut += 1; continue }
            guard let camera = poses[layer]?.presenterFromWorld else { continue }
            let q = shortest(simd_quatf(centre * camera.transpose))
            let angle = radians(q)
            guard angle.isFinite, angle > identityRadians else { continue }
            if angle > Float(max(1, newest - epochs[layer])) * maximumRadiansPerFrame { cut += 1; continue }
            rotations[panel] = q
        }
        return (rotations, cut)
    }

    /// The turns actually applied: as close to `desired` as the joins allow,
    /// with every panel edge still inside another panel.
    static func plan(sources: [EngineImmersiveScreenGeometry.PanoramaSource], desired: [simd_quatf], cut: Int = 0) -> Plan {
        precondition(sources.count == panelCount && desired.count == panelCount, "one source and one turn per panel")
        let desiredDegrees = desired.map { radians($0) * 180 / .pi }
        guard desired.contains(where: { radians($0) > identityRadians }) else {
            return .identity(desired: desiredDegrees, cut: cut)
        }
        for (level, limits) in [(Level.full, Limits.full), (.half, .half), (.capsTwistOnly, .capsTwistOnly)] {
            let rotations = clamp(desired, limits)
            if coverageHolds(sources: sources, rotations: rotations) {
                return Plan(rotations: rotations, desiredDegrees: desiredDegrees,
                            appliedDegrees: rotations.map { radians($0) * 180 / .pi }, level: level, cutPanels: cut)
            }
        }
        return .identity(desired: desiredDegrees, cut: cut)
    }

    /// Each ring panel is turned as far towards its own rotation as its join
    /// with the panel nearer the centre allows, walking outwards from the
    /// centre, so the joins nearest the gaze are the ones fully aligned. The
    /// caps keep their full twist and tilt only as far as every band allows.
    static func clamp(_ desired: [simd_quatf], _ limits: Limits) -> [simd_quatf] {
        var c = Array(repeating: identityQuaternion, count: panelCount)
        func join(_ degrees: Float) -> SIMD3<Float> {
            let a = degrees * .pi / 180
            return SIMD3(sin(a), 0, -cos(a))
        }
        // A band whose camera is more than a quarter turn from the newest
        // shows nothing the joins could let it reach; it follows the band
        // nearer the centre instead of pulling towards an axis that means
        // little so far round.
        func target(_ panel: Int, _ reference: simd_quatf) -> simd_quatf {
            radians(desired[panel]) > farRadians ? reference : desired[panel]
        }
        c[0] = clampJoin(target(0, c[1]), near: c[1], join: join(-30), limit: limits.ringJoin)
        c[2] = clampJoin(target(2, c[1]), near: c[1], join: join(30), limit: limits.ringJoin)
        c[3] = clampJoin(target(3, c[2]), near: c[2], join: join(90), limit: limits.ringJoin)
        c[5] = clampJoin(target(5, c[0]), near: c[0], join: join(-90), limit: limits.ringJoin)
        var rear = target(4, c[3])
        for _ in 0..<4 {
            rear = clampJoin(rear, near: c[3], join: join(150), limit: limits.ringJoin)
            rear = clampJoin(rear, near: c[5], join: join(-150), limit: limits.ringJoin)
        }
        c[4] = rear
        let ups = ringPanels.map { c[$0].act(yAxis) } + [yAxis]
        c[skyPanel] = clampCap(desired[skyPanel], ups: ups, limit: limits.capTilt)
        c[floorPanel] = clampCap(desired[floorPanel], ups: ups, limit: limits.capTilt)
        return c
    }

    /// How far a turn `relative` (panel against its neighbour, in the
    /// neighbour's frame) opens or closes their join at azimuth `join`.
    static func joinMeasure(_ relative: simd_quatf, join: SIMD3<Float>) -> Float {
        let w = log(relative)
        return abs(w.y) + 1.12 * abs(simd_dot(w, join))
    }
    static func clampJoin(_ target: simd_quatf, near reference: simd_quatf, join: SIMD3<Float>, limit: Float) -> simd_quatf {
        guard limit > 0 else { return reference }
        let relative = shortest(reference.inverse * target)
        let measure = joinMeasure(relative, join: join)
        guard measure > limit else { return target }
        return shortest(reference * exp(log(relative) * (limit / measure)))
    }
    /// A cap keeps its twist about the vertical and its axis is moved into
    /// every band's cone of allowed tilt.
    static func clampCap(_ target: simd_quatf, ups: [SIMD3<Float>], limit: Float) -> simd_quatf {
        let q = shortest(target)
        let twistVector = SIMD4<Float>(0, q.imag.y, 0, q.real)
        let twist = simd_length(twistVector) > 1e-6 ? simd_quatf(vector: simd_normalize(twistVector)) : identityQuaternion
        var axis = q.act(yAxis)
        for _ in 0..<8 {
            for up in ups {
                let angle = acos(min(1, max(-1, simd_dot(axis, up))))
                guard angle > limit else { continue }
                let pivot = simd_cross(up, axis)
                axis = simd_length(pivot) > 1e-7
                    ? simd_quatf(angle: limit, axis: simd_normalize(pivot)).act(up) : up
            }
        }
        return shortest(simd_quatf(from: yAxis, to: simd_normalize(axis)) * twist)
    }

    // MARK: - Coverage

    /// The panels that can cover each panel's edge: a band's neighbours in
    /// the ring and both caps, and for a cap the six bands.
    static let coverers: [[Int]] = [[1, 5, 6, 7], [0, 2, 6, 7], [1, 3, 6, 7], [2, 4, 6, 7], [3, 5, 6, 7], [4, 0, 6, 7],
                                    [0, 1, 2, 3, 4, 5], [0, 1, 2, 3, 4, 5]]

    /// True when every point along every panel's edge, turned, lies inside
    /// some other turned panel. A hole in the sphere would be bounded by
    /// panel edges that nothing else covers, so this rules holes out.
    static func coverageHolds(sources: [EngineImmersiveScreenGeometry.PanoramaSource], rotations: [simd_quatf],
                              samples: Int = 24, margin: Float = 0.01) -> Bool {
        let turns = (0..<panelCount).map { Turn(rotations[$0], panel: $0) }
        for k in 0..<panelCount {
            let source = sources[k], (right, up, forward) = EngineImmersiveScreenGeometry.basis(of: source)
            let px = source.projection.x > 0.0001 ? source.projection.x : 1
            let py = source.projection.y > 0.0001 ? source.projection.y : 1
            var last = coverers[k][0]
            for edge in 0..<4 {
                for n in 0...samples {
                    let u = Float(n) / Float(samples) * 2 - 1
                    let (s, t): (Float, Float) = edge == 0 ? (-1, u) : edge == 1 ? (1, u) : edge == 2 ? (u, -1) : (u, 1)
                    let p = turns[k].place(simd_normalize(right * (s / px) - up * (t / py) + forward))
                    // The panel that covered the previous sample usually covers this one.
                    func covers(_ j: Int) -> Bool {
                        guard let q = EngineImmersiveScreenGeometry.imagePoint(of: sources[j], turns[j].base(p)) else { return false }
                        return abs(q.x) <= 1 - margin && abs(q.y) <= 1 - margin
                    }
                    if covers(last) { continue }
                    guard let found = coverers[k].first(where: { $0 != last && covers($0) }) else { return false }
                    last = found
                }
            }
        }
        return true
    }

    /// How opaque panel k is at presenter direction p: the CPU twin of
    /// align_alpha in the shader. Its own edge fade applies only where a
    /// panel drawn before it lies beneath, softened within laidSoftness of
    /// that panel's edge; elsewhere the panel is solid.
    static func alpha(panel k: Int, at p: SIMD3<Float>, sources: [EngineImmersiveScreenGeometry.PanoramaSource],
                      rotations: [simd_quatf]) -> Float {
        let d = Turn(rotations[k], panel: k).base(p)
        guard let st = EngineImmersiveScreenGeometry.imagePoint(of: sources[k], d), abs(st.x) <= 1, abs(st.y) <= 1 else { return 0 }
        let edge = EngineImmersiveScreenGeometry.panelAlpha(st.x, st.y, EngineImmersiveScreenGeometry.panoramaPanelFades[k])
        if edge >= 1 { return 1 }
        var laid: Float = 0
        for j in beneath[k] where laid < 1 {
            if weaponPanels.contains(j) {
                guard let rough = EngineImmersiveScreenGeometry.imagePoint(of: sources[j], p),
                      min(1 - abs(rough.x), 1 - abs(rough.y)) >= sideBandReject else { continue }
            }
            guard let q = EngineImmersiveScreenGeometry.imagePoint(of: sources[j], Turn(rotations[j], panel: j).base(p)) else { continue }
            let inside = min(1 - abs(q.x), 1 - abs(q.y))
            if inside > 0 { laid = max(laid, smoothstep(0, laidSoftness, inside)) }
        }
        return 1 - (1 - edge) * laid
    }

    // MARK: - Shader constants

    /// What one panel's draw binds: three float4 for the vertex stage
    /// (buffer 3) and thirty for the fragment stage (buffer 2). Laid out as
    /// arrays of float4 so Swift and Metal cannot disagree on packing.
    struct PanelConstants {
        var vertex: [SIMD4<Float>]
        var fragment: [SIMD4<Float>]
    }
    static let vertexConstantCount = 3
    static let fragmentConstantCount = 6 + 4 * 6

    static func constants(sources: [EngineImmersiveScreenGeometry.PanoramaSource], rotations: [simd_quatf]) -> [PanelConstants] {
        /// A camera as the fragment stage reads it. `turned` bakes a rigid
        /// panel's turn into its axes (the image of a presenter direction is
        /// then three dot products); a side band keeps its own axes and its
        /// turn, and is solved for (align_base).
        func camera(_ panel: Int, turned: Bool) -> [SIMD4<Float>] {
            let source = sources[panel], (right, up, forward) = EngineImmersiveScreenGeometry.basis(of: source)
            let px = source.projection.x > 0.0001 ? source.projection.x : 1
            let py = source.projection.y > 0.0001 ? source.projection.y : 1
            let zoned = weaponPanels.contains(panel)
            let q = turned && !zoned ? rotations[panel] : identityQuaternion
            return [SIMD4(q.act(right), px), SIMD4(q.act(up), py), SIMD4(q.act(forward), zoned ? 1 : 0),
                    axisAngle(rotations[panel])]
        }
        return (0..<panelCount).map { k in
            let zoned: Float = weaponPanels.contains(k) ? 1 : 0
            let vertex = [axisAngle(rotations[k]), weaponZone, SIMD4<Float>(zoned, 0, 0, 0)]
            var fragment = camera(k, turned: false) + [weaponZone, SIMD4<Float>(Float(beneath[k].count), laidSoftness, sideBandReject, 0)]
            for j in beneath[k] { fragment += camera(j, turned: true) }
            fragment += Array(repeating: SIMD4<Float>(0, 0, 0, 0), count: fragmentConstantCount - fragment.count)
            return PanelConstants(vertex: vertex, fragment: fragment)
        }
    }

    /// The aligned panel pipeline. Appended to the renderer's own shader
    /// source, whose Vertex struct and cubic_sample it uses; compiled as a
    /// second library so a failure here can only switch alignment off.
    static let shaderSource = """

    /* Layer alignment (EngineLayerAlignment.swift). Each panel is turned in
     * the sphere's frame by its layer's rotation to the newest centre camera,
     * and its edge fade is measured per fragment against where the panels
     * beneath it now are. With nothing turned the result is the baked
     * panorama's. */
    struct AlignedRaster { float4 position [[position]]; float2 uv; uint eye [[flat]]; float3 base; float3 placed; };
    static inline float3 align_rotate(float3 v, float4 axisAngle, float scale) {
        float angle = axisAngle.w * scale;
        float c = cos(angle), s = sin(angle);
        float3 k = axisAngle.xyz;
        return v * c + cross(k, v) * s + k * dot(k, v) * (1.0 - c);
    }
    /* How much of a side band's turn applies at d: none where the weapon can
     * be (low and near the front), all clear of it. */
    static inline float align_weight(float3 d, float4 zone) {
        d = normalize(d);
        float horizontal = sqrt(d.x * d.x + d.z * d.z);
        float side = horizontal > 1e-6 ? abs(d.x) / horizontal : 0.0;
        float across = d.z > 0.0 ? 1.0 : smoothstep(zone.x, zone.y, side);
        float up = smoothstep(zone.z, zone.w, d.y);
        return 1.0 - (1.0 - across) * (1.0 - up);
    }
    /* panel: [0] axis and angle, [1] weapon zone, [2].x 1 for a side band. */
    vertex AlignedRaster aligned_vertex(uint id [[vertex_id]], uint eye [[amplification_id]],
        const device Vertex *vertices [[buffer(0)]], constant float4x4 *matrices [[buffer(1)]],
        constant uint &eyeBase [[buffer(2)]], constant float4 *panel [[buffer(3)]]) {
        float3 base = vertices[id].position.xyz;
        float scale = panel[2].x > 0.5 ? align_weight(base, panel[1]) : 1.0;
        float3 placed = align_rotate(base, panel[0], scale);
        AlignedRaster out;
        out.position = matrices[eye] * float4(placed, 1.0);
        out.uv = vertices[id].uv; out.eye = eyeBase + eye; out.base = base; out.placed = placed;
        return out;
    }
    /* A camera is four float4: right (w projection x), up (w projection y),
     * forward (w 1 for a side band), then its turn as axis and angle. A rigid
     * panel beneath arrives with its turn already applied to its axes. */
    static inline bool align_image(float3 d, constant float4 *camera, thread float2 &st) {
        float f = dot(camera[2].xyz, d);
        if (f <= 1e-5) return false;
        st = float2(camera[0].w * dot(camera[0].xyz, d) / f, -camera[1].w * dot(camera[1].xyz, d) / f);
        return true;
    }
    /* The base direction of a camera that lands at `placed`. A side band is
     * turned by a weight that depends on the base direction; solve for the
     * weight that is its own (regula falsi, as EngineLayerAlignment.base). */
    static inline float3 align_base(float3 placed, constant float4 *camera, float4 zone) {
        float a = 0.0, fa = align_weight(placed, zone);
        float b = 1.0, fb = align_weight(align_rotate(placed, camera[3], -1.0), zone) - 1.0;
        if (fa <= 0.0) return placed;
        if (fb >= 0.0) return align_rotate(placed, camera[3], -1.0);
        for (int i = 0; i < 5; ++i) {
            float c = b - fb * (b - a) / (fb - fa);
            float fc = align_weight(align_rotate(placed, camera[3], -c), zone) - c;
            if (fc * fb < 0.0) { a = b; fa = fb; } else { fa *= 0.5; }
            b = c; fb = fc;
        }
        return align_rotate(placed, camera[3], -b);
    }
    static inline float align_ramp(float distance) {
        float x = clamp(distance / 0.15, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }
    /* fade: [0..3] this panel's camera, [4] weapon zone, [5] (panels beneath,
     * softness, side-band reject), then four float4 per panel beneath. */
    static inline float align_alpha(float3 base, float3 placed, constant float4 *fade) {
        float2 st;
        if (!align_image(base, fade, st)) return 0.0;
        float edge = align_ramp(st.x + 1.0) * align_ramp(1.0 - st.x) * align_ramp(st.y + 1.0) * align_ramp(1.0 - st.y);
        if (edge >= 1.0) return 1.0;
        float laid = 0.0;
        uint count = uint(fade[5].x);
        for (uint j = 0; j < count && laid < 1.0; ++j) {
            constant float4 *camera = fade + 6 + 4 * j;
            float2 q;
            float3 d = placed;
            if (camera[2].w > 0.5) {
                if (!align_image(placed, camera, q) || min(1.0 - abs(q.x), 1.0 - abs(q.y)) < fade[5].z) continue;
                d = align_base(placed, camera, fade[4]);
            }
            if (!align_image(d, camera, q)) continue;
            float inside = min(1.0 - abs(q.x), 1.0 - abs(q.y));
            if (inside > 0.0) laid = max(laid, smoothstep(0.0, fade[5].y, inside));
        }
        return 1.0 - (1.0 - edge) * laid;
    }
    /* curve_fragment's colour, with the fade computed here rather than baked. */
    fragment float4 aligned_fragment(AlignedRaster in [[stage_in]], texture2d<float> frame [[texture(0)]],
                                     texture2d<float> other [[texture(1)]],
                                     constant uint &filter [[buffer(1)]], constant float4 *fade [[buffer(2)]]) {
        float2 clamped = clamp(in.uv, 0.0, 1.0);
        float2 outside = abs(in.uv - clamped);
        constexpr sampler bilinear(filter::linear, address::clamp_to_edge);
        float3 colour = filter == 1 ? (in.eye == 0 ? cubic_sample(frame, clamped) : cubic_sample(other, clamped)).rgb
                                    : (in.eye == 0 ? frame : other).sample(bilinear, clamped).rgb;
        float alpha = clamp(align_alpha(normalize(in.base), normalize(in.placed), fade), 0.0, 1.0);
        if (outside.y <= 0.0 && outside.x <= 0.0) return float4(colour, alpha);
        constexpr sampler s(filter::linear, address::clamp_to_edge);
        float3 wash = float3(0.0);
        for (int i = 0; i < 8; ++i) {
            float u = (float(i) + 0.5) / 8.0;
            wash += (in.eye == 0 ? frame : other).sample(s, float2(u, clamped.y)).rgb;
        }
        wash /= 8.0;
        float distance = outside.x + outside.y;
        colour = mix(colour, wash, clamp(distance * 3.0, 0.0, 1.0));
        float fade_out = 1.0 - smoothstep(0.0, 0.55, distance);
        return float4(colour * fade_out, alpha);
    }
    """
}

/// Plans once per engine frame, not per display frame, and keeps the
/// numbers the diagnostics timeline reports.
struct EngineLayerAlignmentState {
    private(set) var constants: [EngineLayerAlignment.PanelConstants]?
    private(set) var plan: EngineLayerAlignment.Plan?
    private var key: (sequence: UInt64, signature: [SIMD4<Float>])?
    /// The last engine frame counted in the statistics: a frame planned again
    /// for a new panel layout, or measured after it was planned, counts once.
    private var recorded: UInt64?
    private(set) var statistics = EngineLayerAlignmentStatistics()

    /// The constants for this snapshot, planning only when the snapshot or
    /// the panel layout has changed since the last call.
    mutating func update(sequence: UInt64, signature: [SIMD4<Float>],
                         sources: [EngineImmersiveScreenGeometry.PanoramaSource],
                         poses: [EngineLayerPose?], epochs: [UInt64], cutEpoch: UInt64)
        -> [EngineLayerAlignment.PanelConstants]? {
        if let key, key.sequence == sequence, key.signature == signature, let constants { return constants }
        guard sources.count == EngineLayerAlignment.panelCount else { constants = nil; plan = nil; key = nil; return nil }
        let desired = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: cutEpoch)
        let plan = EngineLayerAlignment.plan(sources: sources, desired: desired.rotations, cut: desired.cut)
        self.plan = plan
        constants = EngineLayerAlignment.constants(sources: sources, rotations: plan.rotations)
        key = (sequence, signature)
        statistics.enabled = true
        if recorded != sequence { statistics.record(plan, aligned: true); recorded = sequence }
        return constants
    }
    /// Alignment off (the default): nothing is turned and the panels are
    /// drawn as baked, but how far each layer's camera is from the newest
    /// centre's is still recorded once per engine frame. A run with it off is
    /// then the baseline for an A/B against a run, or a stretch of the same
    /// run, with it on. Costs one quaternion per layer per engine frame.
    mutating func measure(sequence: UInt64, poses: [EngineLayerPose?], epochs: [UInt64], cutEpoch: UInt64) {
        constants = nil; plan = nil; key = nil; statistics.enabled = false
        guard recorded != sequence else { return }
        recorded = sequence
        let desired = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: cutEpoch)
        let degrees = desired.rotations.map { EngineLayerAlignment.radians($0) * 180 / .pi }
        statistics.record(.identity(desired: degrees, cut: desired.cut), aligned: false)
    }
    /// No panorama: nothing to plan or measure.
    mutating func disable() { constants = nil; plan = nil; key = nil; statistics.enabled = false }
}

/// Per-layer correction angles for the diagnostics timeline. The current
/// values say what the last engine frame did; the sums and maxima cover
/// every distinct engine frame planned or measured, so two timeline records
/// a second apart give that second's mean, and `enabled` says which of the
/// two it was (the setting can change mid-run).
struct EngineLayerAlignmentStatistics {
    var enabled = false
    /// Every engine frame counted: aligned ones by level, plus those
    /// measured with alignment off (applied 0).
    var frames: UInt64 = 0
    var framesByLevel: [UInt64] = [0, 0, 0, 0]
    var framesOff: UInt64 = 0
    var framesWithCut: UInt64 = 0
    var appliedDegrees = [Float](repeating: 0, count: 10)
    var misalignedDegrees = [Float](repeating: 0, count: 10)
    var appliedSum = [Double](repeating: 0, count: 10)
    var misalignedSum = [Double](repeating: 0, count: 10)
    var appliedMax = [Float](repeating: 0, count: 10)
    var misalignedMax = [Float](repeating: 0, count: 10)
    var level = EngineLayerAlignment.Level.none

    mutating func record(_ plan: EngineLayerAlignment.Plan, aligned: Bool) {
        enabled = aligned; frames += 1; level = plan.level
        if aligned { framesByLevel[plan.level.rawValue] += 1 } else { framesOff += 1 }
        if plan.cutPanels > 0 { framesWithCut += 1 }
        appliedDegrees = Array(repeating: 0, count: 10); misalignedDegrees = Array(repeating: 0, count: 10)
        for (panel, layer) in EngineLayerAlignment.panelLayers.enumerated() {
            let applied = plan.appliedDegrees[panel], misaligned = plan.desiredDegrees[panel]
            appliedDegrees[layer] = applied; misalignedDegrees[layer] = misaligned
            appliedSum[layer] += Double(applied); misalignedSum[layer] += Double(misaligned)
            appliedMax[layer] = max(appliedMax[layer], applied); misalignedMax[layer] = max(misalignedMax[layer], misaligned)
        }
    }

    var dictionary: [String: Any] {
        func rounded(_ values: [Float]) -> [Double] { values.map { (Double($0) * 1000).rounded() / 1000 } }
        return ["enabled": enabled, "frames": frames, "level": ["full", "half", "capsTwistOnly", "none"][level.rawValue],
                "framesByLevel": framesByLevel, "framesOff": framesOff, "framesWithCut": framesWithCut,
                "appliedDegrees": rounded(appliedDegrees), "misalignedDegrees": rounded(misalignedDegrees),
                "appliedDegreesSum": appliedSum.map { ($0 * 1000).rounded() / 1000 },
                "misalignedDegreesSum": misalignedSum.map { ($0 * 1000).rounded() / 1000 },
                "appliedDegreesMax": rounded(appliedMax), "misalignedDegreesMax": rounded(misalignedMax)]
    }
}

/// The aligned pipeline, compiled the first time it is asked for rather than
/// when the renderer starts. Its library is the whole base shader again plus
/// the aligned functions (about 200 ms on a Mac), so it is built on a
/// background queue: the render thread asks each frame and gets nil until it
/// is ready, and nil for good if it did not build, which only leaves
/// alignment off. A session that never turns alignment on never builds it.
final class EngineAlignedPipeline: @unchecked Sendable {
    private enum Build { case idle, building, ready(MTLRenderPipelineState), failed }
    private let lock = NSLock()
    private var build = Build.idle
    private let device: MTLDevice
    private let source: String
    /// A copy of the panorama pipeline's descriptor (formats, sample count,
    /// amplification, blending); only the two functions are replaced.
    private let descriptor: MTLRenderPipelineDescriptor

    init(device: MTLDevice, source: String, descriptor: MTLRenderPipelineDescriptor) {
        self.device = device; self.source = source; self.descriptor = descriptor
    }

    /// The pipeline if it is built; the first call starts building it.
    func pipeline() -> MTLRenderPipelineState? {
        lock.lock(); defer { lock.unlock() }
        switch build {
        case .ready(let state): return state
        case .building, .failed: return nil
        case .idle:
            build = .building
            DispatchQueue.global(qos: .userInitiated).async { self.compile() }
            return nil
        }
    }

    /// True once the build has finished, either way (for the tests).
    var settled: Bool {
        lock.lock(); defer { lock.unlock() }
        switch build { case .ready, .failed: return true; case .idle, .building: return false }
    }

    private func compile() {
        var state: MTLRenderPipelineState?
        var failure = "its functions are missing"
        do {
            let library = try device.makeLibrary(source: source, options: nil)
            if let vertex = library.makeFunction(name: "aligned_vertex"), let fragment = library.makeFunction(name: "aligned_fragment") {
                descriptor.vertexFunction = vertex; descriptor.fragmentFunction = fragment
                state = try device.makeRenderPipelineState(descriptor: descriptor)
            }
        } catch {
            failure = "\(error)"
        }
        lock.lock(); build = state.map { .ready($0) } ?? .failed; lock.unlock()
        if state == nil { NSLog("[immersive] layer alignment unavailable: its pipeline did not build (%@)", failure as NSString) }
    }
}
