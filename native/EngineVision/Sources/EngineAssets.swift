import Foundation
import CryptoKit
import Combine

struct EngineAssetRequirement: Identifiable, Sendable {
    let path: String
    let minimumBytes: Int64
    var id: String { path }
}

enum EngineAssetCatalog {
    static let haloSHA256 = "c9acf0c469543283cfed6d7dc04ade976dbdfc7cb4532cf070386de169c19545"
    static let required: [EngineAssetRequirement] = [
        .init(path: "halo.exe", minimumBytes: 2_000_000),
        .init(path: "maps/ui.map", minimumBytes: 2_000_000),
        .init(path: "maps/a10.map", minimumBytes: 90_000_000),
        .init(path: "maps/bitmaps.map", minimumBytes: 300_000_000),
        .init(path: "maps/sounds.map", minimumBytes: 200_000_000),
        .init(path: "shaders/vsh.bin", minimumBytes: 30_000),
        .init(path: "shaders/fx.bin", minimumBytes: 900_000),
        .init(path: "strings.dll", minimumBytes: 1_000_000),
        .init(path: "halo-vision-registry.txt", minimumBytes: 100)
    ]
}

enum EngineAssetError: LocalizedError {
    case missing(String)
    case tooSmall(String, Int64, Int64)
    case wrongExecutable(String)
    case duplicate(String)
    case emptyFolder
    case invalidBundle(String)

    var errorDescription: String? {
        switch self {
        case .missing(let path): return "Required owned game file is missing: \(path)"
        case .tooSmall(let path, let got, let need): return "\(path) is \(got) bytes; at least \(need) bytes are required."
        case .wrongExecutable(let digest): return "halo.exe has SHA-256 \(digest); this build requires the owned PC 1.10 executable \(EngineAssetCatalog.haloSHA256)."
        case .duplicate(let path): return "The selected folder contains two files that map to \(path)."
        case .emptyFolder: return "The selected folder contains no regular files."
        case .invalidBundle(let detail): return "Bundled game data could not be verified: \(detail)"
        }
    }
}

struct EngineImportUpdate: Sendable {
    let copied: Int64
    let total: Int64
    let file: String
    let detail: String
}

struct EngineImportReceipt: Codable, Sendable {
    let importedAt: Date
    let fileCount: Int
    let totalBytes: Int64
    let executableSHA256: String
    var payloadID: String? = nil
}

struct EngineBundledManifest: Codable, Sendable {
    struct File: Codable, Sendable {
        let path: String
        let bytes: Int64
        let sha256: String
    }
    let formatVersion: Int
    let payloadID: String
    let fileCount: Int
    let totalBytes: Int64
    let executableSHA256: String
    let files: [File]

    static func load(_ url: URL) throws -> Self {
        let value = try JSONDecoder().decode(Self.self, from: Data(contentsOf: url))
        try value.check()
        return value
    }

    func check() throws {
        func isHash(_ value: String) -> Bool {
            value.utf8.count == 64 && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
        }
        guard formatVersion == 1, isHash(payloadID), fileCount > 0,
              fileCount == files.count, totalBytes > 0,
              executableSHA256 == EngineAssetCatalog.haloSHA256 else {
            throw EngineAssetError.invalidBundle("unsupported or incomplete manifest")
        }
        var seen = Set<String>(), total: Int64 = 0
        for file in files {
            let components = file.path.split(separator: "/", omittingEmptySubsequences: false)
            guard !components.isEmpty, !components.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." }),
                  !file.path.contains("\\"), !file.path.contains("\0"),
                  file.path != "engine-vision-import.json", file.bytes >= 0, isHash(file.sha256),
                  seen.insert(file.path.lowercased()).inserted else {
                throw EngineAssetError.invalidBundle("invalid or duplicate path: \(file.path)")
            }
            let sum = total.addingReportingOverflow(file.bytes)
            guard !sum.overflow else { throw EngineAssetError.invalidBundle("file size overflow") }
            total = sum.partialValue
        }
        guard total == totalBytes else { throw EngineAssetError.invalidBundle("total size mismatch") }
        let lookup = Dictionary(uniqueKeysWithValues: files.map { ($0.path, $0) })
        for required in EngineAssetCatalog.required {
            guard let file = lookup[required.path], file.bytes >= required.minimumBytes else {
                throw EngineAssetError.invalidBundle("missing required file: \(required.path)")
            }
        }
        guard lookup["halo.exe"]?.sha256 == executableSHA256 else {
            throw EngineAssetError.invalidBundle("executable hash mismatch")
        }
    }
}

