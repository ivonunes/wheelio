import Combine
import Foundation

/// Runs the bundled VR streamer while VR is on and a game is rendering in VR,
/// and stops it when the game exits. Running it only then keeps the Mac's
/// audio untouched and ALVR quiet the rest of the time. Restarted if it
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
        case connected(codec: String, fps: Double)
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

    private var process: Process?
    private var pipe: Pipe?
    private var lineBuffer = Data()
    private var wantRunning = false
    private var observers: [AnyCancellable] = []

    private init() {}

    /// Run while VR is on and a game is in VR; stop otherwise. Codec, bitrate
    /// and mute changes restart a running streamer so they apply straight
    /// away (the headset reconnects within a few seconds).
    func follow(_ bridge: BridgeController, _ settings: VRSettings) {
        bridge.$vrGameRunning
            .combineLatest(settings.$enabled)
            .map { running, enabled in running && enabled }
            .removeDuplicates()
            .sink { [weak self] wanted in
                guard let self else { return }
                self.wantRunning = wanted
                if wanted { self.start() } else { self.stop() }
            }
            .store(in: &observers)

        settings.$codec.map { _ in () }
            .merge(with: settings.$bitrateMbps.map { _ in () }, settings.$muteMac.map { _ in () })
            .dropFirst(3)
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

    func start() {
        guard process == nil else { return }
        guard let binary = binaryURL else {
            state = .unavailable
            return
        }
        state = .starting

        let process = Process()
        process.executableURL = binary
        process.arguments = VRSettings.shared.streamerArguments
        let pipe = Pipe()
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
        lineBuffer.removeAll()
        stats = Stats()
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
            state = .connected(codec: object["codec"] as? String ?? "",
                               fps: object["fps"] as? Double ?? 0)
        case "restarting":
            state = .starting
            stats = Stats()
        case "stats":
            stats = Stats(fps: object["fps"] as? Double ?? 0,
                          mbps: object["mbps"] as? Double ?? 0,
                          encodeMS: object["encode_ms"] as? Double ?? 0)
        default:
            break
        }
    }
}
