import AppKit
import Combine
import Foundation

/// Swift-friendly snapshot of the engine state, built from the C `WheelioBridgeStatus`.
struct BridgeStatus {
    var listening = false
    var clientConnected = false
    var wheelConnected = false
    var port: UInt16 = 0
    var packetsReceived: UInt64 = 0
    var clientName = ""
    var wheelName = ""

    init() {}

    init(_ raw: WheelioBridgeStatus) {
        listening = raw.listening
        clientConnected = raw.client_connected
        wheelConnected = raw.wheel_connected
        port = raw.port
        packetsReceived = raw.packets_received
        clientName = Self.string(from: raw.client_name)
        wheelName = Self.string(from: raw.wheel_name)
    }

    /// Reads a fixed C `char[64]` (imported as a tuple) into a Swift String.
    private static func string<T>(from tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in
            guard let base = raw.baseAddress else { return "" }
            return String(cString: base.assumingMemoryBound(to: CChar.self))
        }
    }
}

extension BridgeStatus {
    var clientText: String {
        clientConnected ? "Game: \(clientName.isEmpty ? "Connected" : clientName)" : "Game: Not connected"
    }
    var wheelText: String {
        if !wheelName.isEmpty { return "Wheel: \(wheelName)" }
        if wheelConnected { return "Wheel: Supported wheel connected" }
        return "Wheel: Not connected"
    }

    /// Wheel status without the "Wheel: " prefix (for use under a "Wheel" header).
    var wheelDetail: String {
        if !wheelName.isEmpty { return wheelName }
        if wheelConnected { return "Supported wheel connected" }
        return "Not connected"
    }
}

/// Input Monitoring (TCC) access, mirrors the C `WheelioInputMonitoringState`.
enum InputMonitoringAccess {
    case granted
    case denied
    case unknown

    init(_ raw: WheelioInputMonitoringState) {
        switch raw {
        case WheelioInputMonitoringGranted: self = .granted
        case WheelioInputMonitoringDenied: self = .denied
        default: self = .unknown
        }
    }
}

/// Owns the C++ engine handle and publishes its status for SwiftUI.
///
/// A single shared instance backs both the menu UI and the app delegate's
/// terminate handler, so force feedback is always released on quit.
final class BridgeController: ObservableObject {
    static let shared = BridgeController()

    @Published private(set) var status = BridgeStatus()
    @Published private(set) var isReconnecting = false
    @Published private(set) var inputMonitoring: InputMonitoringAccess = .unknown
    @Published private(set) var forceGain: Double = 1.0
    @Published private(set) var springGain: Double = 1.0
    @Published private(set) var damperGain: Double = 1.0
    @Published private(set) var smoothing: Double = 0.0  // off by default
    @Published private(set) var minForce: Double = 0.0   // off by default
    @Published private(set) var isSelfTesting = false
    /// A game is rendering through the VR runtime right now.
    @Published private(set) var vrGameRunning = false

    private let handle: OpaquePointer
    private let idleSleepGuard = IdleSleepGuard()

    /// The engine handle, for the VR preview's own calls into the C shim.
    var rawHandle: OpaquePointer { handle }
    private var timer: Timer?
    private static let forceGainKey = "forceGain"
    private static let springGainKey = "springGain"
    private static let damperGainKey = "damperGain"
    private static let smoothingKey = "smoothing"
    private static let minForceKey = "minForce"

    private static func savedGain(_ key: String) -> Double {
        guard UserDefaults.standard.object(forKey: key) != nil else { return 1.0 }
        return max(0.0, min(2.0, UserDefaults.standard.double(forKey: key)))
    }

