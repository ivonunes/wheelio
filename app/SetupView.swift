import AppKit
import SwiftUI

/// Drives the "Set Up a Game" wizard: bottle/exe selection, install, and the
/// live connection check.
@MainActor
final class SetupModel: ObservableObject {
    enum Phase: Equatable {
        case idle
        case installing
        case awaitingConnection
        case failed(String)
    }

    @Published var bottles: [Bottle] = []
    @Published var selectedBottle: Bottle?
    @Published var selectedExe: URL?
    @Published var exeMachine: PEMachine?
    @Published var phase: Phase = .idle
    @Published var installed: [InstallRecord] = []
    @Published var awaitTimedOut = false
    // Cached during refresh() so SwiftUI body evaluation doesn't hit the
    // filesystem repeatedly.
    @Published var proxyAvailable = false
    @Published var crossOverInstalled = false

    private var connectWatchdog: Timer?

    var canInstall: Bool {
        guard let machine = exeMachine else { return false }
        return selectedBottle != nil && selectedExe != nil && CrossOver.supportsInstall(for: machine)
            && phase != .installing
    }

    deinit { connectWatchdog?.invalidate() }

    func refresh() {
        crossOverInstalled = CrossOver.isCrossOverInstalled()
        proxyAvailable = CrossOver.proxyDLLURL() != nil
        bottles = CrossOver.discoverBottles()
        if selectedBottle == nil { selectedBottle = bottles.first }
        installed = CrossOver.records()
    }

    func chooseExe() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false
        panel.message = "Select the game's .exe inside the bottle"
        panel.prompt = "Choose"
        if let bottle = selectedBottle {
            panel.directoryURL = bottle.url.appendingPathComponent("drive_c")
        }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        selectedExe = url
        exeMachine = CrossOver.peMachine(of: url)
        if phase != .installing { phase = .idle }
    }

    func install() {
        guard let bottle = selectedBottle, let exe = selectedExe else { return }
        phase = .installing
        let vr = VRSettings.shared.enabled
        Task.detached {
            do {
                _ = try CrossOver.install(exe: exe, bottle: bottle, vr: vr)
                await MainActor.run {
                    self.installed = CrossOver.records()
                    self.phase = .awaitingConnection
                    self.startConnectWatchdog()
                }
            } catch {
                await MainActor.run {
                    self.phase = .failed(error.localizedDescription)
                }
            }
        }
    }

    /// If the game doesn't connect within a reasonable window, surface
    /// troubleshooting hints rather than spinning forever.
    private func startConnectWatchdog() {
        awaitTimedOut = false
        connectWatchdog?.invalidate()
        connectWatchdog = Timer.scheduledTimer(withTimeInterval: 45, repeats: false) { [weak self] _ in
            self?.awaitTimedOut = true
        }
    }

    func uninstall(_ record: InstallRecord) {
        CrossOver.uninstall(record)
        installed = CrossOver.records()
    }
}

struct SetupView: View {
    @StateObject private var model = SetupModel()
    @ObservedObject private var bridge = BridgeController.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if !model.crossOverInstalled {
                emptyState(icon: "exclamationmark.triangle.fill",
                           title: "CrossOver not found",
                           message: "This installer supports CrossOver bottles, but CrossOver "
                               + "wasn't found in /Applications.")
            } else if model.bottles.isEmpty {
                emptyState(icon: "tray",
                           title: "No bottles found",
                           message: "Create a bottle in CrossOver first, then reopen this window.")
            } else {
                newGameBox
            }

