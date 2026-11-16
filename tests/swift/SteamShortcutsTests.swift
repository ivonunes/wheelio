import XCTest

final class SteamShortcutsTests: XCTestCase {
    // A shortcuts.vdf as Steam writes it, with one Mac app.
    private func steamFile() -> Data {
        var data = Data()
        func put(_ type: UInt8, _ key: String) { data.append(type); data.append(contentsOf: Array(key.utf8)); data.append(0) }
        func text(_ value: String) { data.append(contentsOf: Array(value.utf8)); data.append(0) }
        put(0, "shortcuts")
        put(0, "0")
        put(2, "appid"); data.append(contentsOf: [0x39, 0x30, 0x00, 0x90])  // 0x90003039
        put(1, "AppName"); text("Some Mac App")
        put(1, "Exe"); text("\"/Applications/Some.app\"")
        put(3, "Scale"); data.append(contentsOf: [0, 0, 0x80, 0x3F])
        put(7, "Big"); data.append(contentsOf: [1, 2, 3, 4, 5, 6, 7, 8])
        put(0, "tags"); put(1, "0"); text("favorite"); data.append(8)
        data.append(8)  // 0
        data.append(8)  // shortcuts
        data.append(8)  // file
        return data
    }

    func testReadsAndWritesBackByteForByte() throws {
        let data = steamFile()
        let parsed = try BinaryKeyValues.parse(data)
        XCTAssertEqual(parsed.serialized(), data)
        XCTAssertEqual(SteamShortcuts.appIDs(in: parsed), ["2415931449"])
    }

    func testRefusesWhatItCantRead() {
        XCTAssertThrowsError(try BinaryKeyValues.parse(Data([0, 0x61, 0, 9, 0x62, 0])))
        XCTAssertThrowsError(try BinaryKeyValues.parse(Data([0, 0x61, 0, 1, 0x62, 0, 0x63])))
    }

    func testAddsAndRemovesShortcutsKeepingTheRest() throws {
        let original = try BinaryKeyValues.parse(steamFile())
        let game = NonSteamGame(appID: "3000000000", name: "Rally", exe: "/Games/Rally/rally.exe",
                                startDir: "/Games/Rally", launchOptions: "-openxr")
        let added = SteamShortcuts.updated(original, adding: [game])
        XCTAssertEqual(SteamShortcuts.appIDs(in: added), ["2415931449", "3000000000"])
        let shortcut = try XCTUnwrap(added.block("shortcuts")?.block("1"))
        XCTAssertEqual(shortcut.string("AppName"), "Rally")
        XCTAssertEqual(shortcut.string("Exe"), "\"/Games/Rally/rally.exe\"")
        XCTAssertEqual(shortcut.string("StartDir"), "\"/Games/Rally\"")
        XCTAssertEqual(shortcut.string("LaunchOptions"), "-openxr")
        XCTAssertEqual(try BinaryKeyValues.parse(added.serialized()), added)

        let removed = SteamShortcuts.updated(added, adding: [], removing: ["2415931449"])
        XCTAssertEqual(SteamShortcuts.appIDs(in: removed), ["3000000000"])
        XCTAssertNotNil(removed.block("shortcuts")?.block("0"), "numbered again from 0")
    }

    func testStartsAFileWhenThereIsNone() {
        let game = NonSteamGame(appID: "3000000000", name: "Rally", exe: "/r.exe", startDir: "/")
        XCTAssertEqual(SteamShortcuts.appIDs(in: SteamShortcuts.updated(BinaryKeyValues(), adding: [game])), ["3000000000"])
    }

    func testNewAppIDsAreShortcutIDsNotTaken() {
        let id = SteamShortcuts.newAppID(taken: ["2415931449"])
        XCTAssertGreaterThanOrEqual(UInt32(id) ?? 0, 0x8000_1388)
        XCTAssertNotEqual(id, "2415931449")
    }

    func testFindsTheAccountLastSignedIn() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try FileManager.default.createDirectory(at: root.appendingPathComponent("config"), withIntermediateDirectories: true)
        try """
        "users"
        {
        \t"76561197960265729"
        \t{
        \t\t"MostRecent"\t\t"0"
        \t}
        \t"76561198000000000"
        \t{
        \t\t"MostRecent"\t\t"1"
        \t}
        }
        """.write(to: root.appendingPathComponent("config/loginusers.vdf"), atomically: true, encoding: .utf8)
        XCTAssertEqual(SteamShortcuts.accountID(steamRoot: root), "39734272")
        XCTAssertEqual(SteamShortcuts.file(steamRoot: root)?.path,
                       root.appendingPathComponent("userdata/39734272/config/shortcuts.vdf").path)
    }
}
