import XCTest

final class GamePortingToolkitTests: XCTestCase {
    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    /// A toolkit's redist/lib, nested as Apple's image has it. `appleSigned`
    /// uses an Apple-signed binary as the library; otherwise a re-signed copy.
    private func makeToolkit(appleSigned: Bool) throws -> URL {
        let toolkit = root.appendingPathComponent("Game Porting Toolkit", isDirectory: true)
        let lib = toolkit.appendingPathComponent("Evaluation environment/redist/lib", isDirectory: true)
        let files = FileManager.default
        try files.createDirectory(at: lib.appendingPathComponent("external"), withIntermediateDirectories: true)
        try files.createDirectory(at: lib.appendingPathComponent("wine/x86_64-windows"), withIntermediateDirectories: true)
        try Data("PE".utf8).write(to: lib.appendingPathComponent("wine/x86_64-windows/d3d12.dll"))
        let library = lib.appendingPathComponent("external/libd3dshared.dylib")
        try files.copyItem(at: URL(fileURLWithPath: "/usr/bin/true"), to: library)
        if !appleSigned {
            let sign = Process()
            sign.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")
            sign.arguments = ["--force", "--sign", "-", library.path]
            sign.standardError = FileHandle.nullDevice
            try sign.run()
            sign.waitUntilExit()
        }
        return toolkit
    }

    func testFindsTheRedistributablesBelowTheTop() throws {
        let toolkit = try makeToolkit(appleSigned: true)
        XCTAssertEqual(GamePortingToolkit.findRedistributables(in: toolkit)?.lastPathComponent, "lib")
        XCTAssertNil(GamePortingToolkit.findRedistributables(in: root.appendingPathComponent("nothing")))
    }

    func testCopiesAnAppleSignedToolkit() throws {
        let toolkit = try makeToolkit(appleSigned: true)
        let destination = root.appendingPathComponent("Support/GamePortingToolkit", isDirectory: true)
        try GamePortingToolkit.copyRedistributables(from: toolkit, to: destination)
        XCTAssertTrue(GamePortingToolkit.isInstalled(at: destination))
        XCTAssertTrue(FileManager.default.fileExists(
            atPath: destination.appendingPathComponent("wine/x86_64-windows/d3d12.dll").path))
    }

    func testRefusesACopyAppleDidNotSign() throws {
        let toolkit = try makeToolkit(appleSigned: false)
        let destination = root.appendingPathComponent("Support/GamePortingToolkit", isDirectory: true)
        XCTAssertThrowsError(try GamePortingToolkit.copyRedistributables(from: toolkit, to: destination)) { error in
            XCTAssertEqual(error as? GamePortingToolkit.ToolkitError, .notApple)
        }
        XCTAssertFalse(FileManager.default.fileExists(atPath: destination.path))
    }

    func testRefusesSomethingElse() {
        let destination = root.appendingPathComponent("Support/GamePortingToolkit", isDirectory: true)
        XCTAssertThrowsError(try GamePortingToolkit.copyRedistributables(from: root, to: destination)) { error in
            XCTAssertEqual(error as? GamePortingToolkit.ToolkitError, .notFound)
        }
    }
}