enum EngineAssetImporter {
    // Halo writes profiles, checkpoints, and playlist state beneath this
    // emulated Windows user directory.  The bundled copies are first-launch
    // defaults; after installation this whole subtree belongs to the player.
    static let bundledMutableRoot = "Users/Player/Documents/My Games/Halo"

    private struct Entry: Sendable {
        let source: URL
        let relative: String
        let bytes: Int64
    }

    static func validate(_ root: URL) throws -> EngineImportReceipt {
        for item in EngineAssetCatalog.required {
            let url = root.appending(path: item.path)
            guard FileManager.default.fileExists(atPath: url.path) else { throw EngineAssetError.missing(item.path) }
            let size = try url.resourceValues(forKeys: [.fileSizeKey]).fileSize.map(Int64.init) ?? 0
            guard size >= item.minimumBytes else { throw EngineAssetError.tooSmall(item.path, size, item.minimumBytes) }
        }
        let digest = try sha256(root.appending(path: "halo.exe"))
        guard digest == EngineAssetCatalog.haloSHA256 else { throw EngineAssetError.wrongExecutable(digest) }
        let receiptURL = root.appending(path: "engine-vision-import.json")
        if let data = try? Data(contentsOf: receiptURL),
           let prior = try? JSONDecoder().decode(EngineImportReceipt.self, from: data),
           prior.executableSHA256 == digest { return prior }
        return .init(importedAt: .now, fileCount: 0, totalBytes: 0, executableSHA256: digest)
    }

    static func preflight(_ source: URL) throws -> EngineImportReceipt {
        let (entries, digest) = try checkedEntries(source)
        return .init(importedAt: .now, fileCount: entries.count,
                     totalBytes: entries.reduce(Int64(0)) { $0 + $1.bytes }, executableSHA256: digest)
    }

