import Combine
import Foundation

/// VR streaming settings. A game is in VR when its launch options ask for
/// OpenXR (any argument containing "openxr"); the engine's launcher gives it
/// the runtime then, and the streamer runs while it renders.
@MainActor
final class VRSettings: ObservableObject {
    static let shared = VRSettings()

    @Published private(set) var bitrateMbps: Int
    @Published private(set) var muteMac: Bool

    private static let bitrateKey = "vrBitrateMbps"
    private static let muteKey = "vrMuteMac"

    // The streamer's own range and default (Purr's options.rs).
    static let bitrateRange = 5...200
    private static let defaultBitrate = 100

    private init() {
        let defaults = UserDefaults.standard
        let storedBitrate = defaults.integer(forKey: Self.bitrateKey)
        bitrateMbps = Self.bitrateRange.contains(storedBitrate) ? storedBitrate : Self.defaultBitrate
        muteMac = defaults.object(forKey: Self.muteKey) == nil ? true : defaults.bool(forKey: Self.muteKey)
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
        ["--bitrate", String(bitrateMbps), "--mute", muteMac ? "1" : "0"]
    }
}