    private init() {
        handle = wheelio_bridge_create(0)  // 0 -> default port
        wheelio_bridge_start(handle)

        // Restore the saved gains/smoothing and push them to the engine.
        forceGain = Self.savedGain(Self.forceGainKey)
        springGain = Self.savedGain(Self.springGainKey)
        damperGain = Self.savedGain(Self.damperGainKey)
        if UserDefaults.standard.object(forKey: Self.smoothingKey) != nil {
            smoothing = max(0.0, min(1.0, UserDefaults.standard.double(forKey: Self.smoothingKey)))
        }
        wheelio_bridge_set_force_gain(handle, forceGain)
        wheelio_bridge_set_spring_gain(handle, springGain)
        wheelio_bridge_set_damper_gain(handle, damperGain)
        wheelio_bridge_set_smoothing(handle, smoothing)
        if UserDefaults.standard.object(forKey: Self.minForceKey) != nil {
            minForce = max(0.0, min(0.5, UserDefaults.standard.double(forKey: Self.minForceKey)))
        }
        wheelio_bridge_set_min_force(handle, minForce)

        refresh()

        // First launch: trigger the system prompt off the main thread —
        // IOHIDRequestAccess shows a dialog and blocks until answered. Once the
        // user has answered, status is granted/denied and this no-ops.
        if inputMonitoring == .unknown {
            requestInputMonitoring()
        }

        // Poll the engine for status; a push channel could replace this later.
        timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            self?.refresh()
        }
    }

    func refresh() {
        var raw = WheelioBridgeStatus()
        wheelio_bridge_status(handle, &raw)
        status = BridgeStatus(raw)
        inputMonitoring = InputMonitoringAccess(wheelio_input_monitoring_status())

        // Also keep an eye on the VR runtime: the streamer runs only while a
        // game is in VR, so the Mac's audio is only captured then.
        wheelio_vr_poll(handle)
        var vr = WheelioVRStatus()
        wheelio_vr_status(handle, &vr)
        if vr.game_running != vrGameRunning { vrGameRunning = vr.game_running }

        idleSleepGuard.active = status.clientConnected || vrGameRunning
    }

    /// Set the force-feedback trims (1.0 = unchanged), persist, and push to the engine.
    func setForceGain(_ gain: Double) {
        let clamped = max(0.0, min(2.0, gain))
        forceGain = clamped
        UserDefaults.standard.set(clamped, forKey: Self.forceGainKey)
        wheelio_bridge_set_force_gain(handle, clamped)
    }

    func setSpringGain(_ gain: Double) {
        let clamped = max(0.0, min(2.0, gain))
        springGain = clamped
        UserDefaults.standard.set(clamped, forKey: Self.springGainKey)
        wheelio_bridge_set_spring_gain(handle, clamped)
    }

    func setDamperGain(_ gain: Double) {
        let clamped = max(0.0, min(2.0, gain))
        damperGain = clamped
        UserDefaults.standard.set(clamped, forKey: Self.damperGainKey)
        wheelio_bridge_set_damper_gain(handle, clamped)
    }

    func setSmoothing(_ amount: Double) {
        let clamped = max(0.0, min(1.0, amount))
        smoothing = clamped
        UserDefaults.standard.set(clamped, forKey: Self.smoothingKey)
        wheelio_bridge_set_smoothing(handle, clamped)
    }

    func setMinForce(_ fraction: Double) {
        let clamped = max(0.0, min(0.5, fraction))
        minForce = clamped
        UserDefaults.standard.set(clamped, forKey: Self.minForceKey)
        wheelio_bridge_set_min_force(handle, clamped)
    }

    /// Pulse force + LEDs on the wheel so the user can confirm it works. Runs off
    /// the main thread (the engine call blocks ~0.7s).
    func runSelfTest() {
        guard !isSelfTesting else { return }
        isSelfTesting = true
        let handle = self.handle
        DispatchQueue.global(qos: .userInitiated).async {
            wheelio_bridge_self_test(handle)
            DispatchQueue.main.async { [weak self] in self?.isSelfTesting = false }
        }
    }

    /// Trigger the system Input Monitoring prompt off the main thread (it blocks
    /// until the user answers). Only effective before the user has answered once.
    func requestInputMonitoring() {
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            _ = wheelio_input_monitoring_request()
            DispatchQueue.main.async { self?.refresh() }
        }
    }

    /// Open System Settings directly at the Input Monitoring pane.
    func openInputMonitoringSettings() {
        guard let url = URL(string:
            "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent")
        else { return }
        NSWorkspace.shared.open(url)
    }

    /// Relaunch the app. Granting Input Monitoring only takes effect for a HID
    /// client after it restarts, so we offer this once access is fixed.
    func relaunch() {
        let bundlePath = Bundle.main.bundlePath
        let task = Process()
        task.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        task.arguments = ["-n", bundlePath]
        try? task.run()
        NSApplication.shared.terminate(nil)
    }

    func reconnect() {
        guard !isReconnecting else { return }
        isReconnecting = true

        let handle = self.handle
        DispatchQueue.global(qos: .userInitiated).async {
            wheelio_bridge_reconnect_wheel(handle)  // blocks; off the main thread
            DispatchQueue.main.async { [weak self] in
                self?.isReconnecting = false
                self?.refresh()
            }
        }
    }

    /// Stops the server and releases the wheel. Called on app termination.
    func shutdown() {
        timer?.invalidate()
        timer = nil
        idleSleepGuard.active = false
        wheelio_bridge_stop(handle)
    }
}
