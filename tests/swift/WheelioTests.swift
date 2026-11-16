import XCTest

// Logic tests for the update checker (no app host needed).
final class WheelioTests: XCTestCase {

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
