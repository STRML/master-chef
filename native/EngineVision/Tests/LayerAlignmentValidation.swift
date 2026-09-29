import Foundation
import simd

/// Layer alignment (EngineLayerAlignment.swift) against the host's own camera
/// construction. Each case rebuilds a bearing exactly as panorama_hooks.inc
/// does from the saved camera, projects a world direction into that layer's
/// picture, places the picture where the presenter puts it, turns it by the
/// planned rotation and checks it lands where the newest centre camera shows
/// the same direction. Then it checks what the turns must never do: open a
/// hole or a dim band anywhere on the sphere, move the weapon, change
/// anything when the camera has not moved, or line up a layer across a cut.
@main struct LayerAlignmentValidation {
    typealias Source = EngineImmersiveScreenGeometry.PanoramaSource

    // Projections as the headset reports them: the ring's 32.25 degree half
    // width and 105 degree height, the caps' 48.3 degree half width.
    static let ring = SIMD2<Float>(1.5847, 0.7593), cap = SIMD2<Float>(0.8910, 0.7593)
    static let sources: [Source] = EngineImmersiveScreenGeometry.panoramaSources(
        projections: Array(repeating: ring, count: 6) + [cap, cap],
        viewports: Array(repeating: SIMD4<Float>(0, 0, 1, 1), count: 8))

    static func degrees(_ r: Float) -> Float { r * 180 / .pi }
    static func radians(_ d: Float) -> Float { d * .pi / 180 }
    /// Degrees between two directions; atan2 keeps small angles exact where
    /// acos of a dot product near one would not.
    static func angle(_ a: SIMD3<Float>, _ b: SIMD3<Float>) -> Float {
        let x = simd_normalize(a), y = simd_normalize(b)
        return degrees(atan2(simd_length(simd_cross(x, y)), simd_dot(x, y)))
    }

    /// A Halo camera (z up) at yaw and pitch, degrees; up is perpendicular to
    /// forward, as the game builds it.
    static func pose(yaw: Float, pitch: Float, at position: SIMD3<Float> = .zero) -> EngineLayerPose {
        let y = radians(yaw), p = radians(pitch)
        return EngineLayerPose(position: position,
                               forward: SIMD3(cos(p) * cos(y), cos(p) * sin(y), sin(p)),
                               up: SIMD3(-sin(p) * cos(y), -sin(p) * sin(y), cos(p)))
    }

    /// The bearing camera panorama_hooks.inc builds for a layer from the
    /// saved camera: yaw about the camera's own up through right = f x u,
    /// then the zenith and nadir turn the whole basis.
    static func hostBearing(_ camera: EngineLayerPose, layer: Int) -> (f: SIMD3<Float>, u: SIMD3<Float>) {
        let yaws: [Int: Float] = [0: -1.0471975512, 1: 0, 2: 1.0471975512, 4: 0, 5: 0, 6: 0,
                                  7: 2.0943951024, 8: 3.1415926536, 9: -2.0943951024]
        let pitches: [Int: Float] = [5: 1.5707963268, 6: -1.5707963268]
        var f = camera.forward, u = camera.up
        let right = simd_cross(f, u), length = simd_length(right)
        let a = yaws[layer]!, pitch = pitches[layer] ?? 0
        f = f * cos(a) + right / length * sin(a)
        if pitch != 0 {
            let nf = f * cos(pitch) + u * sin(pitch), nu = u * cos(pitch) - f * sin(pitch)
            f = nf; u = nu
        }
        return (f, u)
    }

    /// Where the engine's picture for `layer`, drawn from `camera`, shows
    /// world direction w: image x is the camera's right (f x u), image y down.
    static func engineImage(_ w: SIMD3<Float>, camera: EngineLayerPose, layer: Int, projection: SIMD2<Float>) -> SIMD2<Float>? {
        let (f, u) = hostBearing(camera, layer: layer)
        let r = simd_normalize(simd_cross(f, u)), depth = simd_dot(f, w)
        guard depth > 1e-4 else { return nil }
        return SIMD2(projection.x * simd_dot(r, w) / depth, -projection.y * simd_dot(u, w) / depth)
    }

    /// Where the presenter lays image point st of a panel on the sphere
    /// before any turn (EngineImmersiveScreenGeometry.sphericalPanorama).
    static func laid(_ st: SIMD2<Float>, panel: Int) -> SIMD3<Float> {
        let (right, up, forward) = EngineImmersiveScreenGeometry.basis(of: sources[panel])
        return simd_normalize(right * (st.x / sources[panel].projection.x) - up * (st.y / sources[panel].projection.y) + forward)
    }

