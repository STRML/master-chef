import SwiftUI

/// Live controls for the port's presentation features.
///
/// These are the port's own settings rather than Halo's: the original game
/// has no notion of a second eye, of how much sky its camera should cover, or
/// of a controller that can be felt. Every value starts from its environment
/// variable, so scripted runs behave as before, and every change takes effect
/// on the next frame without restarting the engine.
@MainActor
final class EngineSettingsModel: ObservableObject {
    /// Halo's world unit is ten feet, so millimetres convert through 3.048.
    private static let metresPerWorldUnit = 3.048

    @Published var stereo: Bool { didSet { push() } }
    @Published var separationMillimetres: Double { didSet { push() } }
    @Published var verticalFieldDegrees: Double { didSet { push() } }
    @Published var hapticStrength: Double { didSet { push() } }
    @Published var surroundBrightness: Double { didSet { push() } }
    @Published var spatialMenu: Bool { didSet { push() } }
    @Published var frameRateTarget: Double { didSet { push() } }
    @Published var ownWeaponDecibels: Double { didSet { push() } }
    @Published var gazePointer: Bool { didSet { push() } }
    @Published var layerAlign: Bool { didSet { push() } }
    @Published var framePacing: Bool { didSet { push() } }

    init() {
        var current = HaloSettings()
        halo_settings_get(&current)
        let separation = Double(current.stereo_separation)
        stereo = separation > 0
        separationMillimetres = separation > 0
            ? separation * 2 * Self.metresPerWorldUnit * 1000 : 63
        verticalFieldDegrees = Double(current.panorama_vfov) * 180 / .pi
        hapticStrength = Double(current.haptics_strength)
        surroundBrightness = Double(current.backdrop_brightness)
        spatialMenu = current.spatial_shell != 0
        frameRateTarget = Double(current.panorama_target_fps)
        ownWeaponDecibels = Double(current.self_gain_db)
        gazePointer = current.gaze_pointer != 0
        layerAlign = current.layer_align != 0
        framePacing = current.frame_pacing != 0
    }

    private func push() {
        var settings = HaloSettings()
        settings.stereo_separation = stereo
            ? Float(separationMillimetres / 1000 / 2 / Self.metresPerWorldUnit) : 0
        settings.panorama_vfov = Float(verticalFieldDegrees * .pi / 180)
        settings.haptics_strength = Float(hapticStrength)
        settings.backdrop_brightness = Float(surroundBrightness)
        settings.spatial_shell = spatialMenu ? 1 : 0
        settings.panorama_target_fps = Float(frameRateTarget)
        settings.self_gain_db = Float(ownWeaponDecibels)
        settings.gaze_pointer = gazePointer ? 1 : 0
        settings.layer_align = layerAlign ? 1 : 0
        settings.frame_pacing = framePacing ? 1 : 0
        halo_settings_set(&settings)
    }
}

