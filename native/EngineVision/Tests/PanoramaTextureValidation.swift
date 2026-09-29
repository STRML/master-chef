import Foundation
import Metal
import simd

struct EngineVisionPanoramaInfo {
    var sequence: UInt64 = 0
    var width: Int32 = 0
    var height: Int32 = 0
    var byte_count: Int = 0
    var projection_x: (Float, Float, Float) = (1, 1, 1)
    var projection_y: (Float, Float, Float) = (1, 1, 1)
    var source_epoch: UInt64 = 0
    var flat_sequence: UInt64 = 0
    var status: UInt32 = 1
    var failure_reason: UInt32 = 0
    var viewport_u_min: (Float, Float, Float) = (0, 0, 0)
    var viewport_v_min: (Float, Float, Float) = (0, 0, 0)
    var viewport_u_max: (Float, Float, Float) = (1, 1, 1)
    var viewport_v_max: (Float, Float, Float) = (1, 1, 1)
}

private var fixture = EngineVisionPanoramaInfo()
private var infoAvailable = true
private var copyAvailable = true

func enginevision_panorama_info(_ out: inout EngineVisionPanoramaInfo) -> Bool {
    out = fixture
    return infoAvailable
}

func enginevision_copy_panorama(_ destination: UnsafeMutableRawPointer?, _ capacity: Int,
                                _ out: inout EngineVisionPanoramaInfo) -> Bool {
    out = fixture
    guard copyAvailable, let destination, capacity == fixture.byte_count else { return false }
    let pixelsPerLayer = Int(fixture.width * fixture.height)
    let pixels = destination.bindMemory(to: UInt32.self, capacity: pixelsPerLayer * 4)
    for layer in 0..<4 {
        let value = UInt32(0xff000000) | UInt32(fixture.sequence & 0xffff) << 8 | UInt32(layer)
        pixels.advanced(by: layer * pixelsPerLayer).initialize(repeating: value, count: pixelsPerLayer)
    }
    return true
}

private func setFixture(sequence: UInt64, width: Int32 = 2, height: Int32 = 2) {
    fixture.sequence = sequence
    fixture.source_epoch = sequence * 10
    fixture.flat_sequence = sequence
    fixture.status = 1; fixture.failure_reason = 0
    fixture.width = width
    fixture.height = height
    fixture.byte_count = Int(width * height) * 16
    fixture.projection_x = (1.01 + Float(sequence) / 1000, 1.02, 1.03)
    fixture.projection_y = (1.11, 1.12, 1.13)
}

private func identities(_ snapshot: EnginePanoramaTexture.Snapshot) -> [ObjectIdentifier] {
    snapshot.textures.map(ObjectIdentifier.init)
}

private func expected(sequence: UInt64, layer: Int) -> UInt32 {
    UInt32(0xff000000) | UInt32(sequence & 0xffff) << 8 | UInt32(layer)
}

private func validatePixels(_ snapshot: EnginePanoramaTexture.Snapshot, sequence: UInt64) {
    precondition(snapshot.textures.count == 4 && snapshot.sequence == sequence)
    precondition(snapshot.sourceEpoch == sequence * 10 && snapshot.flatSequence == sequence)
    for layer in 0..<4 {
        let texture = snapshot.textures[layer]
        var pixels = [UInt32](repeating: 0, count: texture.width * texture.height)
        texture.getBytes(&pixels, bytesPerRow: texture.width * 4,
                         from: MTLRegionMake2D(0, 0, texture.width, texture.height), mipmapLevel: 0)
        precondition(pixels.allSatisfy { $0 == expected(sequence: sequence, layer: layer) },
                     "layer \(layer) was not from coherent source sequence \(sequence)")
    }
}