    static func layerPoses(centre: EngineLayerPose, others: [Int: EngineLayerPose]) -> [EngineLayerPose?] {
        var poses = [EngineLayerPose?](repeating: centre, count: 10)
        for (layer, camera) in others where layer != 1 && layer != 3 && layer != 4 { poses[layer] = camera }
        return poses
    }

    /// Composite coverage of all eight panels at direction p, in draw order.
    static func coverage(_ p: SIMD3<Float>, rotations: [simd_quatf]) -> Float {
        var laid: Float = 0
        for k in EngineImmersiveScreenGeometry.panoramaPanelOrder {
            let a = EngineLayerAlignment.alpha(panel: k, at: p, sources: sources, rotations: rotations)
            laid += a * (1 - laid)
        }
        return laid
    }
    /// Directions spread evenly over the whole sphere.
    static let sphere: [SIMD3<Float>] = (0..<12000).map { i in
        let y = 1 - 2 * (Float(i) + 0.5) / 12000, r = (1 - y * y).squareRoot()
        let phi = Float(i) * 2.39996323
        return SIMD3(r * cos(phi), y, r * sin(phi))
    }
    static func worstCoverage(_ rotations: [simd_quatf]) -> Float {
        sphere.reduce(Float(1)) { min($0, coverage($1, rotations: rotations)) }
    }

    static func main() {
        setbuf(stdout, nil)
        let identity = Array(repeating: EngineLayerAlignment.identityQuaternion, count: 8)

        // 1. Frames. The presenter's frame is the camera's: forward is -Z,
        // Halo's right (forward x up) is +X, up is +Y.
        let level = pose(yaw: 0, pitch: 0).presenterFromWorld!
        precondition(simd_length(level * SIMD3(1, 0, 0) - SIMD3(0, 0, -1)) < 1e-6)
        precondition(simd_length(level * SIMD3(0, -1, 0) - SIMD3(1, 0, 0)) < 1e-6)
        precondition(simd_length(level * SIMD3(0, 0, 1) - SIMD3(0, 1, 0)) < 1e-6)
        precondition(abs(simd_determinant(level) - 1) < 1e-5)
        precondition(EngineLayerPose(floats: Array(repeating: 0, count: 9)) == nil, "an unknown camera is all zero")
        precondition(EngineLayerPose(position: .zero, forward: SIMD3(Float.nan, 0, 0), up: SIMD3(0, 0, 1)).presenterFromWorld == nil)
        precondition(EngineLayerPose(position: .zero, forward: SIMD3(0, 0, 1), up: SIMD3(0, 0, 2)).presenterFromWorld == nil)

        // 2. The panels as laid out agree with the host's bearings: with one
        // camera for every layer, each world direction lands where layer 1
        // shows it. This is the premise everything else rests on.
        for (panel, layer) in EngineLayerAlignment.panelLayers.enumerated() {
            let camera = pose(yaw: 17, pitch: -23)
            let m = camera.presenterFromWorld!
            let (f, _) = hostBearing(camera, layer: layer)
            for offset in [SIMD3<Float>(0, 0, 0), SIMD3(0.2, 0.1, -0.1), SIMD3(-0.15, -0.2, 0.25)] {
                let w = simd_normalize(f + offset)
                guard let st = engineImage(w, camera: camera, layer: layer, projection: sources[panel].projection),
                      abs(st.x) <= 1, abs(st.y) <= 1 else { continue }
                precondition(angle(laid(st, panel: panel), m * w) < 0.01,
                             "panel \(panel) is not laid where the host drew layer \(layer)")
            }
        }

        // 3. Turned layers line up with the newest centre. A layer drawn from
        // an older camera is turned by the rotation to the newest one, and a
        // distant direction it shows lands exactly where layer 1 shows it,
        // for level and pitched cameras, stick yaw and pitch, and the caps.
        var worstAligned: Float = 0, worstUnaligned: Float = 0
        let cases: [(from: (Float, Float), to: (Float, Float))] = [
            ((0, 0), (2.3, 0)), ((0, 0), (-4.6, 0)), ((10, -30), (12.3, -30)), ((10, 40), (5.4, 40)),
            ((0, -60), (9.2, -60)), ((0, 0), (0, 2.3)), ((30, -10), (34, -14)), ((0, 5), (46, 5)), ((0, 0), (-80, 20))]
        for c in cases {
            let old = pose(yaw: c.from.0, pitch: c.from.1), new = pose(yaw: c.to.0, pitch: c.to.1)
            var poses = [EngineLayerPose?](repeating: old, count: 10), epochs = [UInt64](repeating: 30, count: 10)
            for layer in [1, 4] { poses[layer] = new; epochs[layer] = 50 }
            let desired = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 1).rotations
            let m = new.presenterFromWorld!
            for (panel, layer) in EngineLayerAlignment.panelLayers.enumerated() where panel != 1 {
                let (f, _) = hostBearing(old, layer: layer)
                for offset in [SIMD3<Float>(0, 0, 0), SIMD3(0.3, 0.2, -0.1), SIMD3(-0.2, -0.3, 0.2)] {
                    let w = simd_normalize(f + offset)
                    guard let st = engineImage(w, camera: old, layer: layer, projection: sources[panel].projection),
                          abs(st.x) <= 1, abs(st.y) <= 1 else { continue }
                    let placed = laid(st, panel: panel)
                    worstAligned = max(worstAligned, angle(desired[panel].act(placed), m * w))
                    worstUnaligned = max(worstUnaligned, angle(placed, m * w))
                }
            }
        }
        print(String(format: "turned layers meet the newest centre within %.4f deg (as drawn: up to %.1f deg off)", worstAligned, worstUnaligned))
        precondition(worstAligned < 0.01 && worstUnaligned > 40, "the turn is not the camera's")

