import Combine
import Foundation

/// Runs the bundled VR streamer while VR is on and a game is rendering in VR,
/// and stops it when the game exits. Running it only then keeps the Mac's
/// audio untouched and the headset quiet the rest of the time. Restarted if it
/// crashes mid-game, and after every headset session by design. Status
/// arrives as one JSON object per line on its stdout.
@MainActor
final class StreamerController: ObservableObject {
    static let shared = StreamerController()

    enum State: Equatable {
        case unavailable      // binary not bundled in this build
        case idle             // no game in VR, streamer not running
        case starting
        case waiting          // running, no headset connected
        case connected(fps: Double)
        case failed(String)
    }

    struct Stats: Equatable {
        var fps = 0.0
        var mbps = 0.0
        var encodeMS = 0.0
    }

    @Published private(set) var state: State = .idle
    @Published private(set) var stats = Stats()
    @Published private(set) var audioAvailable = false

    /// A Steam Link headset pairing with this Mac (once, for good): it shows a
    /// PIN, which the player types in here (answerPin).
    struct Pairing: Equatable {
        var device: String
        var pinWasWrong: Bool
    }
    @Published private(set) var pairing: Pairing?
    private var pinWasWrong = false

    private var process: Process?
    private var pipe: Pipe?
    // The streamer stops when its stdin closes, so it is held open while it runs.
    private var input: Pipe?
    private var lineBuffer = Data()
    private var wantRunning = false
    // The game in VR it streams.
    private var game: VRGame?
    private var observers: [AnyCancellable] = []

    private init() {}

    /// Run while a game is in VR; stop otherwise. A new game (a new frames
    /// file) and bitrate and mute changes restart a running streamer so they
    /// apply straight away (the headset reconnects within a few seconds).
    func follow(_ bridge: BridgeController, _ settings: VRSettings) {
        bridge.$vrGame
            .removeDuplicates()
            .sink { [weak self] wanted in
                guard let self else { return }
                let restarting = self.process != nil && wanted != nil
                self.game = wanted
                self.wantRunning = wanted != nil
                if restarting {
                    self.restartIfRunning()
                } else if wanted != nil {
                    self.start()
                } else {
                    self.stop()
                }
            }
            .store(in: &observers)

        settings.$bitrateMbps.map { _ in () }
            .merge(with: settings.$muteMac.map { _ in () })
            .dropFirst(2)
            .debounce(for: .seconds(1), scheduler: DispatchQueue.main)
            .sink { [weak self] _ in self?.restartIfRunning() }
            .store(in: &observers)
    }

