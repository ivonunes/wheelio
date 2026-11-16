import Foundation

/// A Windows game added to Steam as a non-Steam game, which Steam runs
/// through Wheelio like its own Windows games.
struct NonSteamGame: Identifiable, Hashable, Codable {
    /// The shortcut's app ID as Steam writes it in config.vdf and compatdata:
    /// unsigned, with the high bit set.
    let appID: String
    let name: String
    let exe: String
    let startDir: String
    var launchOptions = ""
    /// Written to Steam's shortcuts.vdf yet (it can only be while Steam
    /// isn't running).
    var inSteam = false
    var id: String { appID }
}

/// Steam's non-Steam games, in the shortcuts.vdf of the Steam account last
/// signed in on this Mac. Mac Steam's own Add a Non-Steam Game only takes
/// Mac apps, so Wheelio writes Windows games there itself.
enum SteamShortcuts {
    // A Steam account's 64-bit ID less this is its account ID, the name of
    // its userdata folder.
    private static let individualAccountBase: UInt64 = 76561197960265728
    // Steam gives shortcuts app IDs with the high bit set; below this are
    // IDs Steam keeps for itself.
    private static let lowestAppID: UInt32 = 0x8000_1388

    /// The shortcuts.vdf of the account last signed in, or nil before anyone
    /// has signed in.
    static func file(steamRoot: URL = Steam.root) -> URL? {
        guard let account = accountID(steamRoot: steamRoot) else { return nil }
        return steamRoot.appendingPathComponent("userdata/\(account)/config/shortcuts.vdf")
    }

    static func accountID(steamRoot: URL = Steam.root) -> String? {
        let loginUsers = steamRoot.appendingPathComponent("config/loginusers.vdf")
        if let text = try? String(contentsOf: loginUsers, encoding: .utf8),
           let users = try? KeyValues.parse(text).block("users") {
            for entry in users.entries {
                guard case .block(let user) = entry.value, user.string("MostRecent") == "1",
                      let steamID = UInt64(entry.key), steamID > individualAccountBase else { continue }
                return String(steamID - individualAccountBase)
            }
        }
        // Without a most recent account, the only one there is.
        let userdata = steamRoot.appendingPathComponent("userdata", isDirectory: true)
        let accounts = ((try? FileManager.default.contentsOfDirectory(atPath: userdata.path)) ?? [])
            .filter { !$0.isEmpty && $0 != "0" && $0.allSatisfy(\.isNumber) }
        return accounts.count == 1 ? accounts[0] : nil
    }

    /// The shortcuts in a shortcuts.vdf: an empty list when there's no file.
    static func read(_ url: URL) throws -> BinaryKeyValues {
        guard let data = try? Data(contentsOf: url) else { return BinaryKeyValues() }
        return try BinaryKeyValues.parse(data)
    }

    /// The app IDs of the shortcuts in it.
    static func appIDs(in shortcuts: BinaryKeyValues) -> Set<String> {
        Set(list(shortcuts).compactMap { $0.int32("appid").map { String(UInt32(bitPattern: $0)) } })
    }

    /// A new app ID, unlike any in `taken`.
    static func newAppID(taken: Set<String>) -> String {
        while true {
            let candidate = String(UInt32.random(in: lowestAppID...UInt32.max - 1))
            if !taken.contains(candidate) { return candidate }
        }
    }

    /// The shortcuts with `games` added and those with `removing` app IDs
    /// taken out, numbered again from 0 as Steam numbers them.
    static func updated(_ shortcuts: BinaryKeyValues, adding games: [NonSteamGame],
                        removing: Set<String> = []) -> BinaryKeyValues {
        var kept = list(shortcuts).filter { shortcut in
            guard let appID = shortcut.int32("appid") else { return true }
            return !removing.contains(String(UInt32(bitPattern: appID)))
        }
        kept += games.map(shortcut)
        var result = shortcuts
        let numbered = kept.enumerated().map { BinaryKeyValues.Entry(key: String($0.offset), value: .block($0.element)) }
        result.set("shortcuts", .block(BinaryKeyValues(entries: numbered)))
        return result
    }

    private static func list(_ shortcuts: BinaryKeyValues) -> [BinaryKeyValues] {
        (shortcuts.block("shortcuts")?.entries ?? []).compactMap {
            if case .block(let shortcut) = $0.value { return shortcut }
            return nil
        }
    }

    /// A shortcut as Steam writes one.
    private static func shortcut(for game: NonSteamGame) -> BinaryKeyValues {
        let appID = Int32(bitPattern: UInt32(game.appID) ?? 0)
        return BinaryKeyValues(entries: [
            .init(key: "appid", value: .int32(appID)),
            .init(key: "AppName", value: .string(game.name)),
            .init(key: "Exe", value: .string("\"\(game.exe)\"")),
            .init(key: "StartDir", value: .string("\"\(game.startDir)\"")),
            .init(key: "icon", value: .string("")),
            .init(key: "ShortcutPath", value: .string("")),
            .init(key: "LaunchOptions", value: .string(game.launchOptions)),
            .init(key: "IsHidden", value: .int32(0)),
            .init(key: "AllowDesktopConfig", value: .int32(1)),
            .init(key: "AllowOverlay", value: .int32(1)),
            .init(key: "OpenVR", value: .int32(0)),
            .init(key: "Devkit", value: .int32(0)),
            .init(key: "DevkitGameID", value: .string("")),
            .init(key: "DevkitOverrideAppID", value: .int32(0)),
            .init(key: "LastPlayTime", value: .int32(0)),
            .init(key: "FlatpakAppID", value: .string("")),
            .init(key: "tags", value: .block(BinaryKeyValues())),
        ])
    }
}
