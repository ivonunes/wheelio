import XCTest

// Logic tests for the pure Swift helpers (no app host needed). The test target
// compiles CrossOverSupport.swift and UpdateChecker.swift directly.
final class WheelioTests: XCTestCase {

    // MARK: - PE architecture detection

    private func writePE(machine: UInt16, lfanew: Int = 0x80) throws -> URL {
        var bytes = [UInt8](repeating: 0, count: lfanew + 8)
        bytes[0] = 0x4D; bytes[1] = 0x5A                       // "MZ"
        bytes[0x3C] = UInt8(lfanew & 0xFF)                     // e_lfanew (LE)
        bytes[0x3D] = UInt8((lfanew >> 8) & 0xFF)
        bytes[lfanew] = 0x50; bytes[lfanew + 1] = 0x45         // "PE"
        bytes[lfanew + 2] = 0; bytes[lfanew + 3] = 0           // "\0\0"
        bytes[lfanew + 4] = UInt8(machine & 0xFF)              // machine (LE)
        bytes[lfanew + 5] = UInt8((machine >> 8) & 0xFF)
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString + ".exe")
        try Data(bytes).write(to: url)
        return url
    }

    // MARK: - Bottle environment

    func testBottleEnvironmentIsAddedToTheExistingSection() {
        let config = """
        [Bottle]
        "Name" = "Steam"

        [EnvironmentVariables]
        ;;"PROMPT" = "$p$g"
        "WINEMSYNC" = "1"

        [Other]
        "Key" = "Value"
        """
        let updated = CrossOver.configWithBottleEnvironment(config)
        XCTAssertEqual(updated, """
        [Bottle]
        "Name" = "Steam"

        [EnvironmentVariables]
        ;;"PROMPT" = "$p$g"
        "WINEMSYNC" = "1"
        "SDL_JOYSTICK_MFI" = "0"

        [Other]
        "Key" = "Value"
        """)
        // Already set: nothing to write.
        XCTAssertNil(CrossOver.configWithBottleEnvironment(updated!))
    }

    func testBottleEnvironmentCorrectsAWrongValueAndCreatesAMissingSection() {
        let wrong = "[EnvironmentVariables]\n\"SDL_JOYSTICK_MFI\" = \"1\"\n"
        XCTAssertEqual(CrossOver.configWithBottleEnvironment(wrong),
                       "[EnvironmentVariables]\n\"SDL_JOYSTICK_MFI\" = \"0\"\n")
        let missing = "[Bottle]\n\"Name\" = \"Steam\"\n"
        XCTAssertEqual(CrossOver.configWithBottleEnvironment(missing),
                       "[Bottle]\n\"Name\" = \"Steam\"\n\n[EnvironmentVariables]\n\"SDL_JOYSTICK_MFI\" = \"0\"\n")
    }

    func testPEMachineDetection() throws {
        let cases: [(UInt16, PEMachine)] = [(0x8664, .x64), (0x014C, .x86), (0xAA64, .arm64)]
        for (machine, expected) in cases {
            let url = try writePE(machine: machine)
            defer { try? FileManager.default.removeItem(at: url) }
            XCTAssertEqual(CrossOver.peMachine(of: url), expected, "machine 0x\(String(machine, radix: 16))")
        }
    }

    func testPEMachineNonExecutable() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".txt")
        defer { try? FileManager.default.removeItem(at: url) }
        try Data("not an exe".utf8).write(to: url)
        XCTAssertEqual(CrossOver.peMachine(of: url), .unknown)
    }

    // Malformed executables must be reported as .unknown, never crash the parser.
    func testPEMachineTruncatedHeader() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".exe")
        defer { try? FileManager.default.removeItem(at: url) }
        try Data([0x4D, 0x5A]).write(to: url)  // "MZ" only — too short for the DOS header
        XCTAssertEqual(CrossOver.peMachine(of: url), .unknown)
    }

    func testPEMachineOutOfRangePEOffset() throws {
        var bytes = [UInt8](repeating: 0, count: 0x40)
        bytes[0] = 0x4D; bytes[1] = 0x5A
        bytes[0x3C] = 0xFF; bytes[0x3D] = 0xFF  // e_lfanew points far past EOF
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".exe")
        defer { try? FileManager.default.removeItem(at: url) }
        try Data(bytes).write(to: url)
        XCTAssertEqual(CrossOver.peMachine(of: url), .unknown)
    }

    func testUnsupportedArchitectureMessageIsHelpful() {
        let message = InstallError.unsupportedArchitecture(.x86).errorDescription ?? ""
        XCTAssertTrue(message.contains("32-bit"), "names the detected architecture")
        XCTAssertTrue(message.contains("64-bit"), "states what is supported")
    }

    // MARK: - InstallRecord persistence

    func testInstallRecordRoundTrip() throws {
        let record = InstallRecord(bottleName: "Steam", exePath: "/games/ATS/bin/win_x64/amtrucks.exe",
                                   dllPath: "/games/ATS/bin/win_x64/dinput8.dll")
        let data = try JSONEncoder().encode(record)
        let decoded = try JSONDecoder().decode(InstallRecord.self, from: data)
        XCTAssertEqual(record, decoded)
        XCTAssertEqual(decoded.exeName, "amtrucks.exe")
    }

    // MARK: - Bottle identity

    func testBottleEqualityIsByName() {
        let a = Bottle(name: "Steam", url: URL(fileURLWithPath: "/a/Steam"))
        let b = Bottle(name: "Steam", url: URL(fileURLWithPath: "/b/Steam/"))
        XCTAssertEqual(a, b)
        XCTAssertEqual(a.hashValue, b.hashValue)
    }

    // MARK: - Version comparison / update parsing

    func testVersionComparison() {
        XCTAssertTrue(UpdateChecker.isNewer("1.2.0", than: "1.1.9"))
        XCTAssertTrue(UpdateChecker.isNewer("1.10.0", than: "1.9.9"))
        XCTAssertTrue(UpdateChecker.isNewer("2.0", than: "1.9.9"))
        XCTAssertFalse(UpdateChecker.isNewer("1.0.0", than: "1.0.0"))
        XCTAssertFalse(UpdateChecker.isNewer("1.0.0", than: "1.0.1"))
        XCTAssertEqual(UpdateChecker.normalizedVersion("v2.3.4"), "2.3.4")
        XCTAssertEqual(UpdateChecker.normalizedVersion("2.3.4"), "2.3.4")
    }

    func testParseUpdateAvailable() {
        let json = #"{"tag_name":"v1.2.0","html_url":"https://github.com/x/y/releases/tag/v1.2.0"}"#
        let state = UpdateChecker.parse(data: Data(json.utf8), error: nil, current: "1.0.0")
        guard case let .updateAvailable(version, url) = state else {
            return XCTFail("expected updateAvailable, got \(state)")
        }
        XCTAssertEqual(version, "1.2.0")
        XCTAssertEqual(url.absoluteString, "https://github.com/x/y/releases/tag/v1.2.0")
    }

    func testParseUpToDate() {
        let json = #"{"tag_name":"1.0.0"}"#
        XCTAssertEqual(UpdateChecker.parse(data: Data(json.utf8), error: nil, current: "1.0.0"), .upToDate)
    }
}
