import XCTest

final class SteamCompatToolTests: XCTestCase {
    private let settings = SteamCompatTool.Settings(
        launcher: URL(fileURLWithPath: "/Users/me/Library/Application Support/Wheelio/Engine/run"),
        support: URL(fileURLWithPath: "/Applications/Wheelio's Apps/Wheelio.app/Contents/Resources"),
        d3dMetal: nil)

    private func config(_ mapping: String = "") -> String {
        """
        "InstallConfigStore"
        {
        \t"Software"
        \t{
        \t\t"Valve"
        \t\t{
        \t\t\t"Steam"
        \t\t\t{
        \t\t\t\t"AutoUpdateWindowEnabled"\t\t"0"
        \(mapping)\t\t\t}
        \t\t}
        \t}
        }

        """
    }

    private func mapping(_ entries: [(String, String)]) -> String {
        var text = "\t\t\t\t\"CompatToolMapping\"\n\t\t\t\t{\n"
        for (app, tool) in entries {
            text += "\t\t\t\t\t\"\(app)\"\n\t\t\t\t\t{\n\t\t\t\t\t\t\"name\"\t\t\"\(tool)\"\n"
            text += "\t\t\t\t\t\t\"config\"\t\t\"\"\n\t\t\t\t\t\t\"priority\"\t\t\"250\"\n\t\t\t\t\t}\n"
        }
        return text + "\t\t\t\t}\n"
    }

    private func defaultTool(in text: String) throws -> String? {
        try KeyValues.parse(text)
            .block(at: ["InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping", "0"])?
            .string("name")
    }

    func testToolFilesNameWheelioAndItsLauncher() throws {
        let tools = try KeyValues.parse(SteamCompatTool.compatibilityToolFile())
        let tool = try XCTUnwrap(tools.block(at: ["compatibilitytools", "compat_tools", "wheelio_proton"]))
        XCTAssertEqual(tool.string("display_name"), "Wheelio")
        XCTAssertEqual(tool.string("from_oslist"), "windows")

        let manifest = try KeyValues.parse(SteamCompatTool.toolManifestFile())
        XCTAssertEqual(manifest.block("manifest")?.string("commandline"), "/run %verb%")
    }

    func testRunScriptPassesSettingsSafelyQuoted() {
        let script = SteamCompatTool.runScript(settings)
        XCTAssertTrue(script.hasPrefix("#!/bin/sh\n"))
        XCTAssertTrue(script.contains("\nunset DYLD_INSERT_LIBRARIES\n"), "the Steam patch stays out of games")
        XCTAssertTrue(script.contains("export WHEELIO_SUPPORT='/Applications/Wheelio'\\''s Apps/Wheelio.app/Contents/Resources'\n"), script)
        XCTAssertFalse(script.contains("WHEELIO_D3DMETAL"), "no toolkit, no variable")
        XCTAssertTrue(script.hasSuffix("exec '/Users/me/Library/Application Support/Wheelio/Engine/run' \"$@\"\n"))
    }

    func testInstallWritesAnExecutableLauncher() throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { SteamCompatTool.uninstall(from: folder) }
        try SteamCompatTool.install(settings, in: folder)
        let run = folder.appendingPathComponent("run")
        XCTAssertTrue(FileManager.default.isExecutableFile(atPath: run.path))
        XCTAssertTrue(FileManager.default.fileExists(atPath: folder.appendingPathComponent("toolmanifest.vdf").path))
        SteamCompatTool.uninstall(from: folder)
        XCTAssertFalse(FileManager.default.fileExists(atPath: folder.path))
    }

    func testBecomesTheDefaultOnlyWhenThereIsNone() throws {
        let chosen = try XCTUnwrap(SteamCompatTool.configChoosingWheelio(config()))
        XCTAssertEqual(try defaultTool(in: chosen), "wheelio_proton")
        XCTAssertNil(try SteamCompatTool.configChoosingWheelio(chosen), "already the default")

        let otherDefault = config(mapping([("0", "some_other_tool")]))
        XCTAssertNil(try SteamCompatTool.configChoosingWheelio(otherDefault), "the player's default stays")

        let perGame = config(mapping([("440", "some_other_tool")]))
        let withDefault = try XCTUnwrap(SteamCompatTool.configChoosingWheelio(perGame))
        XCTAssertEqual(try KeyValues.parse(withDefault)
            .block(at: ["InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping", "440"])?
            .string("name"), "some_other_tool", "a game's own choice stays")
    }

    func testRemovingWheelioLeavesOtherChoices() throws {
        let text = config(mapping([("0", "wheelio_proton"), ("440", "wheelio_proton"), ("570", "some_other_tool")]))
        let removed = try XCTUnwrap(SteamCompatTool.configWithoutWheelio(text))
        let mapping = try XCTUnwrap(KeyValues.parse(removed)
            .block(at: ["InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping"]))
        XCTAssertEqual(mapping.entries.map(\.key), ["570"])
        XCTAssertNil(try SteamCompatTool.configWithoutWheelio(removed), "nothing left to remove")
    }

    func testConfigIsOnlyChangedWhileSteamIsClosed() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: url) }
        try config().write(to: url, atomically: true, encoding: .utf8)

        XCTAssertFalse(try SteamCompatTool.updateConfig(at: url, steamRunning: true) { try SteamCompatTool.configChoosingWheelio($0) })
        XCTAssertNil(try defaultTool(in: String(contentsOf: url, encoding: .utf8)))
        XCTAssertTrue(try SteamCompatTool.updateConfig(at: url, steamRunning: false) { try SteamCompatTool.configChoosingWheelio($0) })
        XCTAssertEqual(try defaultTool(in: String(contentsOf: url, encoding: .utf8)), "wheelio_proton")
    }

    func testChosenGamesUseWheelioAndUnchosenOnesGoBack() throws {
        let text = config(mapping([("0", "wheelio_proton"), ("440", "some_other_tool"), ("570", "wheelio_proton")]))
        let chosen = try XCTUnwrap(SteamCompatTool.configChoosingWheelio(text, windowsGames: ["227300"]))
        let mapping = try XCTUnwrap(KeyValues.parse(chosen)
            .block(at: ["InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping"]))
        XCTAssertEqual(mapping.block("227300")?.string("name"), "wheelio_proton")
        XCTAssertEqual(mapping.block("227300")?.string("priority"), "250", "Steam only reads a game's own choice at 250")
        XCTAssertNil(mapping.block("570"), "a game no longer chosen goes back to its Mac version")
        XCTAssertEqual(mapping.block("440")?.string("name"), "some_other_tool", "other tools' choices stay")
        XCTAssertEqual(mapping.block("0")?.string("name"), "wheelio_proton")
        XCTAssertNil(try SteamCompatTool.configChoosingWheelio(chosen, windowsGames: ["227300"]), "nothing left to change")
    }
}
