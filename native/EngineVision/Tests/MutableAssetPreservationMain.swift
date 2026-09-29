import Foundation

@main struct MutableAssetPreservationMain {
    static func write(_ text: String, to url: URL) throws {
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: url)
    }

    static func main() throws {
        let manager = FileManager.default
        let temporary = manager.temporaryDirectory.appending(path: "Halo-mutable-assets-\(UUID().uuidString)",
                                                               directoryHint: .isDirectory)
        let installed = temporary.appending(path: "installed", directoryHint: .isDirectory)
        let staging = temporary.appending(path: "staging", directoryHint: .isDirectory)
        defer { try? manager.removeItem(at: temporary) }

        let immutablePath = "maps/ui.map"
        let savePath = EngineAssetImporter.bundledMutableRoot + "/savegame.bin"
        let profilePath = EngineAssetImporter.bundledMutableRoot + "/saved/player_profiles/new/00.sav"
        let files = [
            EngineBundledManifest.File(path: immutablePath, bytes: 4, sha256: String(repeating: "0", count: 64)),
            EngineBundledManifest.File(path: savePath, bytes: 7, sha256: String(repeating: "0", count: 64))
        ]

        try write("map!", to: installed.appending(path: immutablePath))
        let changedSave = "player checkpoint changed size"
        try write(changedSave, to: installed.appending(path: savePath))
        try write("new profile progress", to: installed.appending(path: profilePath))

        // A changed writable file must not invalidate an otherwise complete
        // installed payload, even when its size differs from the manifest.
        try EngineAssetImporter.validatePreparedFiles(installed, files: files)
        print("MUTATED_SAVE_REUSE_PASS")

        try manager.removeItem(at: installed.appending(path: immutablePath))
        do {
            try EngineAssetImporter.validatePreparedFiles(installed, files: files)
            fatalError("missing immutable file was accepted")
        } catch {
            print("IMMUTABLE_MISSING_REPAIR_REQUIRED_PASS")
        }

        // Model the staged defaults created by importFolder before its repair
        // overlay, then prove both changed and newly-created progress survive.
        try write("map!", to: staging.appending(path: immutablePath))
        try write("default", to: staging.appending(path: savePath))
        try EngineAssetImporter.preserveMutableGameData(from: installed, into: staging)
        let preservedSave = String(decoding: try Data(contentsOf: staging.appending(path: savePath)), as: UTF8.self)
        let preservedProfile = String(decoding: try Data(contentsOf: staging.appending(path: profilePath)), as: UTF8.self)
        precondition(preservedSave == changedSave)
        precondition(preservedProfile == "new profile progress")
        print("REPAIR_PRESERVES_SAVE_TREE_PASS")

        let promotionDestination = temporary.appending(path: "promotion-destination", directoryHint: .isDirectory)
        let originalSave = promotionDestination.appending(path: savePath)
        try write("irreplaceable progress", to: originalSave)
        let missingStaging = temporary.appending(path: "missing-staging", directoryHint: .isDirectory)
        do {
            try EngineAssetImporter.promoteStaging(missingStaging, to: promotionDestination)
            fatalError("missing staging directory was promoted")
        } catch {
            let afterFailure = String(decoding: try Data(contentsOf: originalSave), as: UTF8.self)
            precondition(afterFailure == "irreplaceable progress")
            print("PROMOTION_FAILURE_ROLLBACK_PASS")
        }
    }
}
