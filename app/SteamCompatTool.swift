import Foundation

/// Wheelio as a Steam Play compatibility tool: a folder in Steam's
/// compatibilitytools.d that Steam reads at start, and Wheelio as the tool
/// Steam uses for Windows games, set in Steam's config.vdf.
///
/// Steam starts the tool, not Wheelio, so the tool's folder holds a small
/// script that passes Wheelio's settings to the engine's launcher.
enum SteamCompatTool {
    // Steam maps Windows save locations into a tool's prefix for Steam Cloud
    // only when the tool's name contains "proton" (the display name isn't
    // checked), and then expects Proton's layout: compatdata/<app>/pfx, with
    // the Windows user "steamuser". The engine's launcher keeps to that.
    static let name = "wheelio_proton"
    static let displayName = "Wheelio"

    /// What the launcher needs from Wheelio, written into the tool's script.
    struct Settings: Equatable {
        var launcher: URL        // the engine's run
        var support: URL         // WHEELIO_SUPPORT: the wheel proxy and VR runtime
        var d3dMetal: URL?       // WHEELIO_D3DMETAL: Apple's toolkit, when added
    }

    static var folder: URL { Steam.compatibilityTools.appendingPathComponent(name, isDirectory: true) }

    // MARK: - The tool's files

    static func compatibilityToolFile() -> String {
        var tools = KeyValues()
        tools.updateBlock(at: ["compatibilitytools", "compat_tools", name]) {
            $0.set("install_path", .string("."))
            $0.set("display_name", .string(displayName))
            $0.set("from_oslist", .string("windows"))
            $0.set("to_oslist", .string("macos"))
        }
        return tools.serialized()
    }

    static func toolManifestFile() -> String {
        var manifest = KeyValues()
        manifest.updateBlock(at: ["manifest"]) {
            $0.set("version", .string("2"))
            $0.set("commandline", .string("/run %verb%"))
        }
        return manifest.serialized()
    }

    static func runScript(_ settings: Settings) -> String {
        var lines = [
            "#!/bin/sh",
            "# Written by Wheelio, and again whenever its settings change.",
            "# Steam carries Wheelio's patch in this variable; games mustn't load it.",
            "unset DYLD_INSERT_LIBRARIES",
            "export WHEELIO_SUPPORT=\(shellQuoted(settings.support.path))",
        ]
        if let d3dMetal = settings.d3dMetal {
            lines.append("export WHEELIO_D3DMETAL=\(shellQuoted(d3dMetal.path))")
        }
        lines.append("exec \(shellQuoted(settings.launcher.path)) \"$@\"")
        return lines.joined(separator: "\n") + "\n"
    }

    private static func shellQuoted(_ text: String) -> String {
        "'" + text.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }

    /// Writes the tool's folder, changing only files whose contents differ.
    static func install(_ settings: Settings, in folder: URL = folder) throws {
        let files = FileManager.default
        try files.createDirectory(at: folder, withIntermediateDirectories: true)
        try write(compatibilityToolFile(), to: folder.appendingPathComponent("compatibilitytool.vdf"))
        try write(toolManifestFile(), to: folder.appendingPathComponent("toolmanifest.vdf"))
        let run = folder.appendingPathComponent("run")
        try write(runScript(settings), to: run)
        try files.setAttributes([.posixPermissions: 0o755], ofItemAtPath: run.path)
    }

    static func uninstall(from folder: URL = folder) {
        try? FileManager.default.removeItem(at: folder)
    }

    private static func write(_ text: String, to url: URL) throws {
        if let existing = try? String(contentsOf: url, encoding: .utf8), existing == text { return }
        try text.write(to: url, atomically: true, encoding: .utf8)
    }

    // MARK: - Steam's choice of tool

    private static let mappingPath = ["InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping"]
    // "0" is Steam's default for every title without a choice of its own; 75
    // is the priority Steam itself gives that default.
    private static let defaultMapping = "0"
    private static let defaultPriority = "75"
    // Steam only looks up a single game's choice at 250 or above (the
    // priority its own Compatibility tab gives).
    private static let gamePriority = "250"

    /// config.vdf with Wheelio as the default tool for Windows-only games
    /// (unless the player chose another default), and as the tool for exactly
    /// the `windowsGames`: games with a Mac version whose Windows version the
    /// player chose. Nil when it's already so. Other tools' choices stay.
    static func configChoosingWheelio(_ text: String, windowsGames: Set<String> = []) throws -> String? {
        var config = try KeyValues.parse(text)
        let original = config
        let mapping = config.block(at: mappingPath) ?? KeyValues()

        // The default: only when there's none, or it's empty.
        if (mapping.block(defaultMapping)?.string("name") ?? "").isEmpty {
            config.updateBlock(at: mappingPath + [defaultMapping]) {
                $0.set("name", .string(name))
                $0.set("config", .string(""))
                $0.set("priority", .string(defaultPriority))
            }
        }
        config.updateBlock(at: mappingPath) { mapping in
            // Games no longer chosen go back to their Mac version.
            for entry in mapping.entries where entry.key != defaultMapping && !windowsGames.contains(entry.key) {
                if case .block(let choice) = entry.value, choice.string("name") == name {
                    mapping.remove(entry.key)
                }
            }
            for game in windowsGames.sorted() {
                mapping.updateBlock(at: [game]) {
                    $0.set("name", .string(name))
                    $0.set("config", .string(""))
                    $0.set("priority", .string(gamePriority))
                }
            }
        }
        return config == original ? nil : config.serialized()
    }

    /// config.vdf with Wheelio no longer chosen anywhere, or nil when it isn't.
    static func configWithoutWheelio(_ text: String) throws -> String? {
        var config = try KeyValues.parse(text)
        guard let mapping = config.block(at: mappingPath) else { return nil }
        let ours = mapping.entries.filter {
            if case .block(let choice) = $0.value { return choice.string("name") == name }
            return false
        }
        guard !ours.isEmpty else { return nil }
        config.updateBlock(at: mappingPath) { mapping in
            for entry in ours { mapping.remove(entry.key) }
        }
        return config.serialized()
    }

    /// Applies a config change. Steam rewrites config.vdf as it quits, so
    /// it's only changed while Steam isn't running; returns false otherwise.
    @discardableResult
    static func updateConfig(at url: URL = Steam.configFile, steamRunning: Bool,
                             _ change: (String) throws -> String?) throws -> Bool {
        guard !steamRunning else { return false }
        let text = try String(contentsOf: url, encoding: .utf8)
        if let updated = try change(text) {
            try updated.write(to: url, atomically: true, encoding: .utf8)
        }
        return true
    }
}