        // The direction of the turn, spelled out: the camera turned right by
        // 2.3 degrees, so what the older layer showed straight ahead is now
        // 2.3 degrees to the left.
        do {
            let poses = layerPoses(centre: pose(yaw: -2.3, pitch: 0), others: [0: pose(yaw: 0, pitch: 0)])
            let q = EngineLayerAlignment.desired(poses: poses, epochs: [9, 10, 10, 10, 10, 10, 10, 10, 10, 10], cutEpoch: 0).rotations[0]
            let ahead = q.act(SIMD3(0, 0, -1))
            precondition(abs(degrees(atan2(ahead.x, -ahead.z)) + 2.3) < 0.001, "a right turn moves older pictures left")
        }

        // 4. A still camera changes nothing: exactly the identity, and the
        // per-fragment fades equal the baked ones.
        do {
            let still = pose(yaw: 33, pitch: -12, at: SIMD3(5, 6, 7))
            let desired = EngineLayerAlignment.desired(poses: Array(repeating: still, count: 10),
                                                       epochs: [3, 9, 9, 9, 9, 4, 5, 6, 7, 8], cutEpoch: 1)
            precondition(desired.rotations.allSatisfy { $0.vector == EngineLayerAlignment.identityQuaternion.vector })
            let plan = EngineLayerAlignment.plan(sources: sources, desired: desired.rotations)
            precondition(plan.level == .none && plan.appliedDegrees.allSatisfy { $0 == 0 })
            let constants = EngineLayerAlignment.constants(sources: sources, rotations: plan.rotations)
            precondition(constants.count == 8 && constants.allSatisfy {
                $0.vertex.count == EngineLayerAlignment.vertexConstantCount &&
                $0.fragment.count == EngineLayerAlignment.fragmentConstantCount && $0.vertex[0] == SIMD4(0, 1, 0, 0) })
            // What each panel contributes to the final colour once every later
            // panel is blended over it. (A band's alpha can differ in the corner
            // where two bands and a cap meet, but the cap is solid there.)
            func contributions(_ alphas: [Float]) -> [Float] {
                var result = [Float](repeating: 0, count: 8), above: Float = 1
                for k in EngineImmersiveScreenGeometry.panoramaPanelOrder.reversed() { result[k] = alphas[k] * above; above *= 1 - alphas[k] }
                return result
            }
            var worst: Float = 0
            for p in sphere {
                let baked = contributions(EngineImmersiveScreenGeometry.panelAlphas(sources: sources, direction: p))
                let aligned = contributions((0..<8).map { EngineLayerAlignment.alpha(panel: $0, at: p, sources: sources, rotations: identity) })
                for k in 0..<8 { worst = max(worst, abs(baked[k] - aligned[k])) }
            }
            print(String(format: "unturned per-fragment fades change any panel's share of the picture by at most %.5f", worst))
            // Half an 8-bit level; the baked mesh's own interpolation across a
            // grid cell moves shares by more than this.
            precondition(worst < 0.002, "with nothing turned the picture must be the baked one")
            precondition(EngineLayerAlignment.coverageHolds(sources: sources, rotations: identity), "the layout itself must pass")
            precondition(worstCoverage(identity) > 0.9999)
        }

