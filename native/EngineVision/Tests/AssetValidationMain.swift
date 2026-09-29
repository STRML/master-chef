import Foundation

@main
struct AssetValidationMain {
    static func main() throws {
        guard CommandLine.arguments.count == 2 else {
            FileHandle.standardError.write(Data("usage: asset-validation GAME_FOLDER\n".utf8))
            Foundation.exit(64)
        }
        let receipt = try EngineAssetImporter.preflight(URL(fileURLWithPath: CommandLine.arguments[1]))
        print("ASSET_PREFLIGHT_OK files=\(receipt.fileCount) bytes=\(receipt.totalBytes) halo_sha256=\(receipt.executableSHA256)")
    }
}