    static func importFolder(_ source: URL, destination: URL, bundleManifest: EngineBundledManifest? = nil,
                             progress: @escaping @Sendable (EngineImportUpdate) -> Void) throws -> EngineImportReceipt {
        let manager = FileManager.default
        let entries: [Entry]
        let sourceDigest: String
        var expectedHashes: [String: String] = [:]
        if let manifest = bundleManifest {
            try manifest.check()
            // The signed manifest already specifies exact relative paths. Do
            // not reconstruct them from FileManager's /var vs /private/var URLs.
            entries = try manifest.files.map { file in
                let url = source.appending(path: file.path)
                let values: URLResourceValues
                do {
                    values = try url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey])
                } catch {
                    throw EngineAssetError.invalidBundle("cannot read \(file.path): \(error.localizedDescription)")
                }
                guard values.isRegularFile == true, values.isSymbolicLink != true,
                      values.fileSize.map(Int64.init) == file.bytes else {
                    throw EngineAssetError.invalidBundle("missing or incomplete file: \(file.path)")
                }
                return Entry(source: url, relative: file.path, bytes: file.bytes)
            }
            sourceDigest = manifest.executableSHA256
            expectedHashes = Dictionary(uniqueKeysWithValues: manifest.files.map { ($0.path, $0.sha256) })
        } else {
            (entries, sourceDigest) = try checkedEntries(source)
        }

        let parent = destination.deletingLastPathComponent()
        try manager.createDirectory(at: parent, withIntermediateDirectories: true)
        let staging = parent.appending(path: ".Game-import-\(UUID().uuidString)", directoryHint: .isDirectory)
        try manager.createDirectory(at: staging, withIntermediateDirectories: true)
        var completed: Int64 = 0
        let total = entries.reduce(Int64(0)) { $0 + $1.bytes }
        do {
            for (index, entry) in entries.enumerated() {
                try Task.checkCancellation()
                let target = staging.appending(path: entry.relative)
                try manager.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
                progress(.init(copied: completed, total: total, file: entry.relative,
                               detail: "Copying file \(index + 1) of \(entries.count): \(entry.relative)"))
                let input = try FileHandle(forReadingFrom: entry.source)
                manager.createFile(atPath: target.path, contents: nil)
                let output = try FileHandle(forWritingTo: target)
                defer { try? input.close(); try? output.close() }
                var hash = SHA256()
                while let data = try input.read(upToCount: 1024 * 1024), !data.isEmpty {
                    try Task.checkCancellation(); try output.write(contentsOf: data)
                    if bundleManifest != nil { hash.update(data: data) }
                    completed += Int64(data.count)
                    progress(.init(copied: completed, total: total, file: entry.relative,
                                   detail: "Copied \(ByteCountFormatter.string(fromByteCount: completed, countStyle: .file)) of \(ByteCountFormatter.string(fromByteCount: total, countStyle: .file))"))
                }
                if let expected = expectedHashes[entry.relative] {
                    let digest = hash.finalize().map { String(format: "%02x", $0) }.joined()
                    guard digest == expected else { throw EngineAssetError.invalidBundle("SHA-256 mismatch: \(entry.relative)") }
                }
            }
            if bundleManifest != nil {
                // A repair stages and verifies fresh immutable payload files,
                // then carries forward the player's complete writable tree.
                // The live destination is left untouched until the final move.
                try preserveMutableGameData(from: destination, into: staging)
            }
            let receipt = EngineImportReceipt(importedAt: .now, fileCount: entries.count, totalBytes: total,
                                              executableSHA256: sourceDigest, payloadID: bundleManifest?.payloadID)
            let receiptData = try JSONEncoder().encode(receipt)
            try receiptData.write(to: staging.appending(path: "engine-vision-import.json"), options: .atomic)
            _ = try validate(staging)
            try promoteStaging(staging, to: destination)
            progress(.init(copied: total, total: total, file: "Complete",
                           detail: "Imported \(entries.count) files and verified \(ByteCountFormatter.string(fromByteCount: total, countStyle: .file))."))
            return receipt
        } catch {
            try? manager.removeItem(at: staging)
            throw error
        }
    }

    static func validateInstalledBundle(_ root: URL, manifest: EngineBundledManifest) throws -> EngineImportReceipt {
        try manifest.check()
        let receipt = try validate(root)
        guard receipt.payloadID == manifest.payloadID, receipt.fileCount == manifest.fileCount,
              receipt.totalBytes == manifest.totalBytes else {
            throw EngineAssetError.invalidBundle("this version has not finished preparing")
        }
        try validatePreparedFiles(root, files: manifest.files)
        return receipt
    }

    static func isMutableInstalledPath(_ path: String) -> Bool {
        let root = bundledMutableRoot.lowercased()
        let candidate = path.lowercased()
        return candidate == root || candidate.hasPrefix(root + "/")
    }

    // Kept separate so the installed-file policy can be tested with a small
    // fixture instead of copying the complete 1.5 GB game payload.
    static func validatePreparedFiles(_ root: URL, files: [EngineBundledManifest.File]) throws {
        for file in files where !isMutableInstalledPath(file.path) {
            let values = try root.appending(path: file.path).resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey])
            guard values.isRegularFile == true, values.isSymbolicLink != true,
                  values.fileSize.map(Int64.init) == file.bytes else {
                throw EngineAssetError.invalidBundle("prepared file is missing or incomplete: \(file.path)")
            }
        }
    }

    static func preserveMutableGameData(from installedRoot: URL, into stagingRoot: URL) throws {
        let manager = FileManager.default
        let source = installedRoot.appending(path: bundledMutableRoot, directoryHint: .isDirectory)
        var isDirectory: ObjCBool = false
        guard manager.fileExists(atPath: source.path, isDirectory: &isDirectory) else { return }
        guard isDirectory.boolValue else {
            throw EngineAssetError.invalidBundle("saved game data is not a directory")
        }

        let keys: Set<URLResourceKey> = [.isDirectoryKey, .isRegularFileKey, .isSymbolicLinkKey]
        let rootValues = try source.resourceValues(forKeys: keys)
        guard rootValues.isDirectory == true, rootValues.isSymbolicLink != true else {
            throw EngineAssetError.invalidBundle("saved game data contains an unsupported link")
        }
        guard let enumerator = manager.enumerator(at: source, includingPropertiesForKeys: Array(keys),
                                                  options: []) else {
            throw EngineAssetError.invalidBundle("saved game data could not be read")
        }
        for case let item as URL in enumerator {
            let values = try item.resourceValues(forKeys: keys)
            guard values.isSymbolicLink != true,
                  values.isDirectory == true || values.isRegularFile == true else {
                throw EngineAssetError.invalidBundle("saved game data contains an unsupported item")
            }
        }

        let target = stagingRoot.appending(path: bundledMutableRoot, directoryHint: .isDirectory)
        if manager.fileExists(atPath: target.path) { try manager.removeItem(at: target) }
        try manager.createDirectory(at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
        try manager.copyItem(at: source, to: target)
    }

    static func promoteStaging(_ staging: URL, to destination: URL) throws {
        let manager = FileManager.default
        guard manager.fileExists(atPath: destination.path) else {
            try manager.moveItem(at: staging, to: destination)
            return
        }

        // Both paths are children of the same Application Support directory,
        // so these directory renames stay on one volume. Keep the old install
        // until staging has reached its final name; a failed promotion rolls
        // the original tree, including saves, straight back into place.
        let backup = destination.deletingLastPathComponent()
            .appending(path: ".Game-backup-\(UUID().uuidString)", directoryHint: .isDirectory)
        try manager.moveItem(at: destination, to: backup)
        do {
            try manager.moveItem(at: staging, to: destination)
        } catch {
            do {
                try manager.moveItem(at: backup, to: destination)
            } catch let rollbackError {
                throw EngineAssetError.invalidBundle(
                    "promotion failed and the prior game data remains at \(backup.lastPathComponent): \(rollbackError.localizedDescription)"
                )
            }
            throw error
        }
        // Promotion is complete. A cleanup failure leaves only a redundant
        // backup and must not turn a successful repair into a launch failure.
        try? manager.removeItem(at: backup)
    }

    private static func collect(_ source: URL) throws -> [Entry] {
        let source = source.resolvingSymlinksInPath().standardizedFileURL
        let rootComponents = source.pathComponents
        let keys: [URLResourceKey] = [.isRegularFileKey, .fileSizeKey, .isSymbolicLinkKey]
        guard let enumerator = FileManager.default.enumerator(at: source, includingPropertiesForKeys: keys,
                                                               options: [.skipsHiddenFiles]) else { return [] }
        var entries: [Entry] = [], destinations = Set<String>()
        for case let file as URL in enumerator {
            let values = try file.resourceValues(forKeys: Set(keys))
            guard values.isRegularFile == true, values.isSymbolicLink != true else { continue }
            let components = file.resolvingSymlinksInPath().standardizedFileURL.pathComponents
            guard components.count > rootComponents.count,
                  components.starts(with: rootComponents) else {
                throw EngineAssetError.invalidBundle("enumerated file is outside the selected folder")
            }
            let raw = components.dropFirst(rootComponents.count).joined(separator: "/")
            let relative = canonicalRelativePath(raw)
            let key = relative.lowercased()
            guard destinations.insert(key).inserted else { throw EngineAssetError.duplicate(relative) }
            entries.append(.init(source: file, relative: relative, bytes: Int64(values.fileSize ?? 0)))
        }
        return entries.sorted { $0.relative.localizedStandardCompare($1.relative) == .orderedAscending }
    }

    private static func checkedEntries(_ source: URL) throws -> ([Entry], String) {
        let entries = try collect(source)
        guard !entries.isEmpty else { throw EngineAssetError.emptyFolder }
        let lookup = Dictionary(uniqueKeysWithValues: entries.map { ($0.relative.lowercased(), $0) })
        for item in EngineAssetCatalog.required {
            guard let entry = lookup[item.path] else { throw EngineAssetError.missing(item.path) }
            guard entry.bytes >= item.minimumBytes else { throw EngineAssetError.tooSmall(item.path, entry.bytes, item.minimumBytes) }
        }
        let digest = try sha256(lookup["halo.exe"]!.source)
        guard digest == EngineAssetCatalog.haloSHA256 else { throw EngineAssetError.wrongExecutable(digest) }
        return (entries, digest)
    }

    private static func canonicalRelativePath(_ raw: String) -> String {
        var components = raw.split(separator: "/").map(String.init)
        guard !components.isEmpty else { return raw }
        switch components[0].lowercased() {
        case "maps": components[0] = "maps"
        case "shaders": components[0] = "shaders"
        default: if components.count == 1 { components[0] = components[0].lowercased() }
        }
        return components.joined(separator: "/")
    }

    private static func sha256(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url); defer { try? handle.close() }
        var hash = SHA256()
        while let data = try handle.read(upToCount: 1024 * 1024), !data.isEmpty { hash.update(data: data) }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
}

