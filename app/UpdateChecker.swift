import AppKit
import Foundation

/// Checks GitHub Releases for a newer version — no extra infrastructure, the
/// Releases API is the backend. This only *notifies*; it opens the release page
/// for the user to download (full auto-install would need signing + Sparkle).
@MainActor
final class UpdateChecker: ObservableObject {
    static let shared = UpdateChecker()

    /// GitHub repo that hosts the releases. Update if the repo is renamed/moved.
    nonisolated private static let repo = "ivonunes/wheelio"

    enum State: Equatable {
        case idle
        case checking
        case upToDate
        case updateAvailable(version: String, url: URL)
        case failed(String)
    }

    @Published private(set) var state: State = .idle

    var currentVersion: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    }

    func check() {
        if state == .checking { return }
        state = .checking

        let url = URL(string: "https://api.github.com/repos/\(Self.repo)/releases/latest")!
        var request = URLRequest(url: url)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let current = currentVersion

        URLSession.shared.dataTask(with: request) { [weak self] data, _, error in
            let result = Self.parse(data: data, error: error, current: current)
            DispatchQueue.main.async { self?.state = result }
        }.resume()
    }

    func openReleasePage() {
        if case let .updateAvailable(_, url) = state {
            NSWorkspace.shared.open(url)
        }
    }

    // MARK: - Pure helpers (unit-tested)

    nonisolated static func parse(data: Data?, error: Error?, current: String) -> State {
        if let error = error { return .failed(error.localizedDescription) }
        guard let data = data,
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let tag = json["tag_name"] as? String else {
            return .failed("Couldn't read the latest release.")
        }
        let latest = normalizedVersion(tag)
        let pageURL = (json["html_url"] as? String).flatMap(URL.init(string:))
            ?? URL(string: "https://github.com/\(repo)/releases/latest")!
        return isNewer(latest, than: current)
            ? .updateAvailable(version: latest, url: pageURL)
            : .upToDate
    }

    /// Strip a leading "v" (e.g. "v1.2.0" -> "1.2.0").
    nonisolated static func normalizedVersion(_ tag: String) -> String {
        tag.hasPrefix("v") || tag.hasPrefix("V") ? String(tag.dropFirst()) : tag
    }

    nonisolated static func isNewer(_ candidate: String, than current: String) -> Bool {
        compare(candidate, current) == .orderedDescending
    }

    /// Compare dot-separated numeric versions ("1.10.0" > "1.9.9").
    nonisolated static func compare(_ a: String, _ b: String) -> ComparisonResult {
        let pa = a.split(separator: ".").map { Int($0) ?? 0 }
        let pb = b.split(separator: ".").map { Int($0) ?? 0 }
        for i in 0..<max(pa.count, pb.count) {
            let x = i < pa.count ? pa[i] : 0
            let y = i < pb.count ? pb[i] : 0
            if x != y { return x < y ? .orderedAscending : .orderedDescending }
        }
        return .orderedSame
    }
}
