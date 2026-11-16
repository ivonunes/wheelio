import Combine
import CryptoKit
import Foundation

/// The Wine engine Steam games run on (built from CodeWeavers' sources by
/// engine/build.sh and published as its own release by release-engine.yml).
/// This build of Wheelio pins one engine in engine-release.json; it's
/// downloaded once, verified against the pinned SHA-256 and unpacked into
/// Wheelio's support folder. Only the current version is kept.
@MainActor
final class Engine: ObservableObject {
    static let shared = Engine()

    /// An engine release, as engine-release.json pins it. `source` is the git
    /// tree of engine/ it was built from, which release.yml checks.
    struct Release: Equatable, Decodable {
        let version: String
        let url: URL
        let sha256: String
        let source: String
    }

    /// The engine this build of Wheelio runs games on, bundled with the app;
    /// nil before the first engine is published.
    static let release: Release? = Bundle.main.url(forResource: "engine-release", withExtension: "json")
        .flatMap { try? Data(contentsOf: $0) }
        .flatMap { try? JSONDecoder().decode(Release.self, from: $0) }

    enum State: Equatable {
        case unavailable       // none published for this build
        case missing
        case downloading(Double)
        case installing
        case ready(URL)        // the engine's folder
        case failed(String)
    }

    @Published private(set) var state: State

    nonisolated static var folder: URL { WheelioPaths.support.appendingPathComponent("Engine", isDirectory: true) }

    private init() {
        state = Self.installedState()
    }

    /// The launcher Steam's compatibility tool runs, when the engine is ready.
    var launcher: URL? {
        if case .ready(let root) = state { return root.appendingPathComponent("run") }
        return nil
    }

    private static func installedState() -> State {
        // A local build, for development: WHEELIO_ENGINE=<unpacked engine folder>.
        if let local = ProcessInfo.processInfo.environment["WHEELIO_ENGINE"], !local.isEmpty {
            return .ready(URL(fileURLWithPath: local, isDirectory: true))
        }
        guard let release else { return .unavailable }
        let root = folder.appendingPathComponent(release.version, isDirectory: true)
        return FileManager.default.fileExists(atPath: root.appendingPathComponent("run").path) ? .ready(root) : .missing
    }

    /// Downloads and installs the pinned engine if it isn't there yet.
    func install() {
        guard let release = Self.release, state == .missing || isFailed else { return }
        state = .downloading(0)
        Task {
            do {
                let archive = try await download(release.url)
                state = .installing
                let root = try await Task.detached { try Self.unpack(archive, release: release) }.value
                state = .ready(root)
            } catch {
                state = .failed(error.localizedDescription)
            }
        }
    }

    private var isFailed: Bool {
        if case .failed = state { return true }
        return false
    }

    private func download(_ url: URL) async throws -> URL {
        let progress = DownloadProgress { [weak self] fraction in
            Task { @MainActor in
                if case .downloading = self?.state { self?.state = .downloading(fraction) }
            }
        }
        let (file, response) = try await URLSession.shared.download(from: url, delegate: progress)
        guard (response as? HTTPURLResponse)?.statusCode == 200 else {
            try? FileManager.default.removeItem(at: file)
            throw URLError(.badServerResponse)
        }
        return file
    }

    /// Verifies the archive, unpacks it beside the folder it replaces and
    /// swaps it in, removing older versions. Returns the engine's folder.
    nonisolated static func unpack(_ archive: URL, release: Release, into folder: URL = folder) throws -> URL {
        let files = FileManager.default
        defer { try? files.removeItem(at: archive) }
        guard try sha256(of: archive) == release.sha256.lowercased() else {
            throw CocoaError(.fileReadCorruptFile, userInfo: [NSLocalizedDescriptionKey:
                "The engine download didn't match its checksum."])
        }
        try files.createDirectory(at: folder, withIntermediateDirectories: true)
        let staging = folder.appendingPathComponent(".\(release.version).new", isDirectory: true)
        try? files.removeItem(at: staging)
        try files.createDirectory(at: staging, withIntermediateDirectories: false)
        let tar = Process()
        tar.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        tar.arguments = ["-xJf", archive.path, "-C", staging.path, "--strip-components", "1"]
        try tar.run()
        tar.waitUntilExit()
        guard tar.terminationStatus == 0, files.fileExists(atPath: staging.appendingPathComponent("run").path) else {
            try? files.removeItem(at: staging)
            throw CocoaError(.fileReadCorruptFile, userInfo: [NSLocalizedDescriptionKey:
                "The engine download couldn't be unpacked."])
        }
        let root = folder.appendingPathComponent(release.version, isDirectory: true)
        try? files.removeItem(at: root)
        try files.moveItem(at: staging, to: root)
        for old in (try? files.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        where old.lastPathComponent != release.version {
            try? files.removeItem(at: old)
        }
        return root
    }

    nonisolated static func sha256(of file: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var hash = SHA256()
        while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
            hash.update(data: chunk)
        }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
}

/// Reports a download's progress, a percent at a time.
private final class DownloadProgress: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let report: (Double) -> Void
    private var reported = 0.0

    init(_ report: @escaping (Double) -> Void) {
        self.report = report
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard totalBytesExpectedToWrite > 0 else { return }
        let fraction = Double(totalBytesWritten) / Double(totalBytesExpectedToWrite)
        if fraction - reported >= 0.01 {
            reported = fraction
            report(fraction)
        }
    }

    // The async download(from:delegate:) hands the file over itself.
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {}
}