struct EngineSettingsView: View {
    @StateObject private var model = EngineSettingsModel()
    /// The haptics report is the only way to tell, from inside the headset,
    /// whether the controller was found, whether impacts are being detected,
    /// and whether anything reached the pad.
    @State private var hapticsReport = EngineHaptics.state
    /// Stored, not live: the engine fixes its back buffer when it creates the
    /// device, so this takes effect at the next launch. Read in
    /// enginevision_start; HALO_VIDMODE in the environment still wins.
    @AppStorage("HaloRenderResolution") private var renderResolution = "2048x1536"
    private let renderResolutions = ["1280x960", "1600x1200", "1920x1440", "2048x1536"]
    @State private var hapticsCounts = (taken: 0, played: 0)
    private let tick = Timer.publish(every: 0.5, on: .main, in: .common).autoconnect()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 26) {
                header
                section("Depth", note: stereoNote) {
                    Toggle("Render both eyes", isOn: $model.stereo)
                    slider("Eye separation", value: $model.separationMillimetres,
                           range: 45...80, step: 1, format: "%.0f mm")
                        .disabled(!model.stereo)
                }
                section("Field of view", note: fieldNote) {
                    slider("Vertical coverage", value: $model.verticalFieldDegrees,
                           range: 90...175, step: 5, format: "%.0f°")
                }
                section("Joins", note: "Test setting, off by default. When it is on, views drawn a frame or more before the newest are turned to meet it, so the world lines up across the joins while you turn. Side effects while you turn: the picture can bend at eye level in the views either side of the centre, and some joins can show a sharper edge.") {
                    Toggle("Line up older views", isOn: $model.layerAlign)
                }
                section("Frame rate", note: frameRateNote) {
                    slider("Keep at least", value: $model.frameRateTarget,
                           range: 20...30, step: 1, format: "%.0f fps")
                    Toggle("Even frame cadence", isOn: $model.framePacing)
                    Text(pacingNote).font(.footnote).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                section("Surround", note: "How brightly the space beyond the picture continues its edges.") {
                    slider("Brightness", value: $model.surroundBrightness,
                           range: 0...2, step: 0.05, format: "%.2f×")
                    Toggle("Spatial front-end menu", isOn: $model.spatialMenu)
                }
                section("Menus", note: "Menus are chosen with your eyes: the row you look at lights up, and a pinch chooses it. With this on, Halo's cursor also follows your head between pinches. In play neither does anything.") {
                    Toggle("Head pointer", isOn: $model.gazePointer)
                }
                section("Sound", note: "Halo hands your own weapon, reload and melee to the mixer twenty decibels quieter than the world around you; this lifts them back.") {
                    slider("Your own weapon", value: $model.ownWeaponDecibels,
                           range: 0...24, step: 1, format: "+%.0f dB")
                }
                section("Haptics", note: "Halo has no rumble of its own, so impacts are read from the game's audio.") {
                    slider("Strength", value: $model.hapticStrength,
                           range: 0...2, step: 0.05, format: "%.2f×")
                    HStack {
                        Button("Test pulse") { EngineHaptics.testPulse() }
                        Spacer()
                        Text("\(hapticsCounts.taken) detected · \(hapticsCounts.played) played")
                            .font(.footnote).monospacedDigit().foregroundStyle(.secondary)
                    }
                    Text(hapticsReport).font(.footnote).foregroundStyle(.secondary)
                }
                Picker("Render resolution (next launch)", selection: $renderResolution) {
                    ForEach(renderResolutions, id: \.self) { Text($0.replacingOccurrences(of: "x", with: " × ")) }
                }
                Text("Takes effect when Halo is next launched: the engine fixes its back buffer when it starts. Lower is faster if the headset is spending its time on pixels; the diagnostics report says whether it is.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
            .padding(34)
            .frame(maxWidth: 560, alignment: .leading)
        }
        .onReceive(tick) { _ in
            hapticsReport = EngineHaptics.state
            hapticsCounts = (EngineHaptics.eventsTaken, EngineHaptics.eventsPlayed)
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Presentation").font(.largeTitle.weight(.semibold))
            Text("Changes apply to the next rendered frame.")
                .font(.callout).foregroundStyle(.secondary)
        }
    }

    private var stereoNote: String {
        model.stereo
            ? "Each of the three views is rendered from both eyes, which doubles the world passes."
            : "One eye is rendered and shown to both, which halves the world passes."
    }
    private var fieldNote: String {
        "How much sky and floor each view covers. Widening it spends the same pixels over more angle rather than rendering more, so it trades sharpness for coverage."
    }
    private var pacingNote: String {
        model.framePacing
            ? "Spaces new pictures evenly when the scene has enough time to keep pace. It can lower the frame rate slightly, and while it paces, the pace rather than Keep at least sets how much of the sphere behind and beside you is redrawn, so that can refresh less often. Slower scenes run as soon as each picture is ready."
            : "Shows new pictures as soon as they are ready. The time each picture stays on screen can vary."
    }
    private var frameRateNote: String {
        "The view ahead and both eyes are drawn every frame regardless. What is left of each frame under this rate goes to redrawing the sphere behind and beside you; lower it to refresh those more often, raise it for smoother motion. Halo itself stops at 30."
    }

    @ViewBuilder
    private func section<Content: View>(_ title: String, note: String,
                                        @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(title).font(.title3.weight(.medium))
            content()
            Text(note).font(.footnote).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        .padding(20)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
    }

    private func slider(_ label: String, value: Binding<Double>,
                        range: ClosedRange<Double>, step: Double, format: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(label)
                Spacer()
                Text(String(format: format, value.wrappedValue))
                    .monospacedDigit().foregroundStyle(.secondary)
            }
            Slider(value: value, in: range, step: step)
        }
    }
}
