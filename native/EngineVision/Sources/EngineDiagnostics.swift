import Foundation
import GameController
import CoreGraphics
import UIKit
import ImageIO
import UniformTypeIdentifiers

@MainActor
final class EngineDiagnostics {
    let runID = UUID().uuidString
    let started = Date()
    private let startedUptime = ProcessInfo.processInfo.systemUptime
    /// Owns report.json and the timeline; writes both off the main thread.
    private var writer: EngineDiagnosticReportWriter?
    private let deepTelemetry = EngineDeepTelemetry()
    private var lastDeepUptime = -Double.infinity
    private var latestDeep: [String: Any] = [:]
    private(set) var reportURL: URL?
    private(set) var controllerStatus = "Checking controller"
    private(set) var saveStatus = "Preparing test report"
    private var lastWriteUptime: TimeInterval?
    private var connectedEver = false
    private var seenButtons: UInt32 = 0
    private var minAxes = [Float](repeating: 0, count: 6)
    private var maxAxes = [Float](repeating: 0, count: 6)
    private var peakResident: UInt64 = 0
    private var peakPhysicalFootprint: UInt64 = 0
    private var peakMetalAllocated: UInt64 = 0
    private var lastSequence: UInt64 = 0
    private var lastSampleUptime = ProcessInfo.processInfo.systemUptime
    private var logError: Int32 = 0
    private var capturedFrames: [[String: Any]] = []
    private var lastFrameCapture = Date.distantPast
    // The latest sample's arguments, for a report asked for between samples.
    private var lastState = Int32(ENGINEVISION_IDLE)
    private var lastStatus = ""
    private var lastFrame = EngineVisionFrameInfo()
    private var terminationObserver: NSObjectProtocol?
    var preparationStatus = "Checking bundled game data"
    var preparationCopiedBytes: Int64 = 0
    var preparationTotalBytes: Int64 = 0
    var immersiveStatus = "Opening immersive Halo"
    var immersiveActive = false
    var immersiveLayerState: UInt32?
    var immersiveSubmittedFrames: UInt64 = 0
    var immersiveCancelledFrames: UInt64 = 0
    var immersiveTrackingLossFrames: UInt64 = 0
    var immersiveGPUCompletedFrames: UInt64 = 0
    var immersiveGPUFailedFrames: UInt64 = 0
    var immersiveGPUError = ""
    /// GPU milliseconds per compositor frame (running average, worst). 11.1
    /// is the whole budget at 90 Hz; over it the display halves the rate.
    var immersiveGPUMilliseconds: Double = 0
    var immersiveGPUMillisecondsMax: Double = 0
    var immersiveConfiguration = "pending"
    var immersiveSubmission = EngineImmersiveSubmission()

