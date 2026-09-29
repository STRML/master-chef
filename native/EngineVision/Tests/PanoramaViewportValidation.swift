// CPU coverage test with patterned valid pixels and a distinct invalid sentinel.
// Tests source coverage and finite mesh boundaries independently of scene content.
import Foundation
import simd

@main enum PanoramaViewportValidation {
    static func main() {
        let radius: Float = 4.25, halfBand = Float.pi / 6
        let fixtures: [(String, [SIMD4<Float>])] = [
            ("full", Array(repeating: SIMD4(0, 0, 1, 1), count: 3)),
            ("centered75", Array(repeating: SIMD4(0, 0.125, 1, 0.875), count: 3)),
            ("asymmetric", [SIMD4(0.0625, 0.0625, 0.9375, 0.8125),
                             SIMD4(0, 0.1875, 0.875, 0.9375), SIMD4(0.125, 0.125, 1, 0.875)])]
        var totalMisses = 0, totalHits = 0, contentMissing = 0
        for (name, rectangles) in fixtures {
            let projections = (0..<3).map { sector -> SIMD2<Float> in
                let rect = rectangles[sector]
                return SIMD2(1.5752 * (rect.z-rect.x), (0.54 + Float(sector)*0.014) * (rect.w-rect.y))
            }
            let commonY = (0..<3).map { projections[$0].y / (rectangles[$0].w-rectangles[$0].y) }.max()!
            let halfHeight = radius * cos(halfBand) / commonY
            // Rectangles have integer pixel boundaries at this texture size.
            let width = 128, height = 96
            func pixel(_ uv: SIMD2<Float>, _ sector: Int) -> UInt32 {
                let x = min(width-1,max(0,Int(uv.x*Float(width))))
                let y = min(height-1,max(0,Int(uv.y*Float(height))))
                let rect = rectangles[sector]
                if Float(x) < rect.x*Float(width) || Float(x) >= rect.z*Float(width) ||
                   Float(y) < rect.y*Float(height) || Float(y) >= rect.w*Float(height) { return 0xffff00ff }
                // A black grid cell deliberately models missing scene content
                // inside valid source coverage; this is never a viewport miss.
                return x/8 % 4 == 1 && y/8 % 4 == 1 ? 0xff000000 : 0xff808080 | UInt32((x+y)&0x7f)
            }
            for sector in 0..<3 {
                let rect = rectangles[sector]
                let mesh = EngineImmersiveScreenGeometry.panorama(sector: sector,
                    projection: projections[sector], maxProjectionY: commonY,
                    viewport: rect, texelSize: SIMD2(1/Float(width),1/Float(height)))
                for vertex in mesh.vertices {
                    precondition(vertex.uv.x >= rect.x && vertex.uv.x <= rect.z)
                    precondition(vertex.uv.y >= rect.y && vertex.uv.y <= rect.w)
                    precondition(pixel(vertex.uv,sector) != 0xffff00ff, "invalid source band sampled: \(name)")
                    precondition(abs(vertex.position.y) <= halfHeight + 0.0001)
                }
                let center = mesh.vertices[20*65+32]
                let local = atan2(center.position.x,-center.position.z) - Float(sector-1) * Float.pi/3
                precondition(abs(center.uv.x-((rect.x+rect.z)/2+0.5*tan(local)*projections[sector].x)) < 0.0001)
                precondition(abs(center.uv.y-(rect.y+rect.w)/2) < 0.0001)
            }
            var unchangedCounts: [Int] = []
            for recline: Float in [0,45,80] {
                let neutral = simd_float3x3(simd_quatf(angle: recline * .pi/180, axis: SIMD3(1,0,0)))
                var matchedHits = 0
                for pitch: Float in [-10,-2,0,2,10] { for yaw: Float in [-10,0,10] {
                    let motion = simd_float3x3(simd_quatf(angle:yaw * .pi/180,axis:SIMD3(0,1,0))) *
                        simd_float3x3(simd_quatf(angle:pitch * .pi/180,axis:SIMD3(1,0,0)))
                    let current = neutral * motion
                    let relative = simd_inverse(neutral) * current
                    for eye: Float in [-0.032,0.032] {
                        let origin = relative * SIMD3(eye,0,0)
                        for iy in 0..<23 { for ix in 0..<23 {
                            let ray = relative * SIMD3((Float(ix)/22*2-1)*1.15, (Float(iy)/22*2-1)*1.4281, -1)
                            let a=ray.x*ray.x+ray.z*ray.z
                            let b=2*(origin.x*ray.x+origin.z*ray.z)
                            let c=origin.x*origin.x+origin.z*origin.z-radius*radius
                            let t=(-b+sqrt(b*b-4*a*c))/(2*a), point=origin+ray*t
                            let theta=atan2(point.x,-point.z)
                            if abs(theta) > .pi/2-EngineImmersiveScreenGeometry.panoramaFrontEdgeEpsilon || abs(point.y) > halfHeight {
                                totalMisses += 1; continue
                            }
                            let sector = theta < -halfBand ? 0 : (theta > halfBand ? 2 : 1)
                            let local=theta-Float(sector-1) * Float.pi/3, rect=rectangles[sector], p=projections[sector]
                            let uv=SIMD2((rect.x+rect.z)/2+0.5*tan(local)*p.x,
                                         (rect.y+rect.w)/2-0.5*point.y/(radius*cos(local))*p.y)
                            precondition(uv.x >= rect.x-0.00001 && uv.x <= rect.z+0.00001 && uv.y >= rect.y-0.00001 && uv.y <= rect.w+0.00001)
                            let sample=pixel(uv,sector)
                            precondition(sample != 0xffff00ff, "source miss confused with finite mesh boundary")
                            if sample == 0xff000000 { contentMissing += 1 }
                            totalHits += 1
                            if pitch == 0 && yaw == 0 { matchedHits += 1 }
                        } }
                    }
                } }
                unchangedCounts.append(matchedHits)
            }
            precondition(Set(unchangedCounts).count == 1, "absolute recline changed matched relative pose coverage")
            print("PANORAMA_VIEWPORT_PASS fixture=\(name) validPixels=true unequalSectorProjections=true recline=0,45,80 relativePitch=0,2,10 relativeYaw=0,10 eyes=2")
        }
        precondition(totalMisses > 0 && totalHits > 0 && contentMissing > 0)
        print("PANORAMA_COVERAGE_CLASSIFICATION_PASS finiteMeshMisses=\(totalMisses) invalidViewportSamples=0 validSamples=\(totalHits) syntheticMissingContent=\(contentMissing)")
    }
}
