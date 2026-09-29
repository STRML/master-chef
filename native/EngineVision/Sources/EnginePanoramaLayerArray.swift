import Foundation

/// The layer textures a zero-copy lease carries, in layer order.
///
/// C declares them as `void *textures[HALO_PANORAMA_LAYERS]`, which Swift
/// imports as a tuple with no count and no way to iterate it. Naming the
/// members one at a time is what blacked out the headset when the sphere grew
/// from seven layers to ten: the lease kept the first seven, every frame
/// failed the count check that follows it, and the compositor was left with
/// nothing to present. Walking the memory cannot fall behind the header.
enum EnginePanoramaLayerArray {
    static func pointers<Tuple>(of tuple: Tuple) -> [UnsafeMutableRawPointer?] {
        withUnsafeBytes(of: tuple) { Array($0.bindMemory(to: UnsafeMutableRawPointer?.self)) }
    }

    /// The same, for the per-layer float arrays the panorama info carries.
    /// Diagnostics reported the first three of each for the same reason, and
    /// so described a ten-layer sphere as a three-view one.
    static func floats<Tuple>(of tuple: Tuple) -> [Float] {
        withUnsafeBytes(of: tuple) { Array($0.bindMemory(to: Float.self)) }
    }
}
