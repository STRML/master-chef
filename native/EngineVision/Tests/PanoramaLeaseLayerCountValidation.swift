// Run on a Mac. This covers the zero-copy lease boundary, which only the
// headset exercises: the runtime hands Swift a C array of layer textures and
// the compositor rejects the frame unless it unpacks every one of them. That
// unpack once named the tuple members by hand, so when the sphere grew from
// seven layers to ten it kept the first seven, every frame failed the count
// check, and the headset showed black while the Mac composited perfectly.
//
//   xcrun swiftc -parse-as-library \
//     -import-objc-header native/EngineVision/Sources/EngineVisionBridge.h \
//     -I native/EngineHost \
//     native/EngineVision/Sources/EnginePanoramaLayerArray.swift \
//     native/EngineVision/Tests/PanoramaLeaseLayerCountValidation.swift \
//     -o native/build/panorama-lease-tests && native/build/panorama-lease-tests
import Foundation

@main struct PanoramaLeaseLayerCountValidation {
static func main() {
    let expected = Int(HALO_PANORAMA_LAYERS)
    var snapshot = EngineVisionPanoramaGPUSnapshot()

    // The tuple the header actually produces has to be as long as the header
    // says, or the runtime and the compositor disagree about the sphere.
    let slotCount = MemoryLayout.size(ofValue: snapshot.textures)
        / MemoryLayout<UnsafeMutableRawPointer?>.stride
    precondition(slotCount == expected,
        "lease carries \(slotCount) layer slots, header declares \(expected)")

    // Distinct ordered sentinels, so a short read, a repeat or a swap all show.
    withUnsafeMutableBytes(of: &snapshot.textures) { raw in
        let slots = raw.bindMemory(to: UnsafeMutableRawPointer?.self)
        for k in 0..<slots.count { slots[k] = UnsafeMutableRawPointer(bitPattern: k + 1) }
    }

    let pointers = EnginePanoramaLayerArray.pointers(of: snapshot.textures)
    precondition(pointers.count == expected,
        "unpacked \(pointers.count) layers of \(expected); the compositor would reject every frame")
    for k in 0..<expected {
        precondition(pointers[k] == UnsafeMutableRawPointer(bitPattern: k + 1),
            "layer \(k) came back as \(String(describing: pointers[k])), out of order")
    }
    precondition(!pointers.contains(nil), "a layer came back null")

    print("panorama lease unpack: \(expected) layers, in order")
}
}
