import XCTest

final class KeyValuesTests: XCTestCase {
    private let config = """
    "InstallConfigStore"
    {
    \t"Software"
    \t{
    \t\t"Valve"
    \t\t{
    \t\t\t"Steam"
    \t\t\t{
    \t\t\t\t"AutoUpdateWindowEnabled"\t\t"0"
    \t\t\t\t// A comment Steam doesn't write, but may be there.
    \t\t\t\t"Path"\t\t"C:\\\\Games \\"Steam\\""
    \t\t\t}
    \t\t}
    \t}
    }

    """

    func testParsesNestedBlocksStringsAndEscapes() throws {
        let parsed = try KeyValues.parse(config)
        let steam = try XCTUnwrap(parsed.block(at: ["InstallConfigStore", "Software", "Valve", "Steam"]))
        XCTAssertEqual(steam.string("AutoUpdateWindowEnabled"), "0")
        XCTAssertEqual(steam.string("path"), #"C:\Games "Steam""#, "keys match without regard to case")
    }

    func testWritesBackAsSteamDoes() throws {
        let parsed = try KeyValues.parse(config)
        let written = parsed.serialized()
        XCTAssertEqual(try KeyValues.parse(written), parsed, "what it writes reads back the same")
        XCTAssertTrue(written.hasPrefix("\"InstallConfigStore\"\n{\n\t\"Software\"\n\t{\n"), written)
        XCTAssertTrue(written.contains("\t\t\t\t\"Path\"\t\t\"C:\\\\Games \\\"Steam\\\"\"\n"), written)
    }

    func testUpdatingMakesMissingBlocksAndKeepsTheRest() throws {
        var parsed = try KeyValues.parse(config)
        parsed.updateBlock(at: ["InstallConfigStore", "Software", "Valve", "Steam", "CompatToolMapping", "0"]) {
            $0.set("name", .string("wheelio"))
        }
        let steam = try XCTUnwrap(parsed.block(at: ["InstallConfigStore", "Software", "Valve", "Steam"]))
        XCTAssertEqual(steam.block(at: ["CompatToolMapping", "0"])?.string("name"), "wheelio")
        XCTAssertEqual(steam.string("AutoUpdateWindowEnabled"), "0", "untouched entries stay")
        XCTAssertEqual(steam.entries.first?.key, "AutoUpdateWindowEnabled", "and keep their order")
    }

    func testRefusesWhatItCannotReadFully() {
        XCTAssertThrowsError(try KeyValues.parse("\"a\"\n{\n\t\"b\"\t\"c\"\n")) { error in
            XCTAssertEqual(error as? KeyValues.ParseError, .unexpectedEnd)
        }
        XCTAssertThrowsError(try KeyValues.parse("\"a\" \"b\" [$WIN32]")) { error in
            XCTAssertEqual(error as? KeyValues.ParseError, .unsupportedConditional(line: 1))
        }
    }
}
