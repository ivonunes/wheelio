import Foundation

/// A Steam game, by its app ID.
struct SteamGame: Identifiable, Hashable, Codable {
    let appID: String
    let name: String
    var id: String { appID }
}

/// Finding Steam games for the player to choose from: those installed in
/// Steam's libraries, or any other by its store link or app ID.
enum SteamGames {
    // Steam's own packages, which appear as installed apps.
    private static let notGames: Set<String> = ["228980"]  // Steamworks Common Redistributables

    /// The games installed in Steam's libraries, by name.
    static func installed(libraries: [URL] = Steam.libraryFolders()) -> [SteamGame] {
        var games: [String: SteamGame] = [:]
        for library in libraries {
            let steamapps = library.appendingPathComponent("steamapps", isDirectory: true)
            let manifests = (try? FileManager.default.contentsOfDirectory(at: steamapps, includingPropertiesForKeys: nil)) ?? []
            for manifest in manifests where manifest.lastPathComponent.hasPrefix("appmanifest_")
                && manifest.pathExtension == "acf" {
                guard let text = try? String(contentsOf: manifest, encoding: .utf8),
                      let state = try? KeyValues.parse(text).block("AppState"),
                      let appID = state.string("appid"), let name = state.string("name"),
                      !notGames.contains(appID) else { continue }
                games[appID] = SteamGame(appID: appID, name: name)
            }
        }
        return games.values.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    /// The app ID in a Steam store link (store.steampowered.com/app/<id>/...),
    /// a steam:// link, or the ID itself.
    static func appID(from text: String) -> String? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty, trimmed.allSatisfy(\.isNumber) { return trimmed }
        let parts = trimmed.split(whereSeparator: { "/?#".contains($0) }).map(String.init)
        for (index, part) in parts.enumerated() where ["app", "rungameid", "run", "install"].contains(part.lowercased()) {
            if index + 1 < parts.count, !parts[index + 1].isEmpty, parts[index + 1].allSatisfy(\.isNumber) {
                return parts[index + 1]
            }
        }
        return nil
    }

    /// A game's name from Steam's store, or nil when it can't say.
    static func name(of appID: String) async -> String? {
        guard var components = URLComponents(string: "https://store.steampowered.com/api/appdetails") else { return nil }
        components.queryItems = [URLQueryItem(name: "appids", value: appID), URLQueryItem(name: "filters", value: "basic")]
        guard let url = components.url,
              let (data, _) = try? await URLSession.shared.data(from: url),
              let reply = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let app = reply[appID] as? [String: Any], app["success"] as? Bool == true,
              let details = app["data"] as? [String: Any] else { return nil }
        return details["name"] as? String
    }
}