    init() {
        do {
            let documents = try FileManager.default.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            let directory = documents.appending(path: "Diagnostics/\(runID)", directoryHint: .isDirectory)
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let url = directory.appending(path: "report.json")
            reportURL = url
            writer = EngineDiagnosticReportWriter(reportURL: url, history: EngineDiagnosticHistory(directory: directory))
            logError = directory.appending(path: "host.log").path.withCString { enginevision_capture_host_log($0) }
            saveStatus = logError == 0 ? "Test report saves automatically" : "Report saves; engine log error \(logError)"
        } catch { saveStatus = "Could not save test report: \(error.localizedDescription)" }
        terminationObserver = NotificationCenter.default.addObserver(forName: UIApplication.willTerminateNotification,
                                                                     object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.flush(reason: "willTerminate", wait: true) }
        }
    }

    /// Writes the report now, from the latest sample, rather than at the next
    /// period: on scene-phase changes and at exit, the moments after which
    /// the app may not run again. `wait` returns only once it is on disk.
    func flush(reason: String, wait: Bool = false) {
        guard let writer else { return }
        var sample = EngineDiagnosticSample()
        enginevision_diagnostic_sample(&sample)
        let uptime = ProcessInfo.processInfo.systemUptime
        writer.writeReport(report(state: lastState, status: lastStatus, frame: lastFrame, sample: sample, now: Date()),
                           uptime: uptime, state: lastState, reason: reason)
        if wait { writer.drain() }
    }

    /// The controller link: connects, drops, drops that came within ten
    /// seconds of a haptic pulse, haptics suspensions and pulses the guard held
    /// back (gamecontroller_guard.h), and the pad's battery, which a weak link
    /// can also come down to.
    static func controllerLink() -> [String: Any] {
        var link = HostGCLinkStats()
        hostgc_link_stats(&link)
        let pad = GCController.current ?? GCController.controllers().first
        return [
            "connects": link.connects, "disconnects": link.disconnects,
            "hapticFailures": link.haptic_failures, "hapticResets": link.haptic_resets, "hapticStops": link.haptic_stops,
            "lastConnectUptimeNs": link.last_connect_ns, "lastDisconnectUptimeNs": link.last_disconnect_ns,
            "lastPlayedUptimeNs": link.last_played_ns,
            "disconnectsAfterPulses": link.linked_disconnects, "hapticsSuspensions": link.suspensions,
            "hapticsSuspendedMsLeft": link.suspended_ms_left, "guard": link.guard_enabled != 0,
            "pulsesPlayed": link.pulses_admitted, "pulsesHeldRate": link.pulses_dropped_rate,
            "pulsesHeldGrace": link.pulses_dropped_grace, "pulsesHeldSuspended": link.pulses_dropped_suspended,
            "pulsesHeldDisconnected": link.pulses_dropped_disconnected,
            "batteryLevel": pad?.battery.map { Double($0.batteryLevel) } ?? -1,
            "batteryState": pad?.battery.map { $0.batteryState.rawValue } ?? -1,
        ]
    }

    private static func framePacingReport() -> [String: Any] {
        // The frame pacer (frame_pacer.h):
        // which cadence it chose, how often a frame missed its slot, and
        // what it waited. Judge evenness against worldSubmissionCadence.
        var pacer = HaloFramePacerReport()
        host_frame_pacer_report(&pacer)
        return [
            "mode": pacer.mode, "rung": pacer.rung,
            "cadenceHz": pacer.rung > 0 && pacer.display_period_ms > 0 ? 1000 / (Double(pacer.rung) * Double(pacer.display_period_ms)) : 0,
            "latchLocked": pacer.latch_locked != 0, "displayPeriodMilliseconds": Double(pacer.display_period_ms),
            "observedDisplayPeriodMilliseconds": Double(pacer.observed_display_period_ms),
            "targetPeriodMilliseconds": Double(pacer.target_period_ms),
            "leadMilliseconds": Double(pacer.lead_ms), "publishLagMilliseconds": Double(pacer.publish_lag_ms),
            "workMilliseconds": Double(pacer.work_ms), "floorWorkMilliseconds": Double(pacer.floor_work_ms),
            "budgetTargetMilliseconds": Double(pacer.budget_target_ms), "lateShare": Double(pacer.late_share),
            "frames": pacer.frames, "pacedFrames": pacer.paced_frames, "lateFrames": pacer.late_frames,
            "freeFrames": pacer.free_frames, "rungChanges": pacer.rung_changes,
            "framesByRung": withUnsafeBytes(of: pacer.rung_frames) { Array($0.bindMemory(to: UInt64.self)) },
            "waitSeconds": pacer.wait_seconds, "requestedWaitSeconds": pacer.requested_wait_seconds]
    }

    func sample(state: Int32, status: String, frame: EngineVisionFrameInfo) {
        var sample = EngineDiagnosticSample()
        enginevision_diagnostic_sample(&sample)
        connectedEver = connectedEver || sample.controller_connected != 0
        seenButtons |= sample.button_mask
        let axes: [Float] = withUnsafeBytes(of: sample.axes) { Array($0.bindMemory(to: Float.self)) }
        for i in 0..<6 { minAxes[i] = min(minAxes[i], axes[i]); maxAxes[i] = max(maxAxes[i], axes[i]) }
        peakResident = max(peakResident, sample.resident_bytes)
        peakPhysicalFootprint = max(peakPhysicalFootprint, sample.physical_footprint_bytes)
        peakMetalAllocated = max(peakMetalAllocated, sample.metal_current_allocated_bytes)
        controllerStatus = sample.controller_connected != 0 ? "Controller connected · \(sample.keyboard_events) menu events · \(sample.gamepad_reads) pad reads" : "Controller not connected"
        lastState = state; lastStatus = status; lastFrame = frame
        guard let writer, let reportURL else { return }
        if let failure = writer.stats.failure { saveStatus = "Report save failed: \(failure)" }
        let uptime = ProcessInfo.processInfo.systemUptime
        if uptime - lastDeepUptime >= 0.18 {
            lastDeepUptime = uptime
            recordDeep(sample, frame: frame, state: state, uptime: uptime, writer: writer)
        }
        if let lastWriteUptime, uptime - lastWriteUptime < 1 { return }
        let now = Date()
        let interval = max(0.001, uptime - lastSampleUptime)
        let fps = frame.sequence >= lastSequence ? Double(frame.sequence-lastSequence)/interval : 0
        lastSequence = frame.sequence; lastSampleUptime = uptime; lastWriteUptime = uptime
        var digest = EngineFrameDigest()
        enginevision_frame_digest(&digest)
        let selected = immersiveSubmission
        let submission = submissionRecord()
        if digest.nonblack_count > 0, capturedFrames.count < 4, now.timeIntervalSince(lastFrameCapture) >= 10 {
            captureEngineFrame(at: now, directory: reportURL.deletingLastPathComponent())
        }
        // The pool's own account of itself. Without it a frame the compositor
        // could not lease read exactly like a frame the engine never made.
        var pool = EngineVisionPanoramaGPUStats()
        enginevision_panorama_gpu_stats(&pool)
        // Where the engine thread's time goes. Cumulative; two reports a known
        // time apart give the split for the interval between them.
        var profile = EngineVisionDrawProfile()
        enginevision_draw_profile(&profile)
        // The compact journal of the whole run, one record a second. It is
        // diagnostic evidence, not a claim of audible/visible output or
        // completed missions; verify those against the run's playthrough.
        // Since Build76 it is the only copy of the one-second samples, so it
        // carries every field report.json's samples array did.
        var record: [String: Any] = [
            "schema": 1, "runID": runID, "build": Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "unknown",
            "buildID": Bundle.main.infoDictionary?["HaloBuildID"] as? String ?? "unknown",
            "elapsedSeconds": uptime - startedUptime, "intervalSeconds": interval,
            "state": state, "engineExitCode": enginevision_exit_code(), "menuActive": enginevision_menu_active() != 0,
            "frameSequence": frame.sequence, "enginePresentsPerSecond": fps,
            "immersiveActive": immersiveActive, "mode": selected.mode,
            "immersiveSubmittedFrames": immersiveSubmittedFrames, "immersiveGPUCompletedFrames": immersiveGPUCompletedFrames,
            "immersiveGPUFailedFrames": immersiveGPUFailedFrames, "immersiveGPUMilliseconds": immersiveGPUMilliseconds,
            "panoramaEpoch": selected.sourceEpoch, "panoramaProducerEpoch": selected.producerEpoch,
            "panoramaSceneEpoch": selected.sceneEpoch, "panoramaLayerEpochs": selected.layerEpochs,
            // Per layer (index = layer): how far its camera was from the
            // newest centre's, and how far it was turned to meet it (0 with
            // alignment off, the default: the offsets are still measured).
            // Sums over every engine frame, so consecutive records give means
            // and `enabled` says which setting each second ran with.
            "panoramaLayerAlign": immersiveSubmission.layerAlignment.dictionary,
            "worldSubmissionCadence": selected.worldCadence,
            "immersiveSubmission": submission,
            "guestHeap": Self.heapStats(sample),
            "framePacing": Self.framePacingReport(),
            "nativeGather": EngineGatherDiagnostics.fields(sample),
            "panoramaPublishSuperseded": pool.publish_superseded,
            "panoramaPoolPublished": pool.published, "panoramaPoolDropped": pool.dropped,
            "panoramaPoolPublishFailed": pool.publish_failed, "panoramaPoolCarryFailed": pool.carry_failed,
            "panoramaBudgetTier": profile.panorama_tier,
            "panoramaBudgetExtraHalf": profile.panorama_extra_half,
            "panoramaBusyMilliseconds": Double(profile.panorama_busy_seconds) * 1000,
            "radialFogEnabled": halo_settings_radial_fog() != 0,
            "enginePasses": profile.passes, "enginePassSeconds": Double(profile.pass_ns) / 1e9,
            "engineReadbackSeconds": Double(profile.readback_ns) / 1e9,
            "engineVertexSeconds": Double(profile.vertex_ns) / 1e9,
            "engineSubmitSeconds": Double(profile.submit_ns) / 1e9,
            "engineUploadSeconds": Double(profile.upload_ns) / 1e9,
            "engineSleepSeconds": Double(profile.sleep_ns) / 1e9, "engineSleepCalls": profile.sleep_calls,
            "engineYieldCalls": profile.yield_calls,
            "engineThreadCPUSeconds": Double(profile.engine_cpu_ns) / 1e9,
            "thermalState": ProcessInfo.processInfo.thermalState.rawValue,
            "engineProgramCompiles": profile.program_compiles,
            "engineProgramCompileSeconds": Double(profile.program_compile_ns) / 1e9,
            "engineProgramWaits": profile.program_waits, "engineProgramWaitSeconds": Double(profile.program_wait_ns) / 1e9,
            "engineProgramBackgroundPipelines": profile.program_background_pipelines,
            "engineProgramBackgroundFunctions": profile.program_background_functions,
            "engineProgramBackgroundSeconds": Double(profile.program_background_ns) / 1e9,
            "engineProgramPrewarmHits": profile.program_prewarm_hits, "engineProgramArchiveHits": profile.program_archive_hits,
            "engineProgramFunctionCompiles": profile.program_engine_functions,
            "audioFrames": sample.audio_frames, "audioNonzeroSamples": sample.audio_nonzero_samples,
            "audioPeak": sample.audio_peak, "audioQueueRunning": sample.audio_queue_running != 0,
            "audioQueueStatus": sample.audio_queue_status, "audioOutputVolume": sample.audio_output_volume,
            "audioQueueGeneration": sample.audio_queue_generation, "audioQueueSuspended": sample.audio_queue_suspended != 0,
            "audioQueueRestartRequired": sample.audio_queue_restart != 0, "audioQueueDeferredBuffers": sample.audio_queue_deferred,
            "audioLastPrepare": sample.audio_last_prepare, "audioLastStart": sample.audio_last_start, "audioLastEnqueue": sample.audio_last_enqueue,
            "audioLastPause": sample.audio_last_pause, "audioLastDispose": sample.audio_last_dispose, "audioLastRecovery": sample.audio_last_recovery,
            "hapticsEventsTaken": EngineHaptics.eventsTaken, "hapticsEventsPlayed": EngineHaptics.eventsPlayed,
            "controllerConnected": sample.controller_connected != 0, "gamepadReads": sample.gamepad_reads,
            "keyboardEvents": sample.keyboard_events,
            "residentBytes": sample.resident_bytes, "virtualBytes": sample.virtual_bytes,
            "physicalFootprintBytes": sample.physical_footprint_bytes,
            "physicalFootprintAvailable": (sample.memory_metric_flags & UInt32(ENGINE_DIAGNOSTIC_HAS_PHYSICAL_FOOTPRINT)) != 0,
            "metalCurrentAllocatedBytes": sample.metal_current_allocated_bytes,
            "metalCurrentAllocatedAvailable": (sample.memory_metric_flags & UInt32(ENGINE_DIAGNOSTIC_HAS_METAL_ALLOCATED_SIZE)) != 0,
            "frameSampleCount": digest.sample_count, "nonblackPixelSamples": digest.nonblack_count,
            "differingPixelSamples": digest.differing_count, "pixelSampleHash": String(digest.hash, radix: 16)
        ]
        record.merge(Self.voiceFields(sample)) { current, _ in current }
        record.merge(Self.hostWork(sample)) { current, _ in current }
        record.merge(Self.coreTelemetry()) { current, _ in current }
        writer.appendTimeline(record)
        if let reason = writer.reportDue(uptime: uptime, state: state) {
            writer.writeReport(report(state: state, status: status, frame: frame, sample: sample, now: now),
                               uptime: uptime, state: state, reason: reason)
        }
    }

    private func recordDeep(_ s: EngineDiagnosticSample, frame: EngineVisionFrameInfo, state: Int32,
                            uptime: Double, writer: EngineDiagnosticReportWriter) {
        var link = HostGCLinkStats(); hostgc_link_stats(&link)
        var profile = EngineVisionDrawProfile(); enginevision_draw_profile(&profile)
        let menu = enginevision_menu_active() != 0
        let selected = immersiveSubmission
        var record: [String: Any] = [
            "runID": runID, "build": Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "unknown",
            "buildID": Bundle.main.infoDictionary?["HaloBuildID"] as? String ?? "unknown",
            "updatedAtUnixSeconds": Date().timeIntervalSince1970,
            "elapsedSeconds": uptime - startedUptime, "state": state, "menuActive": menu,
            "gameplay": state == ENGINEVISION_RUNNING && immersiveActive && !menu && selected.mode == "panorama",
            "mode": selected.mode, "frameSequence": frame.sequence,
            "submittedFrames": immersiveSubmittedFrames, "gpuFailures": immersiveGPUFailedFrames,
            "gpuMilliseconds": immersiveGPUMilliseconds, "gpuMillisecondsMax": immersiveGPUMillisecondsMax,
            "sceneEpoch": selected.sceneEpoch, "layerEpochs": selected.layerEpochs,
            "layerAlignment": selected.layerAlignment.dictionary,
            "worldCadence": selected.worldCadence,
            "budgetTier": profile.panorama_tier, "extraHalf": profile.panorama_extra_half,
            "busyMilliseconds": Double(profile.panorama_busy_seconds) * 1000,
            "enginePasses": profile.passes, "enginePassNanoseconds": profile.pass_ns,
            "shaderWaits": profile.program_waits, "shaderWaitNanoseconds": profile.program_wait_ns,
            "audioFrames": s.audio_frames, "audioNonzeroSamples": s.audio_nonzero_samples,
            "audioVoicesPlaying": s.audio_voices_playing, "soundChannelsStarved": s.sound_channels_starved,
            "soundVoicesFree": s.sound_voices_free, "soundVoicesHeld": s.sound_voices_held,
            "audioGeneration": s.audio_queue_generation, "audioSuspended": s.audio_queue_suspended != 0,
            "audioQueueRunning": s.audio_queue_running != 0, "audioQueueStatus": s.audio_queue_status,
            "audioLastEnqueue": s.audio_last_enqueue, "audioLastRecovery": s.audio_last_recovery,
            "audioCallbacks": s.audio_callbacks, "audioLateCallbacks": s.audio_late_callbacks,
            "audioEnqueueFailures": s.audio_enqueue_failures,
            "audioCallbackAgeMilliseconds": Double(s.audio_callback_age_ns) / 1e6,
            "audioMaxCallbackGapMilliseconds": Double(s.audio_max_callback_gap_ns) / 1e6,
            "audioMaxCallbackWorkMilliseconds": Double(s.audio_max_callback_work_ns) / 1e6,
            "audioSampleRate": s.audio_sample_rate, "audioBufferFrames": s.audio_buffer_frames,
            "audioOutputVolume": s.audio_output_volume,
            "controllerConnected": s.controller_connected != 0,
            "controllerConnects": link.connects, "controllerDisconnects": link.disconnects,
            "controllerLastDisconnectUptimeNs": link.last_disconnect_ns,
            "controllerSequence": s.controller_sequence, "gamepadReads": s.gamepad_reads,
            "dinputAcquireLost": s.dinput_acquire_lost, "buttonMask": s.button_mask,
            "controllerAxes": withUnsafeBytes(of: s.axes) { Array($0.bindMemory(to: Float.self)) },
            "hapticsTaken": EngineHaptics.eventsTaken, "hapticsPlayed": EngineHaptics.eventsPlayed,
            "hapticsState": EngineHaptics.state, "hapticFailures": link.haptic_failures,
            "hapticResets": link.haptic_resets, "hapticStops": link.haptic_stops,
            "hapticsHeldRate": link.pulses_dropped_rate, "hapticsHeldGrace": link.pulses_dropped_grace,
            "hapticsHeldSuspended": link.pulses_dropped_suspended, "hapticsHeldDisconnected": link.pulses_dropped_disconnected,
            "thermalState": ProcessInfo.processInfo.thermalState.rawValue,
            "residentBytes": s.resident_bytes, "physicalFootprintBytes": s.physical_footprint_bytes]
        record["soundReadsQueued"] = withUnsafeBytes(of: s.sound_reads_queued) { Array($0.bindMemory(to: Int32.self)) }
        latestDeep = deepTelemetry.sample(record)
        writer.appendDeep(latestDeep, uptime: uptime)
    }

    private static func heapStats(_ sample: EngineDiagnosticSample) -> [String: UInt64] {
        let heapNames = ["liveBytes", "peakLiveBytes", "cumulativeRequestedBytes", "liveAllocations", "metadataBytes", "addressHighWaterBytes"]
        let heapValues = withUnsafeBytes(of: sample.guest_heap) { Array($0.bindMemory(to: UInt64.self)) }
        return Dictionary(uniqueKeysWithValues: zip(heapNames, heapValues))
    }

    /// Host work outside the render passes that only the headset pays, and
    /// the switches that set it (gamecontroller.m, dinput8.c, shims_kernel32.c,
    /// threading.c). Cumulative counts; two records give the interval's.
    private static func hostWork(_ sample: EngineDiagnosticSample) -> [String: Any] {
        [
            "controllerCaptures": sample.controller_captures, "controllerReadingsReused": sample.controller_reuses,
            "controllerLink": EngineDiagnostics.controllerLink(),
            "controllerReuseMicroseconds": sample.controller_reuse_us,
            "dinputAcquireInputLost": sample.dinput_acquire_lost,
            "engineCacheWaits": sample.cache_wait_calls, "engineCacheWaitSeconds": Double(sample.cache_wait_ns) / 1e9,
            "engineCacheWaitBlocks": sample.cache_wait_blocks != 0,
            "cacheReaderReads": sample.cache_reads, "cacheReaderReadSeconds": Double(sample.cache_read_ns) / 1e9,
            "guestThreadQoS": qosName(sample.guest_thread_qos), "guestThreads": sample.guest_threads,
            "hostDrawFastpathEnabled": sample.draw_fastpath_enabled != 0,
            "hostDrawResidentVertexBytes": sample.draw_resident_vertex_bytes,
            "hostDrawArenaVertexBytes": sample.draw_arena_vertex_bytes,
            "hostDrawFoldedClears": sample.draw_folded_clears
        ]
    }

    private static func qosName(_ raw: UInt32) -> String {
        switch raw {
        case 0: "none"
        case QOS_CLASS_USER_INTERACTIVE.rawValue: "userInteractive"
        case QOS_CLASS_USER_INITIATED.rawValue: "userInitiated"
        case QOS_CLASS_DEFAULT.rawValue: "default"
        case QOS_CLASS_UTILITY.rawValue: "utility"
        case QOS_CLASS_BACKGROUND.rawValue: "background"
        default: String(format: "0x%02X", raw)
        }
    }

    private func submissionRecord() -> [String: Any] {
        let selected = immersiveSubmission
        return ["mode": selected.mode, "sequence": selected.sequence,
            "sourceEpoch": selected.sourceEpoch, "producerEpoch": selected.producerEpoch,
            "sceneEpoch": selected.sceneEpoch, "layerEpochs": selected.layerEpochs,
            "worldCadence": selected.worldCadence,
            "sourceFlatSequence": selected.sourceFlatSequence, "latestFlatSequence": selected.latestFlatSequence,
            "poolSlot": selected.poolSlot, "sourceStatus": selected.sourceStatus, "failureReason": selected.failureReason,
            "eyeProjections": selected.eyeProjections, "neutralFromEyes": selected.neutralFromEyes,
            "neutralOriginFromHead": selected.neutralOriginFromHead]
    }

    /// The report: the latest state of everything, a few kilobytes. The
    /// one-second samples it used to carry are in the timeline files.
    private func report(state: Int32, status: String, frame: EngineVisionFrameInfo,
                        sample: EngineDiagnosticSample, now: Date) -> [String: Any] {
        var panorama = EngineVisionPanoramaInfo()
        let panoramaAvailable = enginevision_panorama_info(&panorama)
        var pool = EngineVisionPanoramaGPUStats()
        enginevision_panorama_gpu_stats(&pool)
        let poolError = withUnsafeBytes(of: pool.last_error) { raw -> String in
            String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
        }
        let poolStates = withUnsafeBytes(of: pool.slot_state) { Array($0.bindMemory(to: Int32.self)) }
        let poolLeases = withUnsafeBytes(of: pool.slot_leases) { Array($0.bindMemory(to: Int32.self)) }
        var profile = EngineVisionDrawProfile()
        enginevision_draw_profile(&profile)
        // Schema 2: the samples array moved to the timeline files.
        let report: [String: Any] = [
            "schema": 2, "runID": runID, "startedAt": ISO8601DateFormatter().string(from: started),
            "guestHeap": Self.heapStats(sample),
            "nativeGather": EngineGatherDiagnostics.fields(sample),
            "updatedAt": ISO8601DateFormatter().string(from: now), "state": state, "status": status,
            "bundleID": Bundle.main.bundleIdentifier ?? "unknown",
            "build": Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "unknown",
            "buildID": Bundle.main.infoDictionary?["HaloBuildID"] as? String ?? "unknown",
            "bundlePath": Bundle.main.bundleURL.path,
            "bundledExecutablePath": Bundle.main.bundleURL.appending(path: "GamePayload/halo.exe").path,
            "os": ProcessInfo.processInfo.operatingSystemVersionString,
            "frameSequence": frame.sequence, "width": frame.width, "height": frame.height,
            "audioFrames": sample.audio_frames, "audioNonzeroSamples": sample.audio_nonzero_samples,
            "audioPeak": sample.audio_peak, "audioQueueRunning": sample.audio_queue_running != 0,
            "audioQueueStatus": sample.audio_queue_status, "audioOutputVolume": sample.audio_output_volume,
            "audioQueueGeneration": sample.audio_queue_generation, "audioQueueSuspended": sample.audio_queue_suspended != 0,
            "audioQueueRestartRequired": sample.audio_queue_restart != 0, "audioQueueDeferredBuffers": sample.audio_queue_deferred,
            "audioLastPrepare": sample.audio_last_prepare, "audioLastStart": sample.audio_last_start, "audioLastEnqueue": sample.audio_last_enqueue,
            "audioLastPause": sample.audio_last_pause, "audioLastDispose": sample.audio_last_dispose, "audioLastRecovery": sample.audio_last_recovery,
            "panoramaAvailable": panoramaAvailable, "panoramaSequence": panorama.sequence,
            "radialFogEnabled": halo_settings_radial_fog() != 0,
            "panoramaSourceEpoch": panorama.source_epoch, "panoramaFlatSequence": panorama.flat_sequence,
            "panoramaStatus": panorama.status, "panoramaFailureReason": panorama.failure_reason,
            "panoramaViewportUMin": EnginePanoramaLayerArray.floats(of: panorama.viewport_u_min),
            "panoramaViewportVMin": EnginePanoramaLayerArray.floats(of: panorama.viewport_v_min),
            "panoramaViewportUMax": EnginePanoramaLayerArray.floats(of: panorama.viewport_u_max),
            "panoramaViewportVMax": EnginePanoramaLayerArray.floats(of: panorama.viewport_v_max),
            "panoramaViewCount": panorama.status == 1 ? Int(HALO_PANORAMA_LAYERS) : 0,
            "panoramaZeroCopy": enginevision_panorama_gpu_enabled(),
            "panoramaPublishSuperseded": pool.publish_superseded,
            "panoramaPoolPublished": pool.published, "panoramaPoolDropped": pool.dropped,
            "panoramaPoolPublishFailed": pool.publish_failed,
            "panoramaPoolCarryCopies": pool.carry_copies, "panoramaPoolCarryFailed": pool.carry_failed,
            "panoramaPoolLatestSlot": pool.latest_slot,
            "panoramaPoolSlotStates": poolStates, "panoramaPoolSlotLeases": poolLeases,
            "panoramaPoolLastError": poolError,
            "enginePasses": profile.passes, "enginePassSeconds": Double(profile.pass_ns) / 1e9,
            "engineReadbackSeconds": Double(profile.readback_ns) / 1e9, "engineReadbackCalls": profile.readback_calls,
            "engineDraws": profile.draws, "engineVertexSeconds": Double(profile.vertex_ns) / 1e9,
            "engineSubmitSeconds": Double(profile.submit_ns) / 1e9,
            "engineUploadSeconds": Double(profile.upload_ns) / 1e9,
            "engineSleepSeconds": Double(profile.sleep_ns) / 1e9, "engineSleepCalls": profile.sleep_calls,
            "engineYieldCalls": profile.yield_calls,
            "engineThreadCPUSeconds": Double(profile.engine_cpu_ns) / 1e9,
            "engineThreadFramesByCPU": withUnsafeBytes(of: profile.engine_cpu_frames) { Array($0.bindMemory(to: UInt32.self)) },
            "performanceCores": profile.performance_cores, "efficiencyCores": profile.efficiency_cores,
            "engineProgramCompiles": profile.program_compiles,
            "engineProgramCompileSeconds": Double(profile.program_compile_ns) / 1e9,
            "engineProgramWaits": profile.program_waits, "engineProgramWaitSeconds": Double(profile.program_wait_ns) / 1e9,
            "engineProgramBackgroundPipelines": profile.program_background_pipelines,
            "engineProgramBackgroundFunctions": profile.program_background_functions,
            "engineProgramBackgroundSeconds": Double(profile.program_background_ns) / 1e9,
            "engineProgramPrewarmHits": profile.program_prewarm_hits, "engineProgramArchiveHits": profile.program_archive_hits,
            "engineProgramFunctionCompiles": profile.program_engine_functions,
            "thermalState": ProcessInfo.processInfo.thermalState.rawValue,
            "hapticsOnsets": profile.haptic_onsets,
            "panoramaBudgetExtraHalf": profile.panorama_extra_half, "panoramaBudgetTier": profile.panorama_tier,
            "panoramaBusyMilliseconds": Double(profile.panorama_busy_seconds) * 1000,
            "pointerFrames": profile.pointer_frames, "pointerCursorX": profile.pointer_x, "pointerCursorY": profile.pointer_y,
            "audioWatchdogRebuilds": profile.audio_rebuilds,
            // Layer alignment's running account (the timeline carries it every
            // second; kept out of the per-sample submission to keep it small).
            "panoramaLayerAlign": immersiveSubmission.layerAlignment.dictionary,
            "framePacing": Self.framePacingReport(),
            "panoramaProjectionX": EnginePanoramaLayerArray.floats(of: panorama.projection_x),
            "panoramaProjectionY": EnginePanoramaLayerArray.floats(of: panorama.projection_y),
            "peakResidentBytes": peakResident, "controllerConnectedEver": connectedEver,
            "peakPhysicalFootprintBytes": peakPhysicalFootprint,
            "peakMetalCurrentAllocatedBytes": peakMetalAllocated,
            "physicalFootprintAvailable": (sample.memory_metric_flags & UInt32(ENGINE_DIAGNOSTIC_HAS_PHYSICAL_FOOTPRINT)) != 0,
            "metalCurrentAllocatedAvailable": (sample.memory_metric_flags & UInt32(ENGINE_DIAGNOSTIC_HAS_METAL_ALLOCATED_SIZE)) != 0,
            "buttonsSeenMask": seenButtons, "axisMin": minAxes, "axisMax": maxAxes,
            "originalEngineGamepadReads": sample.gamepad_reads, "originalEngineKeyboardEvents": sample.keyboard_events, "hostLogCaptureError": logError,
            "preparationStatus": preparationStatus, "preparationCopiedBytes": preparationCopiedBytes,
            "preparationTotalBytes": preparationTotalBytes, "immersiveStatus": immersiveStatus,
            "immersiveActive": immersiveActive,
            "immersiveLayerState": immersiveLayerState.map { Int($0) } ?? 0,
            "immersiveLifecycle": EngineImmersiveTrace.shared.snapshot(),
            "immersiveSubmittedFrames": immersiveSubmittedFrames,
            "immersiveCancelledFrames": immersiveCancelledFrames,
            "immersiveTrackingLossFrames": immersiveTrackingLossFrames,
            "immersiveGPUCompletedFrames": immersiveGPUCompletedFrames,
            "immersiveGPUFailedFrames": immersiveGPUFailedFrames,
            "immersiveGPUError": immersiveGPUError,
            "immersiveGPUMilliseconds": immersiveGPUMilliseconds,
            "immersiveGPUMillisecondsMax": immersiveGPUMillisecondsMax,
            "hapticsState": EngineHaptics.state,
            "hapticsEventsTaken": EngineHaptics.eventsTaken,
            "hapticsEventsPlayed": EngineHaptics.eventsPlayed,
            "controllersConnected": GCController.controllers().count,
            "controllerNames": GCController.controllers().map { $0.vendorName ?? "unnamed" }.joined(separator: ", "),
            "controllerHaptics": GCController.controllers().map { $0.haptics == nil ? "none" : "yes" }.joined(separator: ", "),
            "controllerLink": EngineDiagnostics.controllerLink(),
            "immersiveConfiguration": immersiveConfiguration,
            "immersiveSubmission": submissionRecord(),
            "capturedEngineFrames": capturedFrames,
            // historyFiles, historyRecords, historyTruncated and historyError
            // are added by the writer, which alone touches the timeline.
            "deepTelemetry": latestDeep,
            "liveSnapshotLocation": "live.json",
            "deepSamplesLocation": "deep-*.jsonl",
            "deepTargetIntervalSeconds": 0.2,
            "historyScope": "One-second samples in bounded JSONL segments, their only copy since Build76; not per-frame timing or mission-completion proof.",
            "samplesLocation": "timeline-*.jsonl",
            "scope": "Native startup and hardware test; not proof of a playable campaign. Pixel samples measure engine output, not headset display quality."
        ]
        var merged = report
        merged.merge(Self.hostWork(sample)) { current, _ in current }
        merged.merge(Self.voiceFields(sample)) { current, _ in current }
        merged.merge(Self.coreTelemetry()) { current, _ in current }
        return merged
    }

    /// Core residency, scheduling and frame-split groups (CORE_TELEMETRY.md),
    /// appended to the report and to each timeline record. Empty while
    /// HALO_CORE_TELEMETRY=0 has them off, and before the engine starts, so
    /// those records keep exactly the pre-telemetry fields.
    static func coreTelemetry() -> [String: Any] {
        var core = EngineVisionCoreTelemetry()
        return enginevision_core_telemetry(&core) ? EngineCoreTelemetry.fields(core) : [:]
    }

    /// How the engine's voices are used. The counts are cumulative (the
    /// difference between samples is starts and stops per second); the rest
    /// is the moment's state, the sound* fields read from the original
    /// engine's own tables (-1 until readable). A silence with sources and
    /// held voices but none free is voice exhaustion; one with no sources is
    /// nothing asking for sound.
    private static func voiceFields(_ sample: EngineDiagnosticSample) -> [String: Any] {
        func ints<T>(_ tuple: T) -> [Int32] { withUnsafeBytes(of: tuple) { Array($0.bindMemory(to: Int32.self)) } }
        func counts<T>(_ tuple: T) -> [UInt64] { withUnsafeBytes(of: tuple) { Array($0.bindMemory(to: UInt64.self)) } }
        return [
            "audioCallbacks": sample.audio_callbacks, "audioLateCallbacks": sample.audio_late_callbacks,
            "audioEnqueueFailures": sample.audio_enqueue_failures,
            "audioCallbackAgeMilliseconds": Double(sample.audio_callback_age_ns) / 1e6,
            "audioMaxCallbackGapMilliseconds": Double(sample.audio_max_callback_gap_ns) / 1e6,
            "audioMaxCallbackWorkMilliseconds": Double(sample.audio_max_callback_work_ns) / 1e6,
            "audioSampleRate": sample.audio_sample_rate, "audioBufferFrames": sample.audio_buffer_frames,
            "audioVoicePlays": sample.audio_voice_plays, "audioVoiceStops": sample.audio_voice_stops,
            "audioStatusProbes": sample.audio_status_probes, "audioStatusBusy": sample.audio_status_busy,
            "audioCallsRejected": sample.audio_calls_rejected, "audioBuffers": sample.audio_buffers,
            "audioVoicesPlaying": sample.audio_voices_playing, "audioVoicesPeak": sample.audio_voices_peak,
            "soundEngineFlags": sample.sound_engine_flags, "soundSources": sample.sound_sources,
            "soundLoopingSounds": sample.sound_looping_sounds, "soundChannels": sample.sound_channels,
            "soundChannelsBusy": sample.sound_channels_busy, "soundChannelsStarved": sample.sound_channels_starved,
            "soundVoices": sample.sound_voices, "soundVoicesAssigned": sample.sound_voices_assigned,
            "soundVoicesFree": sample.sound_voices_free, "soundVoicesHeld": sample.sound_voices_held,
            "soundVoicesByType": ints(sample.sound_voices_by_type), "soundVoicesFreeByType": ints(sample.sound_free_by_type),
            "soundVoicesHeldByType": ints(sample.sound_held_by_type),
            "soundCacheSounds": sample.sound_cache_sounds, "soundCacheLoaded": sample.sound_cache_loaded,
            "soundCacheLocked": sample.sound_cache_locked, "soundReadsQueued": ints(sample.sound_reads_queued),
            "cacheFileReads": counts(sample.cache_file_reads), "cacheFileReadBytes": counts(sample.cache_file_read_bytes)
        ]
    }

    private func captureEngineFrame(at date: Date, directory: URL) {
        var hint = EngineVisionFrameInfo()
        guard enginevision_frame_info(&hint), hint.width > 0, hint.height > 0,
              hint.byte_count > 0, hint.byte_count <= 4096 * 4096 * 4 else { return }
        var bytes = [UInt8](repeating: 0, count: Int(hint.byte_count))
        var copied = EngineVisionFrameInfo()
        let ok = bytes.withUnsafeMutableBytes {
            enginevision_copy_latest_frame($0.baseAddress, $0.count, &copied)
        }
        guard ok, copied.width > 0, copied.height > 0,
              Int(copied.width) * Int(copied.height) * 4 == bytes.count,
              let provider = CGDataProvider(data: Data(bytes) as CFData),
              let space = CGColorSpace(name: CGColorSpace.sRGB),
              let image = CGImage(width: Int(copied.width), height: Int(copied.height), bitsPerComponent: 8,
                                  bitsPerPixel: 32, bytesPerRow: Int(copied.width) * 4, space: space,
                                  bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent) else { return }
        let encoded = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(encoded, UTType.png.identifier as CFString, 1, nil) else { return }
        CGImageDestinationAddImage(destination, image, nil)
        guard CGImageDestinationFinalize(destination) else { return }
        let name = String(format: "engine-frame-%03d.png", capturedFrames.count + 1)
        do {
            try (encoded as Data).write(to: directory.appending(path: name), options: .atomic)
            capturedFrames.append(["file": name, "sequence": copied.sequence,
                                   "width": copied.width, "height": copied.height,
                                   "capturedAt": ISO8601DateFormatter().string(from: date)])
            lastFrameCapture = date
        } catch { saveStatus = "Frame capture failed: \(error.localizedDescription)" }
    }
}
