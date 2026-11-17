import XCTest

final class SteamGamesTests: XCTestCase {
    func testReadsInstalledGamesFromEveryLibrary() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: root) }
        func install(_ appID: String, _ name: String, in library: String) throws {
            let steamapps = root.appendingPathComponent("\(library)/steamapps", isDirectory: true)
            try FileManager.default.createDirectory(at: steamapps, withIntermediateDirectories: true)
            try "\"AppState\"\n{\n\t\"appid\"\t\t\"\(appID)\"\n\t\"name\"\t\t\"\(name)\"\n}\n"
                .write(to: steamapps.appendingPathComponent("appmanifest_\(appID).acf"), atomically: true, encoding: .utf8)
        }
        try install("270880", "American Truck Simulator", in: "Main")
        try install("227300", "Euro Truck Simulator 2", in: "Games HD")
        try install("228980", "Steamworks Common Redistributables", in: "Main")

        let games = SteamGames.installed(libraries: [root.appendingPathComponent("Main"),
                                                     root.appendingPathComponent("Games HD")])
        XCTAssertEqual(games.map(\.name), ["American Truck Simulator", "Euro Truck Simulator 2"],
                       "sorted by name, without Steam's own packages")
    }

    func testFindsTheAppIDInLinksAndIDs() {
        XCTAssertEqual(SteamGames.appID(from: "227300"), "227300")
        XCTAssertEqual(SteamGames.appID(from: " https://store.steampowered.com/app/227300/Euro_Truck_Simulator_2/ "), "227300")
        XCTAssertEqual(SteamGames.appID(from: "store.steampowered.com/app/270880"), "270880")
        XCTAssertEqual(SteamGames.appID(from: "steam://rungameid/227300"), "227300")
        XCTAssertNil(SteamGames.appID(from: "Euro Truck Simulator 2"))
        XCTAssertNil(SteamGames.appID(from: ""))
    }
}
