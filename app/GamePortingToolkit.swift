import Combine
import Foundation

/// Apple's Game Porting Toolkit (D3DMetal), which games can use for Direct3D
/// 11 and 12 in place of the engine's own DXMT. Apple's licence keeps it with
/// Apple, so Wheelio never ships it: the player downloads it from Apple, and
/// Wheelio copies its redistributable libraries (redist/lib) from the disk
/// image, checking Apple's signature, into its support folder. The engine's
/// launcher finds them there (WHEELIO_D3DMETAL).
@MainActor
final class GamePortingToolkit: ObservableObject {
    static let shared = GamePortingToolkit()

    /// Where Apple offers it (an Apple ID and its licence are needed).
    static let downloadPage = URL(string: "https://developer.apple.com/games/game-porting-toolkit/")!

    @Published private(set) var installed: Bool
    @Published private(set) var adding = false
    @Published private(set) var lastError: String?

    nonisolated static var folder: URL { WheelioPaths.support.appendingPathComponent("GamePortingToolkit", isDirectory: true) }

    private init() {
        installed = Self.isInstalled(at: Self.folder)
    }

    /// Adds the toolkit from Apple's disk image, or from a folder holding it.
    func add(from source: URL) {
        guard !adding else { return }
        adding = true
        lastError = nil
        Task.detached {
            let result = Result { try Self.copyRedistributables(from: source, to: Self.folder) }
            await MainActor.run {
                self.adding = false
                if case .failure(let error) = result { self.lastError = error.localizedDescription }
                self.installed = Self.isInstalled(at: Self.folder)
            }
        }
    }

    func remove() {
        try? FileManager.default.removeItem(at: Self.folder)
        installed = false
    }

    // MARK: - Finding, checking and copying (no UI; tested)

    enum ToolkitError: LocalizedError {
        case notFound
        case notApple

        var errorDescription: String? {
            switch self {
            case .notFound: return "That isn't Apple's Game Porting Toolkit."
            case .notApple: return "That copy of the Game Porting Toolkit isn't signed by Apple."
            }
        }
    }

    /// The parts the engine uses: the D3DMetal library and Apple's Direct3D DLLs.
    nonisolated static func isInstalled(at folder: URL) -> Bool {
        let files = FileManager.default
        return files.fileExists(atPath: folder.appendingPathComponent("external/libd3dshared.dylib").path)
            && files.fileExists(atPath: folder.appendingPathComponent("wine/x86_64-windows").path)
    }

    /// Copies redist/lib from a toolkit disk image or folder into `destination`,
    /// replacing what's there only once the new copy is complete and checked.
    nonisolated static func copyRedistributables(from source: URL, to destination: URL) throws {
        try withToolkitFolder(source) { root in
            guard let lib = findRedistributables(in: root) else { throw ToolkitError.notFound }
            try verifyAppleSignature(lib)
            let files = FileManager.default
            try files.createDirectory(at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
            let staging = destination.deletingLastPathComponent()
                .appendingPathComponent(".\(destination.lastPathComponent).new", isDirectory: true)
            try? files.removeItem(at: staging)
            try run("/usr/bin/ditto", [lib.path, staging.path])
            guard isInstalled(at: staging) else {
                try? files.removeItem(at: staging)
                throw ToolkitError.notFound
            }
            try? files.removeItem(at: destination)
            try files.moveItem(at: staging, to: destination)
        }
    }

    /// redist/lib inside the toolkit, looking a few levels down (Apple's
    /// layout has moved between versions).
    nonisolated static func findRedistributables(in root: URL, depth: Int = 3) -> URL? {
        let candidate = root.appendingPathComponent("redist/lib", isDirectory: true)
        if isInstalled(at: candidate) { return candidate }
        guard depth > 0,
              let children = try? FileManager.default.contentsOfDirectory(
                at: root, includingPropertiesForKeys: [.isDirectoryKey], options: [.skipsHiddenFiles])
        else { return nil }
        for child in children where (try? child.resourceValues(forKeys: [.isDirectoryKey]))?.isDirectory == true {
            if let found = findRedistributables(in: child, depth: depth - 1) { return found }
        }
        return nil
    }

    /// Apple's own signature on every Mac library it ships (Apple's Windows
    /// DLLs can't carry one; they're copied straight from the image).
    nonisolated static func verifyAppleSignature(_ lib: URL) throws {
        var targets = [lib.appendingPathComponent("external/libd3dshared.dylib")]
        let framework = lib.appendingPathComponent("external/D3DMetal.framework")
        if FileManager.default.fileExists(atPath: framework.path) { targets.append(framework) }
        for target in targets {
            do {
                try run("/usr/bin/codesign", ["--verify", "--strict", "-R=anchor apple", target.path])
            } catch {
                throw ToolkitError.notApple
            }
        }
    }

    /// Runs `body` with a folder to search: the source itself, or the disk
    /// image mounted read-only (and any disk image inside it), detached after.
    private nonisolated static func withToolkitFolder(_ source: URL, _ body: (URL) throws -> Void) throws {
        guard source.pathExtension.lowercased() == "dmg" else {
            try body(source)
            return
        }
        let mount = try attach(source)
        defer { detach(mount) }
        if findRedistributables(in: mount) != nil {
            try body(mount)
            return
        }
        // Apple has shipped the redistributables in a disk image inside the download.
        let inner = (try? FileManager.default.contentsOfDirectory(at: mount, includingPropertiesForKeys: nil)) ?? []
        guard let image = inner.first(where: { $0.pathExtension.lowercased() == "dmg" }) else {
            throw ToolkitError.notFound
        }
        let innerMount = try attach(image)
        defer { detach(innerMount) }
        try body(innerMount)
    }

    private nonisolated static func attach(_ image: URL) throws -> URL {
        let point = FileManager.default.temporaryDirectory.appendingPathComponent("wheelio-gptk-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: point, withIntermediateDirectories: true)
        try run("/usr/bin/hdiutil", ["attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", point.path, image.path])
        return point
    }

    private nonisolated static func detach(_ point: URL) {
        try? run("/usr/bin/hdiutil", ["detach", "-quiet", point.path])
        try? FileManager.default.removeItem(at: point)
    }

    private nonisolated static func run(_ tool: String, _ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = arguments
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            throw CocoaError(.executableLoad, userInfo: [NSLocalizedDescriptionKey: "\(tool) failed"])
        }
    }
}