    private func restartIfRunning() {
        guard process != nil, wantRunning else { return }
        stop()
        wantRunning = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self, self.wantRunning else { return }
            self.start()
        }
    }

    private var binaryURL: URL? {
        Bundle.main.url(forResource: "wheelio-streamer", withExtension: nil)
    }

    /// The streamer's arguments for this game: its frames file, the eye size
    /// it renders, and the runtime's data folder in the game's Wine
    /// environment (the headset's optics reach the game through it). Controllers stay off: the game is
    /// played with the wheel.
    private static func arguments(for game: VRGame) -> [String] {
        var arguments = ["--frames", game.framesPath,
                         "--eye", "\(game.eyeWidth)x\(game.eyeHeight)",
                         "--controllers", "0"]
        if let data = vrDataDirectory(for: game.framesPath) {
            arguments += ["--data", data.path]
        }
        return arguments + VRSettings.shared.streamerArguments
    }

    /// The runtime's data folder (PURR_VR_DATA, which the engine's launcher
    /// sets for every game): ProgramData/Wheelio/VR in the Wine environment
    /// the frames file is in.
    static func vrDataDirectory(for framesPath: String) -> URL? {
        guard let drive = framesPath.range(of: "/drive_c/") else { return nil }
        return URL(fileURLWithPath: String(framesPath[..<drive.upperBound]) + "ProgramData/Wheelio/VR",
                   isDirectory: true)
    }

    func start() {
        guard process == nil, let game else { return }
        guard let binary = binaryURL else {
            state = .unavailable
            return
        }
        state = .starting

        let process = Process()
        process.executableURL = binary
        process.arguments = Self.arguments(for: game)
        let pipe = Pipe()
        let input = Pipe()
        process.standardInput = input
        process.standardOutput = pipe
        process.standardError = FileHandle.nullDevice

        // Both handlers run on background threads; hop to the main queue with
        // GCD rather than a Task so no actor bookkeeping runs on those threads.
        pipe.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            DispatchQueue.main.async { self?.consume(data) }
        }
        process.terminationHandler = { [weak self] exited in
            DispatchQueue.main.async { self?.processExited(exited) }
        }
        do {
            try process.run()
            self.process = process
            self.pipe = pipe
            self.input = input
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    func stop() {
        guard let process else { return }
        // SIGTERM: the streamer disconnects the headset and releases the audio
        // tap before exiting.
        process.terminate()
        forget()
        state = .idle
    }

    /// Drop the current process and its pipe. At EOF the readability handler
    /// would otherwise keep firing forever.
    private func forget() {
        process = nil
        pipe?.fileHandleForReading.readabilityHandler = nil
        pipe = nil
        try? input?.fileHandleForWriting.close()
        input = nil
        lineBuffer.removeAll()
        stats = Stats()
        pairing = nil
        pinWasWrong = false
    }

    private func processExited(_ exited: Process) {
        // A process stopped by stop(), or replaced by a newer start(), has
        // already been forgotten; its late exit must not touch the new one.
        guard exited === process else { return }
        let exitedCleanly = exited.terminationStatus == 0
        forget()
        // The streamer exits on purpose after every headset session; while the
        // game is still in VR, start the next one straight away.
        if exitedCleanly {
            state = wantRunning ? .starting : .idle
            if wantRunning {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                    guard let self, self.wantRunning else { return }
                    self.start()
                }
            }
            return
        }
        state = .failed("The streamer stopped unexpectedly. Restarting\u{2026}")
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.start() }
    }

    /// The PIN the headset shows, or nil to cancel pairing.
    func answerPin(_ pin: String?) {
        let digits = pin?.filter(\.isNumber) ?? ""
        let line = digits.isEmpty ? "pin cancel\n" : "pin \(digits)\n"
        try? input?.fileHandleForWriting.write(contentsOf: Data(line.utf8))
        if digits.isEmpty { pairing = nil }
    }

    private func consume(_ data: Data) {
        guard !data.isEmpty else { return }  // EOF
        lineBuffer.append(data)
        while let newline = lineBuffer.firstIndex(of: UInt8(ascii: "\n")) {
            let line = lineBuffer[lineBuffer.startIndex..<newline]
            lineBuffer.removeSubrange(lineBuffer.startIndex...newline)
            handle(line: Data(line))
        }
    }

    private func handle(line: Data) {
        guard let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any],
              let event = object["event"] as? String else { return }
        switch event {
        case "waiting":
            audioAvailable = object["audio"] as? Bool ?? false
            state = .waiting
            stats = Stats()
        case "connected":
            state = .connected(fps: object["fps"] as? Double ?? 0)
        case "restarting":
            state = .starting
            stats = Stats()
        case "stats":
            stats = Stats(fps: object["fps"] as? Double ?? 0,
                          mbps: object["mbps"] as? Double ?? 0,
                          encodeMS: object["encode_ms"] as? Double ?? 0)
        case "pin_needed":
            pairing = Pairing(device: object["device"] as? String ?? "", pinWasWrong: pinWasWrong)
            pinWasWrong = false
        case "pin_wrong":
            pinWasWrong = true  // a pin_needed follows
        case "paired", "pin_cancelled":
            pairing = nil
            pinWasWrong = false
        default:
            break
        }
    }
}