@main
enum PanoramaTextureValidation {
static func main() {
guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue() else {
    fatalError("Metal device unavailable")
}
let panorama = EnginePanoramaTexture(device: device)

setFixture(sequence: 1)
let firstCommand = queue.makeCommandBuffer()!
let first = panorama.refresh(leasedTo: firstCommand)!
validatePixels(first, sequence: 1)
let firstIDs = identities(first)
precondition(identities(panorama.refresh()!) == firstIDs, "duplicate sequence replaced its slot")

// Encode a GPU read, then advance two source frames without committing it.
// A later refresh must not replace the sampled texture until completion.
let output = device.makeBuffer(length: 512, options: .storageModeShared)!
let blit = firstCommand.makeBlitCommandEncoder()!
blit.copy(from: first.textures[0], sourceSlice: 0, sourceLevel: 0,
          sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
          sourceSize: MTLSize(width: 2, height: 2, depth: 1),
          to: output, destinationOffset: 0, destinationBytesPerRow: 256,
          destinationBytesPerImage: 512)
blit.endEncoding()

setFixture(sequence: 2)
let secondCommand = queue.makeCommandBuffer()!
let second = panorama.refresh(leasedTo: secondCommand)!
setFixture(sequence: 3)
let thirdCommand = queue.makeCommandBuffer()!
let third = panorama.refresh(leasedTo: thirdCommand)!
precondition(identities(second) != firstIDs && identities(third) != firstIDs)

setFixture(sequence: 4)
let blockedCommand = queue.makeCommandBuffer()!
let blocked = panorama.refresh(leasedTo: blockedCommand)!
precondition(blocked.sequence == 3 && identities(blocked) == identities(third),
             "a fourth slot was allocated while all three pool slots were protected")

firstCommand.commit()
firstCommand.waitUntilCompleted()
precondition(firstCommand.status == .completed)
let gpuPixels = output.contents().bindMemory(to: UInt32.self, capacity: 128)
precondition(gpuPixels[0] == expected(sequence: 1, layer: 0) &&
             gpuPixels[64] == expected(sequence: 1, layer: 0),
             "an in-flight texture was overwritten")

let reusedCommand = queue.makeCommandBuffer()!
let reused = panorama.refresh(leasedTo: reusedCommand)!
precondition(reused.sequence == 4 && identities(reused) == firstIDs,
             "completed slot was not recycled for the pending source sequence")
validatePixels(reused, sequence: 4)

for command in [secondCommand, thirdCommand, blockedCommand, reusedCommand] {
    command.commit()
    command.waitUntilCompleted()
    precondition(command.status == .completed)
}

// A transient copy failure preserves the last complete snapshot. Explicit
// source unavailability clears it so the renderer can resume its live 2D path.
setFixture(sequence: 5)
copyAvailable = false
let preserved = panorama.refresh()!
precondition(preserved.sequence == 4 && identities(preserved) == identities(reused))
copyAvailable = true
infoAvailable = false
fixture.status = 2; fixture.failure_reason = 2
let heldWorld = panorama.refresh()!
precondition(heldWorld.sequence == 4 && heldWorld.sourceEpoch == 40)
precondition(!panorama.allowsFlatFallback && panorama.latestSourceEpoch == 50 && panorama.failureReason == 2)
validatePixels(heldWorld, sequence: 4)
fixture.status = 0; fixture.source_epoch = 0
precondition(panorama.refresh() == nil)
precondition(panorama.allowsFlatFallback)
// With no previous complete world, a failed pass must not become a flat world.
fixture.status = 2
precondition(panorama.refresh() == nil && !panorama.allowsFlatFallback)
infoAvailable = true

// A resolution change reallocates one idle slot, publishes all four resized
// layers together, and remains recoverable after invalid metadata.
setFixture(sequence: 5, width: 3, height: 1)
let resized = panorama.refresh()!
precondition(resized.textures.allSatisfy { $0.width == 3 && $0.height == 1 })
validatePixels(resized, sequence: 5)
fixture.byte_count -= 1
precondition(panorama.refresh()?.sequence == 5 && !panorama.allowsFlatFallback)
setFixture(sequence: 6, width: 2, height: 3)
let recovered = panorama.refresh()!
validatePixels(recovered, sequence: 6)

print("PANORAMA_TEXTURE_POOL_PASS slots=3 coherentLayers=4 inFlightProtected=true completedReused=true")
print("PANORAMA_TEXTURE_FAILURE_PASS copyPreserved=true incompleteWorldRetained=true flatMenuCleared=true firstWorldFailureBlank=true resize=3x1 recovery=2x3")
}
}
