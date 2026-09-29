import CoreHaptics
import GameController
import Foundation

/// Plays controller haptics for the impacts the engine's audio implies.
///
/// Halo has no force-feedback code of its own, so there is no rumble intent
/// to forward. The host synthesises events from low-frequency transients in
/// the mixed audio (native/EngineHost/haptics.c) and this plays them.
///
/// Playing them is the host's job, not this type's. The host already holds a
/// live controller for input, so asking it to buzz that same pad cannot
/// disagree about whether a controller exists. A separate lookup here did
/// disagree on the device: fourteen impacts detected, none played, and no
/// controller seen, while the game was taking input from one.
final class EngineHaptics {
    /// Reported in the device diagnostics: with the headset out of reach
    /// there is nothing else to debug from.
    private static let diagnosticsLock = NSLock()
    private static var takenCount = 0
    static var state: String {
        halo_haptics_enabled() == 0 ? "disabled" : String(cString: hostgc_haptic_state())
    }
    static var eventsTaken: Int {
        diagnosticsLock.lock()
        defer { diagnosticsLock.unlock() }
        return takenCount
    }
    static var eventsPlayed: Int { Int(hostgc_haptics_played()) }

    init() {}

    /// Fires one pulse directly, skipping detection entirely. With the headset
    /// out of reach this is the only way to tell a detector that never fires
    /// from a controller that cannot buzz.
    static func testPulse() {
        hostgc_play_haptic(1, 0.5)
    }

    /// Called once per rendered frame on the render thread. Cheap when idle.
    ///
    /// The pulse goes to the host, which plays it on the very controller it
    /// is already reading for input. Looking the controller up separately
    /// from here is what used to fail: the device reported fourteen impacts
    /// detected, none played, and no controller seen, while the game was
    /// taking input from one.
    func pump() {
        guard halo_haptics_enabled() != 0 else { return }
        var intensity: Float = 0, sharpness: Float = 0
        guard halo_haptics_take(&intensity, &sharpness) != 0 else { return }
        EngineHaptics.diagnosticsLock.lock()
        EngineHaptics.takenCount += 1
        EngineHaptics.diagnosticsLock.unlock()
        hostgc_play_haptic(min(1, max(0, intensity)), min(1, max(0, sharpness)))
    }

}
