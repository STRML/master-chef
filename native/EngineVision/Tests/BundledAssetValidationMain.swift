import Foundation

final class ProgressLog: @unchecked Sendable {
    private let lock = NSLock()
    var previous: Int64 = 0
    func check(_ update: EngineImportUpdate) {
        lock.lock(); defer { lock.unlock() }
        precondition(update.copied >= previous && update.copied <= update.total)
        previous = update.copied
    }
}
@main struct BundledAssetValidationMain {
    static func rejects(_ name: String, _ body: () throws -> Void) {
        do { try body(); fatalError("Expected rejection: \(name)") } catch { print("REJECT_OK \(name)") }
    }
    static func main() throws {
        let base = URL(fileURLWithPath: CommandLine.arguments[1])
        let manifestURL = base.appending(path: "GamePayloadManifest.json")
        let manifest = try EngineBundledManifest.load(manifestURL)
        // Apple exposes this temp root as /var while enumeration returns
        // /private/var. The old character-count slicing lost the file names.
        let temporary = FileManager.default.temporaryDirectory.appending(path: "Halo-path-\(UUID().uuidString)")
        let source = temporary.appending(path: "HaloVision.app/GamePayload", directoryHint: .isDirectory)
        try FileManager.default.createDirectory(at: source, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: temporary) }
        for file in manifest.files {
            let target = source.appending(path: file.path)
            try FileManager.default.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
            try FileManager.default.linkItem(at: base.appending(path: "GamePayload/\(file.path)"), to: target)
        }
        let resolved = source.resolvingSymlinksInPath()
        let enumeration = FileManager.default.enumerator(at: resolved, includingPropertiesForKeys: [.isRegularFileKey])!
        for case let url as URL in enumeration where url.lastPathComponent == "halo.exe" {
            let oldRelative = String(url.path.dropFirst(resolved.path.count)).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
            print("LEGACY_PATH_REPRO old=\(oldRelative) expected=halo.exe")
            precondition(oldRelative != "halo.exe", "Fixture must exercise Apple path aliases")
        }
        let preflight = try EngineAssetImporter.preflight(source)
        precondition(preflight.fileCount == manifest.fileCount)
        let alias = temporary.appending(path: "Alias.app")
        try FileManager.default.createSymbolicLink(at: alias, withDestinationURL: source.deletingLastPathComponent())
        let aliasPreflight = try EngineAssetImporter.preflight(alias.appending(path: "GamePayload"))
        precondition(aliasPreflight.fileCount == manifest.fileCount)
        print("VAR_ALIAS_AND_SYMLINK_PREFLIGHT_PASS")
        let target = base.appending(path: "test-installed-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: target) }
        let progress = ProgressLog()
        let receipt = try EngineAssetImporter.importFolder(source, destination: target, bundleManifest: manifest) { progress.check($0) }
        precondition(receipt.payloadID == manifest.payloadID && receipt.fileCount == manifest.fileCount)
        precondition(receipt.totalBytes == manifest.totalBytes && progress.previous == manifest.totalBytes)
        _ = try EngineAssetImporter.validateInstalledBundle(target, manifest: manifest)
        print("BUNDLED_INSTALL_AND_REUSE_PASS \(receipt.fileCount) files \(receipt.totalBytes) bytes")
        let object = try JSONSerialization.jsonObject(with: Data(contentsOf: manifestURL)) as! [String: Any]
        func changed(_ block: (inout [String: Any]) -> Void) throws -> EngineBundledManifest {
            var copy = object; block(&copy)
            return try JSONDecoder().decode(EngineBundledManifest.self, from: JSONSerialization.data(withJSONObject: copy))
        }
        rejects("unsafe path") { try changed { value in
            var files = value["files"] as! [[String: Any]]; files[0]["path"] = "../escape"; value["files"] = files
        }.check() }
        rejects("duplicate path") { try changed { value in
            var files = value["files"] as! [[String: Any]]; files[1]["path"] = files[0]["path"]; value["files"] = files
        }.check() }
        rejects("incorrect total") { try changed { $0["totalBytes"] = manifest.totalBytes + 1 }.check() }
        rejects("stale receipt") {
            let stale = try changed { $0["payloadID"] = String(repeating: "b", count: 64) }
            _ = try EngineAssetImporter.validateInstalledBundle(target, manifest: stale)
        }
        let badHash = try changed { value in
            var files = value["files"] as! [[String: Any]]
            let i = files.firstIndex { $0["path"] as? String != "halo.exe" }!
            files[i]["sha256"] = String(repeating: "0", count: 64); value["files"] = files
        }
        rejects("corrupt bundled content") {
            _ = try EngineAssetImporter.importFolder(source, destination: target, bundleManifest: badHash) { _ in }
        }
        _ = try EngineAssetImporter.validateInstalledBundle(target, manifest: manifest)
        let leaf = manifest.files.first { $0.path != "halo.exe" }!
        try FileManager.default.removeItem(at: target.appending(path: leaf.path))
        rejects("incomplete prepared data") { _ = try EngineAssetImporter.validateInstalledBundle(target, manifest: manifest) }
        print("BUNDLED_ASSET_VALIDATION_PASS")
    }
}
