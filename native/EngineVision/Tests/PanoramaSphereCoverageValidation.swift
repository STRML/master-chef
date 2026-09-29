import Foundation
import simd

/// The view is eight camera pictures drawn as panels on one sphere,
/// in a fixed order, each dissolving into the one beneath it. Over blending
/// makes the result a proper mix whatever the alphas are, so there is no
/// brightness ripple to check. What has to hold instead is coverage: for
/// every direction across the hemisphere the panels must build up to fully
/// opaque, or the surround behind them shows through as a hole or a dim
/// patch. That is what this measures, by compositing the panels exactly as
/// the rasteriser will.
@main struct PanoramaSphereCoverageValidation {
static func main() {
    setbuf(stdout, nil)
    // Forward bands keep their 64.8 degree horizontal field. The two cap
    // views are widened to 85 so they reach the sides of the hemisphere;
    // projection.x is 1/tan(halfWidth), projection.y is 1/tan(halfHeight).
    let bandX: Float = 1 / tan(64.8 / 2 * .pi / 180)
    let capX: Float = 1 / tan(96.0 / 2 * .pi / 180)
    let projY: Float = 1 / tan(105.0 / 2 * .pi / 180)
    let projections: [SIMD2<Float>] = Array(repeating: SIMD2(bandX, projY), count: 6)
                                    + [SIMD2(capX, projY), SIMD2(capX, projY)]
    let viewports = [SIMD4<Float>](repeating: SIMD4(0, 0, 1, 1), count: 8)
    let sources = EngineImmersiveScreenGeometry.panoramaSources(projections: projections, viewports: viewports)
    let fades = EngineImmersiveScreenGeometry.panoramaPanelFades
    let meshes = EngineImmersiveScreenGeometry.sphericalPanorama(sources: sources, texelSize: .zero)
    precondition(meshes.count == 8 && fades.count == 8)

    // One surface: no step where two panels meet, whatever the head does.
    var minR = Float.infinity, maxR: Float = 0
    for mesh in meshes { for v in mesh.vertices {
        let r = simd_length(SIMD3(v.position.x, v.position.y, v.position.z))
        minR = min(minR, r); maxR = max(maxR, r)
    } }
    print(String(format: "vertex radius %.4f to %.4f", minR, maxR))
    precondition(maxR - minR < 0.001, "the panels are not on one sphere")

    // The sky and floor cameras are pitched by turning the whole basis, the
    // way the host does it: forward becomes +Y and up becomes +Z for the
    // sky, and -Y and -Z for the floor. So the sky camera's up axis points
    // BEHIND the viewer, and a direction tilted from straight up towards
    // where the head is looking belongs in the bottom half of that picture.
    // Assuming the opposite renders both caps vertically mirrored against
    // the bands they meet, which is what used to be wrong at the poles.
    let sky = sources[6], floor = sources[7]
    let towardsFront = simd_normalize(SIMD3<Float>(0, 3, -1))     // up, leaning forward
    let towardsBack  = simd_normalize(SIMD3<Float>(0, 3,  1))
    guard let skyFront = EngineImmersiveScreenGeometry.imagePointForTest(sky, towardsFront),
          let skyBack = EngineImmersiveScreenGeometry.imagePointForTest(sky, towardsBack),
          let floorFront = EngineImmersiveScreenGeometry.imagePointForTest(floor, simd_normalize(SIMD3<Float>(0, -3, -1))),
          let floorBack = EngineImmersiveScreenGeometry.imagePointForTest(floor, simd_normalize(SIMD3<Float>(0, -3, 1)))
    else { preconditionFailure("the cap cameras do not see straight up or down") }
    print(String(format: "sky: forward lands at t %+.3f, backward at t %+.3f", skyFront.y, skyBack.y))
    print(String(format: "floor: forward lands at t %+.3f, backward at t %+.3f", floorFront.y, floorBack.y))
    precondition(skyFront.y > 0 && skyBack.y < 0, "the sky cap is vertically mirrored")
    precondition(floorFront.y < 0 && floorBack.y > 0, "the floor cap is vertically mirrored")
    // And the caps must agree with the bands where they meet. Straight up
    // and slightly forward is inside both the sky cap and the centre band;
    // both must place it above the middle of their own pictures.
    let shared = simd_normalize(SIMD3<Float>(0, 1, -1))
    if let inSky = EngineImmersiveScreenGeometry.imagePointForTest(sky, shared),
       let inBand = EngineImmersiveScreenGeometry.imagePointForTest(sources[1], shared),
       abs(inBand.x) <= 1, abs(inBand.y) <= 1 {
        print(String(format: "a direction shared by sky and centre band: sky t %+.3f, band t %+.3f", inSky.y, inBand.y))
        precondition(inBand.y < 0, "the centre band puts an upward direction below its middle")
    }

    // Composite the panels in draw order, reading the alpha the way the
    // rasteriser does: bilinearly across the panel's own grid, not from the
    // smooth function, so tessellation error is included.
    let n = EngineImmersiveScreenGeometry.panoramaPanelDivisions
    func alphaOnMesh(_ index: Int, _ d: SIMD3<Float>) -> Float {
        guard let p = EngineImmersiveScreenGeometry.imagePointForTest(sources[index], d),
              abs(p.x) <= 1, abs(p.y) <= 1 else { return 0 }
        let fx = (p.x + 1) / 2 * Float(n), fy = (p.y + 1) / 2 * Float(n)
        let cx = min(n - 1, max(0, Int(fx))), cy = min(n - 1, max(0, Int(fy)))
        let tx = fx - Float(cx), ty = fy - Float(cy)
        let v = meshes[index].vertices
        func w(_ c: Int, _ r: Int) -> Float { v[r * (n + 1) + c].weight.x }
        return (w(cx, cy) * (1 - tx) + w(cx + 1, cy) * tx) * (1 - ty)
             + (w(cx, cy + 1) * (1 - tx) + w(cx + 1, cy + 1) * tx) * ty
    }

    var holes = 0, worstShortfall: Float = 0, worstAt = (0, 0), samples = 0
    for elevation in stride(from: -89, through: 89, by: 1) {
        for azimuth in stride(from: -179, through: 180, by: 1) {
            let e = Float(elevation) * .pi / 180, a = Float(azimuth) * .pi / 180
            let d = simd_normalize(SIMD3(cos(e) * sin(a), sin(e), -cos(e) * cos(a)))
            samples += 1
            var covered: Float = 0
            for i in EngineImmersiveScreenGeometry.panoramaPanelOrder {
                let alpha = alphaOnMesh(i, d)
                covered = covered * (1 - alpha) + alpha        // source over
            }
            if covered < 0.999 {
                if 1 - covered > worstShortfall { worstShortfall = 1 - covered; worstAt = (azimuth, elevation) }
                if covered < 0.5 { holes += 1 }
            }
        }
    }
    print("whole-sphere samples: \(samples), directions less than half covered: \(holes)")
    print(String(format: "worst shortfall from solid: %.3f%% at azimuth %d elevation %d",
                 worstShortfall * 100, worstAt.0, worstAt.1))
    precondition(holes == 0, "the panels leave a hole")
    precondition(worstShortfall < 0.01, "the panels do not build up to solid everywhere")
    print("PANORAMA PANELS OK: one sphere, solid in every direction, every join a dissolve")
}
}
