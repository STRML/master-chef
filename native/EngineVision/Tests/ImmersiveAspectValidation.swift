import Foundation
import simd

for aspect: Float in [4/3, 16/9, 1024/640, 1, 32/9, 8] {
    let mesh = EngineImmersiveScreenGeometry.make(aspect: aspect)
    let left = mesh.vertices[0].position, right = mesh.vertices[192].position
    let width = (atan2(right.x, -right.z) - atan2(left.x, -left.z)) * 4.25
    let height = left.y - mesh.vertices[1].position.y
    precondition(abs(width / height - aspect) < 0.0001, "image stretched")
    precondition(width <= Float.pi * 4.25 + 0.001)
    precondition(mesh.vertices.count == 194 && mesh.indices.count == 576)
    precondition(mesh.indices.allSatisfy { $0 < mesh.vertices.count })
    precondition(mesh.vertices.first!.uv == SIMD2(0,0) && mesh.vertices.last!.uv == SIMD2(1,1), "image cropped")
    print("ASPECT_PASS input=\(aspect) displayed=\(width/height) horizontalDegrees=\(width/4.25*180/Float.pi)")
}
print("IMMERSIVE_ASPECT_VALIDATION_PASS")
