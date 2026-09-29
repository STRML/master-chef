import Foundation

/// A sampled health journal, never a diagnosis of physical output. Runs on
/// the main actor's existing 200 ms timer; records actual intervals, including
/// observer stalls. Counter deltas retain short faults between samples.
final class EngineDeepTelemetry {
    private var previous: [String: Any]?
    private var layerEpochs: [UInt64] = []
    private var layerChanged: [Double] = []
    private var scene: UInt64?
    private var activeFaults = Set<String>()
    private(set) var eventCounts: [String: Int] = [:]
    private(set) var records: UInt64 = 0

    func sample(_ input: [String: Any]) -> [String: Any] {
        var record = input
        let now = number(input, "elapsedSeconds")
        let dt = previous.map { max(0, now - number($0, "elapsedSeconds")) } ?? 0
        func delta(_ key: String) -> Double {
            guard let previous else { return 0 }
            return max(0, number(input, key) - number(previous, key))
        }
        var events: [String] = []
        var faults = Set<String>()
        func fault(_ name: String, _ present: Bool) { if present { faults.insert(name) } }
        let gameplay = input["gameplay"] as? Bool == true
        record["intervalSeconds"] = dt
        record["engineFPS"] = dt > 0 ? delta("frameSequence") / dt : 0.0
        record["submittedFPS"] = dt > 0 ? delta("submittedFrames") / dt : 0.0
        record["audioFramesPerSecond"] = dt > 0 ? delta("audioFrames") / dt : 0.0
        record["audioNonzeroSamplesDelta"] = delta("audioNonzeroSamples")
        record["gamepadReadsDelta"] = delta("gamepadReads")
        record["hapticsPlayedDelta"] = delta("hapticsPlayed")
        record["hapticsTakenDelta"] = delta("hapticsTaken")
        for (key, event) in [("controllerDisconnects", "controller.disconnect"),
                             ("controllerConnects", "controller.connect"),
                             ("audioGeneration", "audio.queueRebuilt"),
                             ("audioEnqueueFailures", "audio.enqueueFailure"),
                             ("audioLateCallbacks", "audio.callbackGap"),
                             ("gpuFailures", "gpu.failure"),
                             ("hapticFailures", "haptics.failure"),
                             ("hapticResets", "haptics.reset"),
                             ("hapticStops", "haptics.engineStopped")] {
            let change = delta(key)
            record[key + "Delta"] = change
            if change > 0 { events.append(event); eventCounts[event, default: 0] += Int(change) }
        }
        let epochs = input["layerEpochs"] as? [UInt64] ?? []
        let newScene = (input["sceneEpoch"] as? NSNumber)?.uint64Value ?? 0
        if scene != newScene || epochs.count != layerEpochs.count {
            layerChanged = Array(repeating: now, count: epochs.count)
            layerEpochs = epochs
            if scene != nil { events.append("panorama.sceneChanged") }
            scene = newScene
        }
        var ages: [Double] = []
        for i in epochs.indices {
            if epochs[i] != layerEpochs[i] { layerChanged[i] = now }
            // -1 means never published, not a fresh image. An epoch's age
            // is a lower bound from observation, not its render timestamp.
            ages.append(epochs[i] == 0 ? -1 : max(0, now - layerChanged[i]) * 1000)
        }
        layerEpochs = epochs
        record["layerObservedAgeMilliseconds"] = ages
        let maxEpoch = epochs.max() ?? 0
        record["layerEpochLag"] = epochs.map { $0 == 0 ? NSNull() as Any : NSNumber(value: maxEpoch - $0) }
        record["missingLayerCount"] = epochs.filter { $0 == 0 }.count
        if previous != nil && dt > 0 {
            fault("observer.delayed", dt > 0.6)
            fault("engine.noProgress", gameplay && delta("frameSequence") == 0)
            fault("presenter.noProgress", gameplay && delta("submittedFrames") == 0)
            let playing = gameplay && input["audioSuspended"] as? Bool == false && number(input, "audioVoicesPlaying") > 0
            fault("audio.noCallbackProgress", playing && delta("audioFrames") == 0)
            fault("audio.zeroMixWithVoices", playing && delta("audioFrames") > 0 && delta("audioNonzeroSamples") == 0)
            fault("panorama.staleLayer", gameplay && ages.contains { $0 > 250 })
            fault("panorama.missingLayer", gameplay && epochs.contains(0))
        }
        for fault in faults.subtracting(activeFaults).sorted() {
            events.append(fault + ".begin"); eventCounts[fault, default: 0] += 1
        }
        for fault in activeFaults.subtracting(faults).sorted() { events.append(fault + ".end") }
        activeFaults = faults
        records += 1
        record["schema"] = 1
        record["recordIndex"] = records
        record["events"] = events
        record["activeSignals"] = faults.sorted()
        record["eventCounts"] = eventCounts
        record["scope"] = "Sampled engine evidence; stale-layer and silence signals are not proof of visible seams or audible failure. Controller polls are not Bluetooth packets."
        previous = input
        return record
    }

    private func number(_ record: [String: Any], _ key: String) -> Double {
        (record[key] as? NSNumber)?.doubleValue ?? 0
    }
}