        // 5. The check notices a hole: one side band turned ten degrees
        // outward leaves its join with the centre open.
        do {
            var turned = identity
            turned[2] = simd_quatf(angle: radians(-10), axis: SIMD3(0, 1, 0))
            precondition(!EngineLayerAlignment.coverageHolds(sources: sources, rotations: turned))
            precondition(worstCoverage(turned) < 0.5)
        }

        // 6. Plans for what the headset does: the stick at up to 2.3 degrees
        // a frame (and faster for a mouse), layer ages as Build75 measured
        // them (sides 0-3 frames, caps and rear up to 40), level and pitched
        // cameras, aim moving up and down. Every plan leaves the whole sphere
        // covered, with no dim band.
        var plans = 0, full = 0, sideApplied: Float = 0, sideDesired: Float = 0, capApplied: Float = 0, capDesired: Float = 0
        var worstPlanned: Float = 1, timings: [Double] = []
        let ageSets: [[Int]] = [[1, 0, 2, 5, 9, 3, 7, 11], [0, 0, 1, 18, 20, 19, 17, 22], [3, 0, 3, 35, 36, 34, 40, 38],
                                [1, 0, 0, 1, 3, 5, 2, 4], [2, 0, 1, 26, 12, 30, 8, 16]]
        for rate in [Float(-2.3), 1.1, 2.3, 4.6] {
            for pitch in [Float(-60), -30, 0, 25, 55] {
                for pitchRate in [Float(0), 0.8, -1.2] {
                    for ages in ageSets {
                        let newest: UInt64 = 1000
                        var poses = [EngineLayerPose?](repeating: nil, count: 10), epochs = [UInt64](repeating: newest, count: 10)
                        let current = pose(yaw: 0, pitch: pitch)
                        for (panel, layer) in EngineLayerAlignment.panelLayers.enumerated() {
                            let age = Float(ages[panel])
                            poses[layer] = pose(yaw: -rate * age, pitch: max(-80, min(80, pitch - pitchRate * age)))
                            epochs[layer] = newest - UInt64(ages[panel])
                        }
                        poses[1] = current; poses[4] = current; poses[3] = current
                        let started = Date()
                        let desired = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 1)
                        let plan = EngineLayerAlignment.plan(sources: sources, desired: desired.rotations)
                        _ = EngineLayerAlignment.constants(sources: sources, rotations: plan.rotations)
                        timings.append(Date().timeIntervalSince(started))
                        plans += 1; if plan.level == .full { full += 1 }
                        precondition(plan.level != .none || ages.allSatisfy { $0 == 0 })
                        precondition(EngineLayerAlignment.coverageHolds(sources: sources, rotations: plan.rotations))
                        worstPlanned = min(worstPlanned, worstCoverage(plan.rotations))
                        for side in [0, 2] { sideApplied += plan.appliedDegrees[side]; sideDesired += plan.desiredDegrees[side] }
                        // The stick's own rate with a side band a frame old is
                        // well inside the overlap: turned all the way.
                        if abs(rate) <= 2.3 && abs(pitch) <= 30 && pitchRate == 0 {
                            for side in [0, 2] where ages[side] <= 1 {
                                precondition(abs(plan.appliedDegrees[side] - plan.desiredDegrees[side]) < 0.01, "a one-frame side band was held back")
                            }
                        }
                        for c in [6, 7] { capApplied += plan.appliedDegrees[c]; capDesired += plan.desiredDegrees[c] }
                        // The centre is never turned and the side bands never further
                        // than their camera did. (A rear band can be carried a little
                        // past its own turn by older neighbours on both sides, so
                        // that neither join opens.)
                        precondition(plan.appliedDegrees[1] == 0)
                        for k in [0, 2] { precondition(plan.appliedDegrees[k] <= plan.desiredDegrees[k] + 0.01) }
                    }
                }
            }
        }
        timings.sort()
        print(String(format: "%d plans (%d at full limits): coverage never below %.5f; sides turned %.0f%% and caps %.0f%% of their camera's turn; plan median %.3f ms, p90 %.3f ms (once per engine frame)",
                     plans, full, worstPlanned, 100 * sideApplied / sideDesired, 100 * capApplied / capDesired,
                     timings[timings.count / 2] * 1000, timings[timings.count * 9 / 10] * 1000))
        precondition(worstPlanned > 0.9999, "a plan opened a hole or a dim band")
        precondition(sideApplied / sideDesired > 0.5 && capApplied / capDesired > 0.7)

        // A level turn: the side bands a frame or two old are turned all the
        // way, and the caps twist the whole turn however old they are.
        do {
            var poses = [EngineLayerPose?](repeating: pose(yaw: 0, pitch: 0), count: 10)
            poses[0] = pose(yaw: -2.3, pitch: 0); poses[2] = pose(yaw: -4.6, pitch: 0)
            poses[5] = pose(yaw: -69, pitch: 0); poses[6] = pose(yaw: -92, pitch: 0)
            for layer in [7, 8, 9] { poses[layer] = pose(yaw: -46, pitch: 0) }
            let epochs: [UInt64] = [99, 100, 98, 100, 100, 70, 60, 80, 80, 80]
            let desired = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 1)
            let plan = EngineLayerAlignment.plan(sources: sources, desired: desired.rotations)
            precondition(plan.level == .full)
            precondition(abs(plan.appliedDegrees[0] - 2.3) < 0.01 && abs(plan.appliedDegrees[6] - 69) < 0.01 && abs(plan.appliedDegrees[7] - 92) < 0.01)
            precondition(plan.appliedDegrees[2] > 3.5 && plan.appliedDegrees[2] < 4.6, "the side's join with the centre is held inside its overlap")
            print(String(format: "level turn: sides %.2f/%.2f deg, rear %.1f %.1f %.1f of 46, sky %.0f, floor %.0f",
                         plan.appliedDegrees[0], plan.appliedDegrees[2], plan.appliedDegrees[3], plan.appliedDegrees[4],
                         plan.appliedDegrees[5], plan.appliedDegrees[6], plan.appliedDegrees[7]))
        }

        // 7. The weapon. It is attached to the camera, so in a side band it is
        // already where it belongs: turning the band must not move it, or its
        // copy there would tear away from the centre's across the join.
        do {
            var poses = [EngineLayerPose?](repeating: pose(yaw: 0, pitch: -20), count: 10)
            poses[0] = pose(yaw: 3, pitch: -20); poses[2] = pose(yaw: 3, pitch: -20)
            let desired = EngineLayerAlignment.desired(poses: poses, epochs: [9, 10, 9, 10, 10, 10, 10, 10, 10, 10], cutEpoch: 0)
            let plan = EngineLayerAlignment.plan(sources: sources, desired: desired.rotations)
            precondition(plan.appliedDegrees[0] > 2.9 && plan.appliedDegrees[2] > 2.9)
            func direction(_ azimuth: Float, _ elevation: Float) -> SIMD3<Float> {
                let a = radians(azimuth), e = radians(elevation)
                return SIMD3(sin(a) * cos(e), sin(e), -cos(a) * cos(e))
            }
            var worstWeapon: Float = 0
            for side in [0, 2] {
                let sign: Float = side == 0 ? -1 : 1
                for azimuth in stride(from: Float(28), through: 55, by: 3) {
                    for elevation in stride(from: Float(-50), through: -5, by: 5) {
                        let d = direction(sign * azimuth, elevation)
                        let moved = EngineLayerAlignment.effective(plan.rotations[side], panel: side, at: d).act(d)
                        worstWeapon = max(worstWeapon, angle(moved, d))
                    }
                }
                // Above the weapon and beyond it the band takes its whole turn.
                for d in [direction(sign * 40, 20), direction(sign * 80, -30)] {
                    let full = EngineLayerAlignment.effective(plan.rotations[side], panel: side, at: d)
                    precondition(angle(full.act(d), plan.rotations[side].act(d)) < 0.001, "the band is bent outside the weapon's region")
                }
            }
            print(String(format: "weapon region of the side bands moves %.4f deg under a %.1f deg turn", worstWeapon, plan.appliedDegrees[2]))
            precondition(worstWeapon < 0.001, "the weapon would tear at the side joins")
            precondition(worstCoverage(plan.rotations) > 0.9999)
            // Where the centre and a side band both hold the weapon, the two
            // copies still coincide: a camera-locked point in the overlap is
            // laid at the same place by both panels.
            for elevation in stride(from: Float(-45), through: -6, by: 3) {
                for azimuth in [Float(28.5), 30, 31.5] {
                    for side in [0, 2] {
                        let d = direction(side == 0 ? -azimuth : azimuth, elevation)
                        precondition(angle(EngineLayerAlignment.effective(plan.rotations[side], panel: side, at: d).act(d), d) < 0.001)
                    }
                }
            }
        }

        // 8. The fixed-point inverse finds a side band's own direction under
        // its bent turn to well inside a pixel.
        do {
            let q = simd_quatf(angle: radians(4), axis: simd_normalize(SIMD3<Float>(0.1, 1, 0.3)))
            var worst: Float = 0
            for p in sphere where p.x > 0.4 {
                let d = simd_normalize(p)
                let placed = EngineLayerAlignment.effective(q, panel: 2, at: d).act(d)
                worst = max(worst, angle(EngineLayerAlignment.base(of: placed, panel: 2, rotation: q), d))
            }
            print(String(format: "side band inverse within %.5f deg", worst))
            precondition(worst < 0.005)
        }

        // 9. Head pose never enters: the turn is applied on the sphere before
        // the eye's view, so a rolled, pitched or turned head sees the turned
        // older picture and the fresh centre agree on screen exactly as they
        // agree on the sphere.
        do {
            let old = pose(yaw: 0, pitch: -15), new = pose(yaw: 3.5, pitch: -15)
            var poses = [EngineLayerPose?](repeating: old, count: 10), epochs = [UInt64](repeating: 4, count: 10)
            for layer in [1, 4] { poses[layer] = new; epochs[layer] = 5 }
            let q = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 1).rotations[5]
            let w = simd_normalize(hostBearing(old, layer: 9).f + SIMD3(0.1, 0.2, -0.3))   // a direction the older layer 9 shows
            guard let st = engineImage(w, camera: old, layer: 9, projection: ring), abs(st.x) <= 1, abs(st.y) <= 1 else { fatalError("setup") }
            let stale = q.act(laid(st, panel: 5)) * EngineImmersiveScreenGeometry.panoramaRadius
            let fresh = new.presenterFromWorld! * w * EngineImmersiveScreenGeometry.panoramaRadius
            for head in [simd_quatf(angle: radians(25), axis: SIMD3(0, 0, 1)),
                         simd_quatf(angle: radians(-40), axis: simd_normalize(SIMD3(0.3, 1, 0.2)))] {
                let view = simd_float4x4(head.inverse)
                let a = view * SIMD4(stale, 1), b = view * SIMD4(fresh, 1)
                precondition(simd_length(a - b) < 1e-3)
            }
        }

        // 10. Cuts and unusable cameras leave layers as drawn.
        do {
            var poses = [EngineLayerPose?](repeating: pose(yaw: 0, pitch: 0), count: 10)
            for layer in [0, 2, 5, 6, 7, 9] { poses[layer] = pose(yaw: -20, pitch: 0) }
            poses[8] = pose(yaw: -2.3, pitch: 0)
            let epochs: [UInt64] = [95, 100, 95, 100, 100, 95, 95, 95, 99, 95]
            // Twenty degrees over five frames is four a frame: a fast turn, aligned.
            var d = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 90)
            precondition(d.cut == 0 && EngineLayerAlignment.radians(d.rotations[6]) > radians(19))
            // After a cut at epoch 97, everything older is left as drawn.
            d = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 97)
            precondition(d.cut == 6 && EngineLayerAlignment.radians(d.rotations[6]) == 0)
            precondition(EngineLayerAlignment.radians(d.rotations[4]) > radians(2.2), "layer 8 was drawn after the cut")
            // Twenty degrees in one frame is not a stick turn: treated as a cut.
            d = EngineLayerAlignment.desired(poses: poses, epochs: epochs, cutEpoch: 90)
            precondition(EngineLayerAlignment.radians(d.rotations[4]) > 0)
            var fast = epochs; fast[8] = 99
            poses[8] = pose(yaw: -30, pitch: 0)
            d = EngineLayerAlignment.desired(poses: poses, epochs: fast, cutEpoch: 90)
            precondition(EngineLayerAlignment.radians(d.rotations[4]) == 0 && d.cut == 1)
            // No centre camera, or an unknown one for a layer: nothing turns there.
            var unknown = poses; unknown[1] = nil
            precondition(EngineLayerAlignment.desired(poses: unknown, epochs: epochs, cutEpoch: 0).rotations
                .allSatisfy { EngineLayerAlignment.radians($0) == 0 })
            unknown = poses; unknown[5] = EngineLayerPose(floats: Array(repeating: 0, count: 9))
            precondition(EngineLayerAlignment.radians(EngineLayerAlignment.desired(poses: unknown, epochs: epochs, cutEpoch: 0).rotations[6]) == 0)
            // A layer claiming to be newer than the centre, or never drawn.
            var odd = epochs; odd[6] = 101; odd[7] = 0
            let o = EngineLayerAlignment.desired(poses: poses, epochs: odd, cutEpoch: 0)
            precondition(EngineLayerAlignment.radians(o.rotations[7]) == 0 && EngineLayerAlignment.radians(o.rotations[3]) == 0)
        }

        // 11. State: planned once per engine frame, the timeline's numbers
        // follow it, and the same snapshot reuses its constants.
        do {
            var state = EngineLayerAlignmentState()
            var poses = [EngineLayerPose?](repeating: pose(yaw: 0, pitch: 0), count: 10)
            poses[0] = pose(yaw: -2.3, pitch: 0)
            let epochs: [UInt64] = [9, 10, 10, 10, 10, 10, 10, 10, 10, 10]
            let signature = [SIMD4<Float>(1, 2, 3, 4)]
            precondition(state.update(sequence: 7, signature: signature, sources: sources, poses: poses, epochs: epochs, cutEpoch: 1) != nil)
            _ = state.update(sequence: 7, signature: signature, sources: sources, poses: poses, epochs: epochs, cutEpoch: 1)
            precondition(state.statistics.frames == 1)
            _ = state.update(sequence: 8, signature: signature, sources: sources, poses: poses, epochs: epochs, cutEpoch: 1)
            let stats = state.statistics
            precondition(stats.frames == 2 && abs(stats.appliedDegrees[0] - 2.3) < 0.01 && stats.appliedDegrees[1] == 0)
            precondition(abs(stats.appliedSum[0] - 4.6) < 0.02 && stats.framesByLevel[0] == 2)
            let dictionary = stats.dictionary
            precondition((dictionary["appliedDegrees"] as? [Double])?.count == 10 && dictionary["level"] as? String == "full")
            precondition(JSONSerialization.isValidJSONObject(dictionary))
            state.disable(); precondition(state.constants == nil && !state.statistics.enabled)
            // Off (the default): nothing is turned, but the cameras' offsets
            // are still counted once per engine frame, as the A/B baseline.
            state.measure(sequence: 8, poses: poses, epochs: epochs, cutEpoch: 1)
            precondition(state.statistics.frames == 2 && state.statistics.framesOff == 0, "a frame already planned counts once")
            state.measure(sequence: 9, poses: poses, epochs: epochs, cutEpoch: 1)
            state.measure(sequence: 9, poses: poses, epochs: epochs, cutEpoch: 1)
            var off = state.statistics
            precondition(state.constants == nil && state.plan == nil && !off.enabled)
            precondition(off.frames == 3 && off.framesOff == 1 && off.framesByLevel.reduce(0, +) == 2)
            precondition(off.appliedDegrees.allSatisfy { $0 == 0 } && abs(off.misalignedDegrees[0] - 2.3) < 0.01)
            precondition(abs(off.appliedSum[0] - 4.6) < 0.02 && abs(off.misalignedSum[0] - 6.9) < 0.03)
            precondition(off.dictionary["framesOff"] as? UInt64 == 1 && off.dictionary["enabled"] as? Bool == false)
            // On again: the next frame is planned and counted as aligned.
            precondition(state.update(sequence: 10, signature: signature, sources: sources, poses: poses, epochs: epochs, cutEpoch: 1) != nil)
            off = state.statistics
            precondition(off.enabled && off.frames == 4 && off.framesOff == 1 && off.framesByLevel[0] == 3)
        }
        print("PASS layer alignment: host bearings, exact turns, still camera unchanged, no holes or dim bands, weapon held, cuts left alone")
    }
}
