import XCTest

final class SteamPatchTests: XCTestCase {
    private var root: URL!
    private var app: URL!
    private var folder: URL!
    private var library: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        app = root.appendingPathComponent("Steam.app", isDirectory: true)
        folder = root.appendingPathComponent("Support/Steam", isDirectory: true)
        library = root.appendingPathComponent("libwheelio_steam.dylib")

        // A stand-in Steam.app: an Info.plist and an executable codesign accepts.
        let macOS = app.appendingPathComponent("Contents/MacOS", isDirectory: true)
        try FileManager.default.createDirectory(at: macOS, withIntermediateDirectories: true)
        try FileManager.default.copyItem(at: URL(fileURLWithPath: "/usr/bin/true"),
                                         to: macOS.appendingPathComponent("steam_osx"))
        let plist: [String: Any] = ["CFBundleExecutable": "steam_osx", "CFBundleIdentifier": "com.example.steam"]
        try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
            .write(to: app.appendingPathComponent("Contents/Info.plist"))
        try Data("patch".utf8).write(to: library)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    private var installedLibrary: URL { folder.appendingPathComponent("libwheelio_steam.dylib") }

    private func insert() throws -> String? {
        let data = try Data(contentsOf: app.appendingPathComponent("Contents/Info.plist"))
        let plist = try PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any]
        return (plist?["LSEnvironment"] as? [String: Any])?[SteamPatch.insertKey] as? String
    }

    func testInstallKeepsTheOriginalAndPointsSteamAtThePatch() throws {
        XCTAssertEqual(SteamPatch.state(of: app, library: installedLibrary), .notInstalled)
        try SteamPatch.install(app: app, library: library, into: folder)

        XCTAssertEqual(SteamPatch.state(of: app, library: installedLibrary), .installed)
        XCTAssertEqual(try insert(), installedLibrary.path)
        XCTAssertEqual(try Data(contentsOf: installedLibrary), Data("patch".utf8))
        let original = folder.appendingPathComponent("Steam.app/Contents/Info.plist")
        let originalPlist = try PropertyListSerialization.propertyList(from: Data(contentsOf: original), format: nil)
        XCTAssertNil((originalPlist as? [String: Any])?["LSEnvironment"], "the kept copy is as it was")
    }

    func testInstallingAgainOnlyRefreshesTheLibrary() throws {
        try SteamPatch.install(app: app, library: library, into: folder)
        try Data("newer patch".utf8).write(to: library)
        try SteamPatch.install(app: app, library: library, into: folder)
        XCTAssertEqual(try Data(contentsOf: installedLibrary), Data("newer patch".utf8))
        XCTAssertEqual(SteamPatch.state(of: app, library: installedLibrary), .installed)
    }

    func testRemovePutsTheOriginalBack() throws {
        try SteamPatch.install(app: app, library: library, into: folder)
        try SteamPatch.remove(app: app, from: folder)

        XCTAssertEqual(SteamPatch.state(of: app, library: installedLibrary), .notInstalled)
        XCTAssertNil(try insert())
        XCTAssertFalse(FileManager.default.fileExists(atPath: installedLibrary.path))
        XCTAssertFalse(FileManager.default.fileExists(atPath: folder.appendingPathComponent("Steam.app").path))
    }

    func testLeavesAnotherToolsChangeAlone() throws {
        let plistURL = app.appendingPathComponent("Contents/Info.plist")
        var plist = try XCTUnwrap(PropertyListSerialization.propertyList(
            from: Data(contentsOf: plistURL), format: nil) as? [String: Any])
        plist["LSEnvironment"] = [SteamPatch.insertKey: "/somewhere/else.dylib"]
        try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0).write(to: plistURL)

        XCTAssertEqual(SteamPatch.state(of: app, library: installedLibrary), .foreign("/somewhere/else.dylib"))
        XCTAssertThrowsError(try SteamPatch.install(app: app, library: library, into: folder))
        XCTAssertEqual(try insert(), "/somewhere/else.dylib")
    }

    func testReadsWhatThePatchReported() throws {
        let url = root.appendingPathComponent("status.json")
        try Data(#"{"steam_build": "1792618285", "ready": false, "patch_applied": false, "reason": "x", "gates": []}"#.utf8)
            .write(to: url)
        XCTAssertEqual(SteamPatch.status(at: url),
                       SteamPatch.Status(steamBuild: "1792618285", patchApplied: false, reason: "x"))
    }

    func testChecksTheInstalledSteamClient() throws {
        guard FileManager.default.fileExists(atPath: SteamPatch.steamClient.path) else {
            throw XCTSkip("Steam isn't installed here")
        }
        XCTAssertTrue(SteamPatch.check().ready)
        XCTAssertFalse(SteamPatch.check(steamClient: URL(fileURLWithPath: "/usr/bin/true")).ready)
    }
}
