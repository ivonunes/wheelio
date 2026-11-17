import XCTest

final class WinePrefixTests: XCTestCase {
    /// A .lnk as Wine writes one: a local path in its LinkInfo, then a
    /// working folder and arguments as Unicode strings.
    static func link(to target: String, workingDirectory: String? = nil, arguments: String? = nil) -> Data {
        var data = Data()
        func u16(_ value: Int) { data.append(UInt8(value & 0xFF)); data.append(UInt8(value >> 8 & 0xFF)) }
        func u32(_ value: Int) { u16(value & 0xFFFF); u16(value >> 16) }
        var flags = 0x2 | 0x80  // HasLinkInfo, IsUnicode
        if workingDirectory != nil { flags |= 0x10 }
        if arguments != nil { flags |= 0x20 }
        u32(0x4C)
        data.append(Data(count: 16))  // the class ID
        u32(flags)
        data.append(Data(count: 0x4C - data.count))

        let path = Array(target.utf8) + [0]
        let headerSize = 0x1C
        let volume = [UInt8](repeating: 0, count: 16)
        u32(headerSize + volume.count + path.count + 1)  // LinkInfoSize
        u32(headerSize)
        u32(1)  // VolumeIDAndLocalBasePath
        u32(headerSize)  // VolumeIDOffset
        u32(headerSize + volume.count)  // LocalBasePathOffset
        u32(0)
        u32(headerSize + volume.count + path.count)  // CommonPathSuffixOffset: empty
        data.append(contentsOf: volume)
        data.append(contentsOf: path)
        data.append(0)

        for text in [workingDirectory, arguments].compactMap({ $0 }) {
            let units = Array(text.utf16)
            u16(units.count)
            for unit in units { u16(Int(unit)) }
        }
        return data
    }

    func testReadsAShortcut() throws {
        let link = try XCTUnwrap(WindowsShortcut(data: Self.link(
            to: "C:\\Games\\Rally\\rally.exe", workingDirectory: "C:\\Games\\Rally", arguments: "-windowed")))
        XCTAssertEqual(link.target, "C:\\Games\\Rally\\rally.exe")
        XCTAssertEqual(link.workingDirectory, "C:\\Games\\Rally")
        XCTAssertEqual(link.arguments, "-windowed")
        XCTAssertNil(WindowsShortcut(data: Data("not a link".utf8)))
    }

    func testFindsTheGamesAnInstallerAdded() throws {
        let files = FileManager.default
        let root = files.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        defer { try? files.removeItem(at: root) }
        let prefix = WinePrefix(folder: root.appendingPathComponent("pfx", isDirectory: true))
        try files.createDirectory(at: prefix.folder.appendingPathComponent("dosdevices"), withIntermediateDirectories: true)
        try files.createSymbolicLink(atPath: prefix.folder.appendingPathComponent("dosdevices/c:").path,
                                     withDestinationPath: "../drive_c")
        let game = prefix.driveC.appendingPathComponent("Games/Rally", isDirectory: true)
        try files.createDirectory(at: game, withIntermediateDirectories: true)
        for exe in ["rally.exe", "unins000.exe"] {
            try Data().write(to: game.appendingPathComponent(exe))
        }
        let menu = prefix.driveC.appendingPathComponent(
            "ProgramData/Microsoft/Windows/Start Menu/Programs/Rally", isDirectory: true)
        try files.createDirectory(at: menu, withIntermediateDirectories: true)
        try Self.link(to: "C:\\Games\\Rally\\rally.exe", arguments: "-windowed")
            .write(to: menu.appendingPathComponent("Rally.lnk"))
        try Self.link(to: "C:\\Games\\Rally\\unins000.exe").write(to: menu.appendingPathComponent("Uninstall Rally.lnk"))
        try Self.link(to: "C:\\Games\\Rally\\manual.pdf").write(to: menu.appendingPathComponent("Manual.lnk"))
        try Self.link(to: "C:\\Games\\Gone\\gone.exe").write(to: menu.appendingPathComponent("Gone.lnk"))

        let programs = prefix.programs()
        XCTAssertEqual(programs.map(\.name), ["Rally"], "without uninstallers, documents or missing programs")
        XCTAssertEqual(programs.first?.exe.resolvingSymlinksInPath(),
                       game.appendingPathComponent("rally.exe").resolvingSymlinksInPath())
        XCTAssertEqual(programs.first?.startDir.resolvingSymlinksInPath(), game.resolvingSymlinksInPath())
        XCTAssertEqual(programs.first?.arguments, "-windowed")
    }
}
