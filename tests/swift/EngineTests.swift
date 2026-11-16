import XCTest

final class EngineTests: XCTestCase {
    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: root)
    }

    /// An archive shaped like a published engine: one top folder holding run.
    private func makeArchive() throws -> URL {
        let top = root.appendingPathComponent("source/wheelio-engine-26.3.0-1", isDirectory: true)
        try FileManager.default.createDirectory(at: top.appendingPathComponent("bin"), withIntermediateDirectories: true)
        try "#!/bin/sh\n".write(to: top.appendingPathComponent("run"), atomically: true, encoding: .utf8)
        let archive = root.appendingPathComponent("engine.tar.xz")
        let tar = Process()
        tar.executableURL = URL(fileURLWithPath: "/usr/bin/tar")
        tar.arguments = ["-cJf", archive.path, "-C", top.deletingLastPathComponent().path, top.lastPathComponent]
        try tar.run()
        tar.waitUntilExit()
        XCTAssertEqual(tar.terminationStatus, 0)
        return archive
    }

    func testUnpacksAVerifiedEngineAndKeepsOnlyThatVersion() throws {
        let archive = try makeArchive()
        let release = Engine.Release(version: "26.3.0-1", url: URL(string: "https://example.invalid")!,
                                     sha256: try Engine.sha256(of: archive), source: "")
        let folder = root.appendingPathComponent("Engine", isDirectory: true)
        // An older engine whose DLLs are read-only, as the engine ships them.
        let old = folder.appendingPathComponent("26.2.0-4/lib/wine/x86_64-windows", isDirectory: true)
        try FileManager.default.createDirectory(at: old, withIntermediateDirectories: true)
        try Data("MZ".utf8).write(to: old.appendingPathComponent("kernel32.dll"))
        try FileManager.default.setAttributes([.posixPermissions: 0o444],
                                              ofItemAtPath: old.appendingPathComponent("kernel32.dll").path)

        let installed = try Engine.unpack(archive, release: release, into: folder)

        XCTAssertEqual(installed.lastPathComponent, "26.3.0-1")
        XCTAssertTrue(FileManager.default.fileExists(atPath: installed.appendingPathComponent("run").path))
        XCTAssertEqual(try FileManager.default.contentsOfDirectory(atPath: folder.path), ["26.3.0-1"],
                       "older versions go")
        XCTAssertFalse(FileManager.default.fileExists(atPath: archive.path), "the download is removed")
    }

    func testRefusesAnArchiveThatDoesNotMatchItsChecksum() throws {
        let archive = try makeArchive()
        let release = Engine.Release(version: "26.3.0-1", url: URL(string: "https://example.invalid")!,
                                     sha256: String(repeating: "0", count: 64), source: "")
        let folder = root.appendingPathComponent("Engine", isDirectory: true)

        XCTAssertThrowsError(try Engine.unpack(archive, release: release, into: folder))
        XCTAssertFalse(FileManager.default.fileExists(atPath: folder.appendingPathComponent("26.3.0-1").path))
    }

    func testReadsTheBundledPin() throws {
        let json = #"{"version": "26.3.0-1", "url": "https://github.com/ivonunes/wheelio/releases/download/engine-26.3.0-1/wheelio-engine-26.3.0-1.tar.xz", "sha256": "e56a", "source": "4b825dc6"}"#
        let release = try JSONDecoder().decode(Engine.Release.self, from: Data(json.utf8))
        XCTAssertEqual(release.version, "26.3.0-1")
        XCTAssertEqual(release.url.lastPathComponent, "wheelio-engine-26.3.0-1.tar.xz")
        XCTAssertEqual(release.source, "4b825dc6")
    }
}