            if !model.installed.isEmpty {
                installedBox
            }
        }
        .padding(20)
        .frame(width: 460)
        .onAppear { model.refresh() }
    }

    // MARK: - New game

    private var newGameBox: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 14) {
                LabeledContent("Bottle") {
                    Picker("", selection: $model.selectedBottle) {
                        ForEach(model.bottles) { bottle in
                            Text(bottle.name).tag(Optional(bottle))
                        }
                    }
                    .labelsHidden()
                }

                LabeledContent("Game") {
                    HStack(spacing: 6) {
                        if let exe = model.selectedExe {
                            if let machine = model.exeMachine {
                                let supported = machine == .x64 || machine == .x86
                                Image(systemName: supported ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                                    .foregroundStyle(supported ? Color.green : Color.orange)
                            }
                            Text(exe.lastPathComponent).lineLimit(1).truncationMode(.middle)
                        } else {
                            Text("None selected").foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button(model.selectedExe == nil ? "Choose\u{2026}" : "Change\u{2026}") {
                            model.chooseExe()
                        }
                    }
                }

                if let machine = model.exeMachine, machine != .x64, machine != .x86 {
                    Text("Only 64-bit and 32-bit x86 games are supported.")
                        .font(.callout).foregroundStyle(.secondary)
                }
                if model.exeMachine == .x86, VRSettings.shared.enabled {
                    Text("VR is only available for 64-bit games.")
                        .font(.callout).foregroundStyle(.secondary)
                }
                if !model.proxyAvailable {
                    Text("The proxy isn't bundled in this build, so install is disabled.")
                        .font(.callout).foregroundStyle(.secondary)
                }

                Button { model.install() } label: {
                    Text("Install Force Feedback").frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .keyboardShortcut(.defaultAction)
                .disabled(!model.canInstall)

                statusView
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 2)
        } label: {
            Label("New Game", systemImage: "gamecontroller").font(.headline)
        }
    }

    @ViewBuilder
    private var statusView: some View {
        switch model.phase {
        case .idle:
            Text("Pick the bottle and the game's executable, then install.")
                .font(.callout).foregroundStyle(.secondary)
        case .installing:
            HStack(spacing: 8) { ProgressView().controlSize(.small); Text("Installing\u{2026}") }
        case .awaitingConnection:
            if bridge.status.clientConnected {
                label("Connected: \(bridge.status.clientName.isEmpty ? "game" : bridge.status.clientName)",
                      systemImage: "checkmark.circle.fill", color: .green)
            } else if model.awaitTimedOut {
                VStack(alignment: .leading, spacing: 6) {
                    label("Installed, but the game hasn't connected yet.",
                          systemImage: "questionmark.circle.fill", color: .orange)
                    Text("If force feedback isn't working in-game, check that:\n"
                        + "\u{2022} the game was launched from this CrossOver bottle\n"
                        + "\u{2022} the bottle's dinput8 override is enabled (winecfg \u{2192} Libraries)\n"
                        + "\u{2022} Wheelio shows the wheel as connected")
                        .font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            } else {
                VStack(alignment: .leading, spacing: 6) {
                    label("Installed. Now launch your game in CrossOver\u{2026}",
                          systemImage: "checkmark.circle.fill", color: .green)
                    HStack(spacing: 8) {
                        ProgressView().controlSize(.small)
                        Text("Waiting for the game to connect\u{2026}").font(.callout).foregroundStyle(.secondary)
                    }
                }
            }
        case .failed(let message):
            VStack(alignment: .leading, spacing: 4) {
                label("Install failed", systemImage: "xmark.circle.fill", color: .red)
                Text(message).font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    // MARK: - Installed games

    private var installedBox: some View {
        GroupBox {
            VStack(spacing: 10) {
                ForEach(Array(model.installed.enumerated()), id: \.element.id) { index, record in
                    if index > 0 { Divider() }
                    HStack(spacing: 10) {
                        Image(systemName: "gamecontroller").foregroundStyle(.secondary)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(record.exeName)
                            Text(record.vrInstalled == true ? "\(record.bottleName) \u{00B7} VR" : record.bottleName)
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Remove") { model.uninstall(record) }
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 2)
        } label: {
            Label("Installed Games", systemImage: "tray.full").font(.headline)
        }
    }

    // MARK: - Helpers

    private func label(_ text: String, systemImage: String, color: Color) -> some View {
        Label { Text(text) } icon: { Image(systemName: systemImage).foregroundStyle(color) }
    }

    private func emptyState(icon: String, title: String, message: String) -> some View {
        GroupBox {
            VStack(spacing: 8) {
                Image(systemName: icon).font(.largeTitle).foregroundStyle(.secondary)
                Text(title).font(.headline)
                Text(message)
                    .font(.callout).foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 14)
        }
    }
}
