import Foundation

@main struct DeepTelemetryValidation {
    static func main() throws {
        let telemetry = EngineDeepTelemetry()
        var input: [String: Any] = ["elapsedSeconds": 10.0, "frameSequence": UInt64(100),
            "submittedFrames": UInt64(900), "audioFrames": UInt64(48000), "audioNonzeroSamples": UInt64(1000),
            "gameplay": true, "audioSuspended": false, "audioVoicesPlaying": 3,
            "controllerDisconnects": 0, "layerEpochs": [UInt64(10), 10, 0], "sceneEpoch": UInt64(1)]
        let initial = telemetry.sample(input)
        precondition(initial["engineFPS"] as? Double == 0)
        input["elapsedSeconds"] = 10.2; input["frameSequence"] = UInt64(106)
        input["submittedFrames"] = UInt64(918); input["audioFrames"] = UInt64(57600)
        input["controllerDisconnects"] = 2
        var record = telemetry.sample(input)
        precondition(abs((record["engineFPS"] as! Double) - 30) < 0.001)
        precondition((record["events"] as! [String]).contains("controller.disconnect"))
        precondition((record["eventCounts"] as! [String: Int])["controller.disconnect"] == 2)
        precondition((record["activeSignals"] as! [String]).contains("audio.zeroMixWithVoices"))
        input["elapsedSeconds"] = 10.6
        record = telemetry.sample(input)
        precondition((record["events"] as! [String]).contains("audio.noCallbackProgress.begin"))
        precondition((record["events"] as! [String]).contains("panorama.staleLayer.begin"))
        precondition((record["layerObservedAgeMilliseconds"] as! [Double])[2] == -1)
        // A scene switch resets observed ages; a deliberate pause is not an audio failure.
        input["elapsedSeconds"] = 11.0; input["sceneEpoch"] = UInt64(2); input["gameplay"] = false
        record = telemetry.sample(input)
        precondition((record["activeSignals"] as! [String]).isEmpty)
        precondition((record["layerObservedAgeMilliseconds"] as! [Double])[0] == 0)
        // Counter rollback must not underflow and observer delays remain explicit.
        input["elapsedSeconds"] = 15.0; input["frameSequence"] = UInt64(1)
        record = telemetry.sample(input)
        precondition(record["engineFPS"] as? Double == 0)
        precondition((record["events"] as! [String]).contains("observer.delayed.begin"))
        _ = try JSONSerialization.data(withJSONObject: record)
        print("PASS deep telemetry: measured intervals, fault/recovery transitions, short disconnects, layer reset, missing layers, counter reset")
    }
}
