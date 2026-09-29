import Foundation
import Metal

struct EngineVisionFrameInfo {
    var width: UInt32 = 2
    var height: UInt32 = 2
    var byte_count: UInt64 = 16
    var sequence: UInt64 = 1
}
var fixture = EngineVisionFrameInfo()
var fixturePixel: UInt32 = 0xff102030
func enginevision_frame_info(_ info: inout EngineVisionFrameInfo) -> Bool { info = fixture; return true }
func enginevision_copy_latest_frame(_ target: UnsafeMutableRawPointer?, _ count: Int, _ info: inout EngineVisionFrameInfo) -> Bool {
    guard count == 16, let target else { return false }
    info = fixture
    target.bindMemory(to: UInt32.self, capacity: 4).initialize(repeating: fixturePixel, count: 4)
    return true
}
let device = MTLCreateSystemDefaultDevice()!
let frame = EngineFrameTexture(device: device)
precondition(frame.refresh())
let old = frame.texture!
let command = device.makeCommandQueue()!.makeCommandBuffer()!
let output = device.makeBuffer(length: 512, options: .storageModeShared)!
let blit = command.makeBlitCommandEncoder()!
blit.copy(from: old, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x:0,y:0,z:0),
          sourceSize: MTLSize(width:2,height:2,depth:1), to: output, destinationOffset:0, destinationBytesPerRow:256, destinationBytesPerImage:512)
blit.endEncoding()
// Refresh between encoding a GPU read and executing it, reproducing the
// exact ownership hazard without relying on thread scheduling or timing.
fixture.sequence = 2; fixturePixel = 0xffaabbcc
precondition(frame.refresh())
precondition(old !== frame.texture, "frame refresh mutated an in-flight texture")
command.commit(); command.waitUntilCompleted()
precondition(command.status == .completed)
let pixels = output.contents().bindMemory(to: UInt32.self, capacity: 128)
precondition(pixels[0] == 0xff102030 && pixels[64] == 0xff102030, "GPU read a later frame")
var latest = [UInt32](repeating: 0, count: 4)
frame.texture!.getBytes(&latest, bytesPerRow: 8, from: MTLRegionMake2D(0,0,2,2), mipmapLevel: 0)
precondition(latest.allSatisfy { $0 == 0xffaabbcc })
let same = frame.texture!
precondition(!frame.refresh() && same === frame.texture, "duplicate sequence reallocated")
print("IMMUTABLE_GPU_FRAME_PASS old snapshot preserved across pending GPU read; new snapshot correct; duplicate reused")
