import AppKit

/// The Mac Steam client's files and process.
enum Steam {
    static let bundleIdentifier = "com.valvesoftware.steam"

    static var app: URL { URL(fileURLWithPath: "/Applications/Steam.app", isDirectory: true) }

    /// ~/Library/Application Support/Steam
    static var root: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Steam", isDirectory: true)
    }

    static var configFile: URL { root.appendingPathComponent("config/config.vdf") }
    static var libraryFoldersFile: URL { root.appendingPathComponent("config/libraryfolders.vdf") }
    static var compatibilityTools: URL { root.appendingPathComponent("compatibilitytools.d", isDirectory: true) }

    /// Steam's library folders (the main one first), where games and, for
    /// compatibility tools, each game's compatdata live.
    static func libraryFolders(file: URL = libraryFoldersFile) -> [URL] {
        guard let text = try? String(contentsOf: file, encoding: .utf8),
              let folders = try? KeyValues.parse(text).block("libraryfolders") else { return [] }
        return folders.entries.compactMap { entry in
            guard case .block(let folder) = entry.value, let path = folder.string("path") else { return nil }
            return URL(fileURLWithPath: path, isDirectory: true)
        }
    }

    static var isInstalled: Bool { FileManager.default.fileExists(atPath: app.path) }

    static var isRunning: Bool {
        !NSRunningApplication.runningApplications(withBundleIdentifier: bundleIdentifier).isEmpty
    }
}
