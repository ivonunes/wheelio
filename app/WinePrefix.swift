import Foundation

/// A game's Wine environment (its prefix, compatdata/<app id>/pfx in Steam's
/// layout), read from the Mac side.
struct WinePrefix {
    /// A program an installer added to the Start menu or the desktop.
    struct Program: Hashable, Identifiable {
        let name: String
        let exe: URL
        let startDir: URL
        let arguments: String
        var id: String { exe.path + "\0" + arguments }
    }

    let folder: URL

    /// The prefix Steam gives a game, in Steam's main library.
    static func forSteamApp(_ appID: String, steamRoot: URL = Steam.root) -> WinePrefix {
        WinePrefix(folder: compatData(appID, steamRoot: steamRoot).appendingPathComponent("pfx", isDirectory: true))
    }

    static func compatData(_ appID: String, steamRoot: URL = Steam.root) -> URL {
        steamRoot.appendingPathComponent("steamapps/compatdata/\(appID)", isDirectory: true)
    }

    var driveC: URL { folder.appendingPathComponent("drive_c", isDirectory: true) }

    /// Where a Windows path ("C:\Games\game.exe") is on the Mac, through the
    /// prefix's drive letters.
    func macPath(for windowsPath: String) -> URL? {
        let parts = windowsPath.split(separator: "\\", omittingEmptySubsequences: true).map(String.init)
        guard let drive = parts.first, drive.count == 2, drive.hasSuffix(":") else { return nil }
        let device = folder.appendingPathComponent("dosdevices/\(drive.lowercased())")
        guard let root = try? FileManager.default.destinationOfSymbolicLink(atPath: device.path) else { return nil }
        let base = root.hasPrefix("/") ? URL(fileURLWithPath: root, isDirectory: true)
                                       : device.deletingLastPathComponent().appendingPathComponent(root, isDirectory: true)
        return parts.dropFirst().reduce(base.standardizedFileURL) { $0.appendingPathComponent($1) }
    }

    /// The programs in the Start menu and on the desktop, without
    /// uninstallers or anything that isn't a Windows program, by name.
    func programs() -> [Program] {
        let places = [
            "drive_c/ProgramData/Microsoft/Windows/Start Menu",
            "drive_c/users/steamuser/AppData/Roaming/Microsoft/Windows/Start Menu",
            "drive_c/users/steamuser/Desktop",
            "drive_c/users/Public/Desktop",
        ]
        var found: [String: Program] = [:]
        for place in places {
            let root = folder.appendingPathComponent(place, isDirectory: true)
            // A folder linked to the Mac's own (Wine links the desktop) isn't the installer's.
            let isLink = (try? root.resourceValues(forKeys: [.isSymbolicLinkKey]))?.isSymbolicLink ?? false
            guard !isLink, let files = FileManager.default.enumerator(at: root, includingPropertiesForKeys: nil) else { continue }
            for case let file as URL in files where file.pathExtension.lowercased() == "lnk" {
                if let program = program(at: file) { found[program.id] = program }
            }
        }
        return found.values.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    private func program(at file: URL) -> Program? {
        guard let data = try? Data(contentsOf: file), let link = WindowsShortcut(data: data) else { return nil }
        let exe: URL?
        if let target = link.target {
            exe = macPath(for: target)
        } else if let relative = link.relativePath {
            exe = relative.split(separator: "\\").reduce(file.deletingLastPathComponent()) {
                $0.appendingPathComponent(String($1))
            }.standardizedFileURL
        } else {
            exe = nil
        }
        let name = file.deletingPathExtension().lastPathComponent
        guard let exe, exe.pathExtension.lowercased() == "exe",
              FileManager.default.fileExists(atPath: exe.path),
              !Self.isUninstaller(name), !Self.isUninstaller(exe.lastPathComponent) else { return nil }
        let startDir = link.workingDirectory.flatMap(macPath(for:)) ?? exe.deletingLastPathComponent()
        return Program(name: name, exe: exe, startDir: startDir, arguments: link.arguments ?? "")
    }

    private static func isUninstaller(_ name: String) -> Bool {
        let lower = name.lowercased()
        return lower.contains("uninst") || lower.contains("deinstall")
    }
}