@MainActor
final class EngineAssetStore: ObservableObject {
    enum Phase: Equatable { case checking, missing, importing, ready, failed(String) }
    @Published var phase: Phase = .checking
    @Published var detail = "Checking app storage for imported game data…"
    @Published var copied: Int64 = 0
    @Published var total: Int64 = 0
    @Published var currentFile = ""
    @Published var receipt: EngineImportReceipt?

    @Published private(set) var rootURL: URL
    let hasBundledPayload: Bool
    private let payloadURL: URL
    private let manifestURL: URL
    private let supportURL: URL
    private var preparationTask: Task<Void, Never>?
    init() {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        supportURL = support.appending(path: "HaloVision", directoryHint: .isDirectory)
        rootURL = support.appending(path: "HaloVision/Game", directoryHint: .isDirectory)
        payloadURL = Bundle.main.bundleURL.appending(path: "GamePayload", directoryHint: .isDirectory)
        manifestURL = Bundle.main.bundleURL.appending(path: "GamePayloadManifest.json")
        hasBundledPayload = FileManager.default.fileExists(atPath: payloadURL.path) || FileManager.default.fileExists(atPath: manifestURL.path)
        if hasBundledPayload { prepareBundledGame() } else { restore() }
    }

    func prepareBundledGame() {
        guard hasBundledPayload, preparationTask == nil else { return }
        phase = .checking; detail = "Checking the game data included in this app…"
        preparationTask = Task {
            defer { preparationTask = nil }
            do {
                let manifest = try EngineBundledManifest.load(manifestURL)
                // A distinct version directory preserves earlier game files and saves.
                let destination = supportURL.appending(path: "PackagedGames/\(manifest.payloadID)", directoryHint: .isDirectory)
                rootURL = destination
                if let prior = try? EngineAssetImporter.validateInstalledBundle(destination, manifest: manifest) {
                    receipt = prior; phase = .ready; detail = "Included game data is ready."
                    return
                }
                phase = .importing; copied = 0; total = manifest.totalBytes
                detail = "Preparing the included game data on this headset…"
                let result = try await Task.detached(priority: .userInitiated) { [payloadURL] in
                    try EngineAssetImporter.importFolder(payloadURL, destination: destination, bundleManifest: manifest) { update in
                        Task { @MainActor [weak self] in
                            guard let self, self.phase == .importing else { return }
                            self.copied = update.copied; self.total = update.total
                            self.currentFile = update.file; self.detail = "Preparing included game data: \(update.detail)"
                        }
                    }
                }.value
                receipt = result; copied = result.totalBytes; total = result.totalBytes
                currentFile = "Complete"; phase = .ready
                detail = "All \(result.fileCount) included game files are verified and ready."
            } catch {
                phase = .failed(error.localizedDescription); detail = error.localizedDescription
            }
        }
    }

    func restore() {
        do {
            receipt = try EngineAssetImporter.validate(rootURL); phase = .ready
            detail = "Owned Halo game data is verified and ready."
        } catch {
            receipt = nil; phase = .missing; detail = error.localizedDescription
        }
    }

    func importFolder(_ source: URL) {
        guard !hasBundledPayload, phase != .importing else { return }
        phase = .importing; copied = 0; total = 0; currentFile = "Preparing…"
        let accessed = source.startAccessingSecurityScopedResource()
        Task {
            defer { if accessed { source.stopAccessingSecurityScopedResource() } }
            do {
                let result = try await Task.detached(priority: .userInitiated) { [rootURL] in
                    try EngineAssetImporter.importFolder(source, destination: rootURL) { update in
                        Task { @MainActor [weak self] in
                            self?.copied = update.copied; self?.total = update.total
                            self?.currentFile = update.file; self?.detail = update.detail
                        }
                    }
                }.value
                receipt = result; copied = result.totalBytes; total = result.totalBytes
                currentFile = "Complete"; detail = "Imported and verified \(result.fileCount) actual game files."
                phase = .ready
            } catch {
                phase = .failed(error.localizedDescription); detail = error.localizedDescription
            }
        }
    }
}
