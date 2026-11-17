import AppKit
import Combine
import SwiftUI

/// Asks for the PIN a Steam Link headset shows the first time it pairs with
/// this Mac. Wheelio lives in the menu bar, so the window opens on its own
/// when the streamer asks, and closes once the headset is paired or pairing
/// is cancelled. Closing it cancels pairing.
@MainActor
final class PairingWindow: NSObject, NSWindowDelegate {
    static let shared = PairingWindow()

    private var window: NSWindow?
    private var observer: AnyCancellable?

    func follow(_ streamer: StreamerController) {
        observer = streamer.$pairing
            .removeDuplicates()
            .sink { [weak self] pairing in
                if let pairing { self?.show(pairing) } else { self?.close() }
            }
    }

    private func show(_ pairing: StreamerController.Pairing) {
        let window = self.window ?? makeWindow()
        self.window = window
        window.contentView = NSHostingView(rootView: PairingView(pairing: pairing) { pin in
            StreamerController.shared.answerPin(pin)
        })
        NSApp.setActivationPolicy(.regular)
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func close() {
        guard let window, window.isVisible else { return }
        window.delegate = nil
        window.close()
        window.delegate = self
    }

    private func makeWindow() -> NSWindow {
        let window = NSWindow(contentRect: .zero, styleMask: [.titled, .closable], backing: .buffered, defer: true)
        window.title = "Pair Headset"
        window.isReleasedWhenClosed = false
        window.delegate = self
        return window
    }

    // The player closed the window: no PIN.
    func windowWillClose(_ notification: Notification) {
        if StreamerController.shared.pairing != nil {
            StreamerController.shared.answerPin(nil)
        }
    }
}

private struct PairingView: View {
    let pairing: StreamerController.Pairing
    let answer: (String?) -> Void
    @State private var pin = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(pairing.device.isEmpty ? "Pair your headset" : "Pair \(pairing.device)")
                .font(.headline)
            Text(pairing.pinWasWrong
                 ? "That PIN didn't match. Enter the PIN the headset shows."
                 : "Enter the PIN the headset shows.")
                .foregroundStyle(.secondary)
            TextField("PIN", text: $pin)
                .textFieldStyle(.roundedBorder)
                .font(.title2.monospacedDigit())
                .onSubmit(pair)
            HStack {
                Spacer()
                Button("Cancel") { answer(nil) }
                    .keyboardShortcut(.cancelAction)
                Button("Pair", action: pair)
                    .keyboardShortcut(.defaultAction)
                    .disabled(!pin.contains(where: \.isNumber))
            }
        }
        .padding(20)
        .frame(width: 320)
    }

    private func pair() {
        guard pin.contains(where: \.isNumber) else { return }
        answer(pin)
        pin = ""
    }
}
