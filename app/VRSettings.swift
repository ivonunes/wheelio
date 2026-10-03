import Combine
import Foundation

/// VR streaming settings. Off on a fresh install: the OpenXR runtime is only
/// placed next to games while VR is on, so with it off a game sees no VR at all.
@MainActor
final class VRSettings: ObservableObject {
    static let shared = VRSettings()

    enum Codec: String, CaseIterable, Identifiable {
        case hevc
        case h264
        var id: String { rawValue }
        var displayName: String {
            switch self {
            case .hevc: return "HEVC"
            case .h264: return "H.264"
            }
        }
    }

    @Published private(set) var enabled: Bool
    @Published private(set) var codec: Codec
    @Published private(set) var bitrateMbps: Int
    @Published private(set) var muteMac: Bool
    /// Set while enabling/disabling touches the installed games.
    @Published private(set) var applying = false
    @Published private(set) var lastError: String?

    private static let enabledKey = "vrEnabled"
    private static let codecKey = "vrCodec"
    private static let bitrateKey = "vrBitrateMbps"
    private static let muteKey = "vrMuteMac"

    static let bitrateRange = 10...100

    private init() {
        let defaults = UserDefaults.standard
        enabled = defaults.bool(forKey: Self.enabledKey)
        codec = Codec(rawValue: defaults.string(forKey: Self.codecKey) ?? "") ?? .hevc
        let storedBitrate = defaults.integer(forKey: Self.bitrateKey)
        bitrateMbps = Self.bitrateRange.contains(storedBitrate) ? storedBitrate : 30
        muteMac = defaults.object(forKey: Self.muteKey) == nil ? true : defaults.bool(forKey: Self.muteKey)
    }

    /// Turn VR on or off. Installs or removes the OpenXR runtime for every game
    /// set up in this app, so the change applies without redoing setup.
    func setEnabled(_ value: Bool) {
        guard value != enabled, !applying else { return }
        enabled = value
        UserDefaults.standard.set(value, forKey: Self.enabledKey)
        applying = true
        lastError = nil
        Task.detached {
            let failures = CrossOver.applyVR(enabled: value)
            await MainActor.run {
                self.applying = false
                if !failures.isEmpty {
                    self.lastError = failures.joined(separator: "\n")
                }
            }
        }
    }

    func setCodec(_ value: Codec) {
        codec = value
        UserDefaults.standard.set(value.rawValue, forKey: Self.codecKey)
    }

    func setBitrate(_ value: Int) {
        let clamped = min(max(value, Self.bitrateRange.lowerBound), Self.bitrateRange.upperBound)
        bitrateMbps = clamped
        UserDefaults.standard.set(clamped, forKey: Self.bitrateKey)
    }

    func setMuteMac(_ value: Bool) {
        muteMac = value
        UserDefaults.standard.set(value, forKey: Self.muteKey)
    }

    /// Command-line arguments the streamer takes.
    var streamerArguments: [String] {
        ["--codec", codec.rawValue, "--bitrate", String(bitrateMbps), "--mute", muteMac ? "1" : "0"]
    }
}
