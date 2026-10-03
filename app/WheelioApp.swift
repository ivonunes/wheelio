import SwiftUI

@main
struct WheelioApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var bridge = BridgeController.shared

    static let setupWindowID = "wheelio-setup"
    static let settingsWindowID = "wheelio-settings"

    init() {
        // Before anything opens the wheel or the bridge port.
        SingleInstance.handOverIfAlreadyRunning()
    }

    var body: some Scene {
        MenuBarExtra {
            MenuContent(bridge: bridge)
        } label: {
            // Steering-wheel glyph in the menu bar; a warning triangle when
            // Input Monitoring isn't granted so the wheel can't be reached.
            Image(systemName: bridge.inputMonitoring == .granted
                ? "steeringwheel" : "exclamationmark.triangle.fill")
        }

        Window("Set Up a Game", id: Self.setupWindowID) {
            SetupView()
        }
        .windowResizability(.contentSize)

        Window("Wheelio Settings", id: Self.settingsWindowID) {
            SettingsView()
        }
        .windowResizability(.contentSize)

    }
}

/// The menu-bar dropdown. Split out so it can use the openWindow environment.
private struct MenuContent: View {
    @ObservedObject var bridge: BridgeController
    @ObservedObject private var updates = UpdateChecker.shared
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        if case let .updateAvailable(version, _) = updates.state {
            Button("\u{2B06}\u{FE0E} Update available: v\(version)\u{2026}") { updates.openReleasePage() }
            Divider()
        }

        // Input Monitoring gate: HID access is required to reach the wheel.
        if bridge.inputMonitoring != .granted {
            Text("\u{26A0}\u{FE0E} Input Monitoring not enabled")
            Text("The wheel can't be reached until you allow it.")

            if bridge.inputMonitoring == .unknown {
                Button("Request Permission\u{2026}") { bridge.requestInputMonitoring() }
            } else {
                Button("Open Input Monitoring Settings\u{2026}") {
                    bridge.openInputMonitoringSettings()
                }
            }
            Button("Relaunch Wheelio") { bridge.relaunch() }

            Divider()
        }

        // Status rows (non-interactive). Internal bridge/port details are hidden;
        // a warning shows only if the bridge genuinely failed to start.
        if !bridge.status.listening {
            Text("\u{26A0}\u{FE0E} Bridge not running")
        }
        Text(bridge.status.wheelText)
        Text(bridge.status.clientText)

        Divider()

        Button("Set Up a Game\u{2026}") { present(WheelioApp.setupWindowID) }
        Button("Settings\u{2026}") { present(WheelioApp.settingsWindowID) }
            .keyboardShortcut(",")

        Button(bridge.isReconnecting ? "Reconnecting\u{2026}" : "Reconnect Wheel") {
            bridge.reconnect()
        }
        .disabled(bridge.isReconnecting)

        Button("Quit") {
            NSApplication.shared.terminate(nil)
        }
        .keyboardShortcut("q")
    }

    /// Accessory apps don't show ordinary windows, so switch to a regular
    /// activation policy to present and focus a window. AppDelegate restores
    /// .accessory once all content windows close.
    private func present(_ windowID: String) {
        NSApp.setActivationPolicy(.regular)
        openWindow(id: windowID)
        NSApp.activate(ignoringOtherApps: true)
    }
}

/// Releases the wheel when the app quits (the old AppKit app did this in
/// applicationWillTerminate). NSApplication.terminate won't deinit the
/// StateObject, so we hook termination explicitly.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Keep proxies installed next to games in sync with the app's bundled
        // DLL — e.g. after the app updates to a newer version.
        DispatchQueue.global(qos: .utility).async {
            CrossOver.refreshInstalledProxies()
        }

        // Check GitHub for a newer release (notify-only).
        UpdateChecker.shared.check()

        // The VR streamer runs while VR is on and a game is rendering in VR.
        StreamerController.shared.follow(BridgeController.shared, VRSettings.shared)

        // Drop back to accessory (no Dock icon) once the user closes our windows.
        // Done here rather than per-view onDisappear, which doesn't fire reliably.
        NotificationCenter.default.addObserver(
            self, selector: #selector(windowWillClose(_:)),
            name: NSWindow.willCloseNotification, object: nil)
    }

    @objc private func windowWillClose(_ notification: Notification) {
        DispatchQueue.main.async {
            // The closing window is still in NSApp.windows here, so exclude it.
            let closing = notification.object as? NSWindow
            let hasContentWindow = NSApp.windows.contains { window in
                window !== closing && window.isVisible && window.canBecomeMain
            }
            if !hasContentWindow {
                NSApp.setActivationPolicy(.accessory)
            }
        }
    }

    func applicationWillTerminate(_ notification: Notification) {
        StreamerController.shared.stop()
        BridgeController.shared.shutdown()
    }
}
