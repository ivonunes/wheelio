import Foundation

/// Wheelio's Steam Play patch (steam/): a small library Steam loads as it
/// starts, which switches on the Steam Play support Mac Steam already has.
/// It finds what it changes by text in Steam's own code and changes nothing
/// unless all of it is found, then says what it did in status.json.
///
/// Steam only loads it from the variable in its Info.plist once Steam.app is
/// signed locally (Valve's signature doesn't allow it), so Valve's original
/// is kept beside it and put back exactly when the patch is removed. Every
/// change waits until Steam isn't running.
enum SteamPatch {
    enum State: Equatable {
        case notInstalled
        case installed
        // Another tool's library is already in Steam's Info.plist.
        case foreign(String)
    }

    /// What the patch reported the last time Steam started.
    struct Status: Decodable, Equatable {
        let steamBuild: String?
        let patchApplied: Bool
        let reason: String?

        enum CodingKeys: String, CodingKey {
            case steamBuild = "steam_build"
            case patchApplied = "patch_applied"
            case reason
        }
    }

    static let insertKey = "DYLD_INSERT_LIBRARIES"
    private static let environmentKey = "LSEnvironment"
    private static let libraryName = "libwheelio_steam.dylib"

    static var folder: URL { WheelioPaths.support.appendingPathComponent("Steam", isDirectory: true) }
    static var installedLibrary: URL { folder.appendingPathComponent(libraryName) }
    /// Valve's own Steam.app, as it was before the patch.
    static var originalApp: URL { folder.appendingPathComponent("Steam.app", isDirectory: true) }
    static var statusFile: URL { folder.appendingPathComponent("status.json") }

    static var bundledLibrary: URL? {
        Bundle.main.url(forResource: "libwheelio_steam", withExtension: "dylib")
    }

    /// The Steam client library the patch works on.
    static var steamClient: URL {
        Steam.root.appendingPathComponent("Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib")
    }

    // MARK: - State

    static func state(of app: URL = Steam.app, library: URL = installedLibrary) -> State {
        guard let insert = environment(of: app)?[insertKey] as? String, !insert.isEmpty else { return .notInstalled }
        return insert == library.path ? .installed : .foreign(insert)
    }

    static func status(at url: URL = statusFile) -> Status? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(Status.self, from: data)
    }

    /// Whether the patch would apply to the Steam client on disk, without
    /// loading it. The reason when it wouldn't.
    static func check(steamClient: URL = steamClient) -> (ready: Bool, reason: String?) {
        var report = [CChar](repeating: 0, count: Int(WHEELIO_STEAM_REPORT_SIZE))
        let ready = wheelio_steam_check(steamClient.path, &report, report.count)
        let json = String(cString: report)
        let reason = (try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [String: Any])?["reason"] as? String
        return (ready, reason)
    }

    // MARK: - Installing and removing

    enum PatchError: LocalizedError {
        case notBundled
        case foreign(String)
        case command(String)

        var errorDescription: String? {
            switch self {
            case .notBundled: return "The Steam patch isn't included in this build."
            case .foreign: return "Another tool has already changed Steam, so Wheelio leaves it alone."
            case .command(let detail): return detail
            }
        }
    }

    /// Installs or refreshes the patch. Call only while Steam isn't running.
    static func install(app: URL = Steam.app, library: URL? = bundledLibrary, into folder: URL = folder) throws {
        guard let library else { throw PatchError.notBundled }
        let installed = folder.appendingPathComponent(libraryName)
        let files = FileManager.default
        try files.createDirectory(at: folder, withIntermediateDirectories: true)

        switch state(of: app, library: installed) {
        case .foreign(let insert):
            throw PatchError.foreign(insert)
        case .notInstalled:
            // Steam.app as Valve made it (or as Steam's own update left it):
            // keep it to put back.
            let original = folder.appendingPathComponent("Steam.app", isDirectory: true)
            try? files.removeItem(at: original)
            try run("/usr/bin/ditto", [app.path, original.path])
            try copyIfChanged(library, to: installed)
            try setEnvironment(of: app) { $0[insertKey] = installed.path }
            try run("/usr/bin/codesign", ["--force", "--sign", "-", app.path])
        case .installed:
            // Only the library can be out of date (a newer Wheelio).
            try copyIfChanged(library, to: installed)
        }
    }

    /// Puts Valve's Steam.app back. Call only while Steam isn't running.
    static func remove(app: URL = Steam.app, from folder: URL = folder) throws {
        let files = FileManager.default
        let installed = folder.appendingPathComponent(libraryName)
        guard state(of: app, library: installed) == .installed else { return }
        let original = folder.appendingPathComponent("Steam.app", isDirectory: true)
        if files.fileExists(atPath: original.path) {
            let aside = app.deletingLastPathComponent().appendingPathComponent(".Steam.app.\(UUID().uuidString)")
            try files.moveItem(at: app, to: aside)
            do {
                try run("/usr/bin/ditto", [original.path, app.path])
            } catch {
                try? files.moveItem(at: aside, to: app)
                throw error
            }
            try? files.removeItem(at: aside)
            try? files.removeItem(at: original)
        } else {
            // No original kept: take the variable out and sign it again here.
            try setEnvironment(of: app) { $0.removeValue(forKey: insertKey) }
            try run("/usr/bin/codesign", ["--force", "--sign", "-", app.path])
        }
        try? files.removeItem(at: installed)
        try? files.removeItem(at: folder.appendingPathComponent("status.json"))
    }

    // MARK: - Helpers

    private static func infoPlist(of app: URL) -> URL {
        app.appendingPathComponent("Contents/Info.plist")
    }

    private static func environment(of app: URL) -> [String: Any]? {
        guard let data = try? Data(contentsOf: infoPlist(of: app)),
              let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
        else { return nil }
        return plist[environmentKey] as? [String: Any]
    }

    private static func setEnvironment(of app: URL, _ change: (inout [String: Any]) -> Void) throws {
        let url = infoPlist(of: app)
        var format = PropertyListSerialization.PropertyListFormat.xml
        let data = try Data(contentsOf: url)
        guard var plist = try PropertyListSerialization.propertyList(from: data, format: &format) as? [String: Any] else {
            throw PatchError.command("Steam's Info.plist couldn't be read.")
        }
        var environment = plist[environmentKey] as? [String: Any] ?? [:]
        change(&environment)
        plist[environmentKey] = environment.isEmpty ? nil : environment
        try PropertyListSerialization.data(fromPropertyList: plist, format: format, options: 0)
            .write(to: url, options: .atomic)
    }

    private static func copyIfChanged(_ source: URL, to destination: URL) throws {
        let files = FileManager.default
        if let current = try? Data(contentsOf: destination), current == (try Data(contentsOf: source)) { return }
        try? files.removeItem(at: destination)
        try files.copyItem(at: source, to: destination)
    }

    private static func run(_ tool: String, _ arguments: [String]) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: tool)
        process.arguments = arguments
        let errors = Pipe()
        process.standardError = errors
        process.standardOutput = FileHandle.nullDevice
        try process.run()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            let detail = String(decoding: errors.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
            throw PatchError.command("\(URL(fileURLWithPath: tool).lastPathComponent) failed: \(detail)")
        }
    }
}
