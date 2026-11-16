import SwiftUI

struct SettingsView: View {
    @ObservedObject private var bridge = BridgeController.shared
    @ObservedObject private var updates = UpdateChecker.shared
    @ObservedObject private var vr = VRSettings.shared
    @ObservedObject private var streamer = StreamerController.shared
    @ObservedObject private var steam = SteamIntegration.shared
    @ObservedObject private var engine = Engine.shared
    @ObservedObject private var toolkit = GamePortingToolkit.shared
    @State private var addingToolkit = false
    @State private var addingOtherGame = false
    @State private var nonSteamGameFile: PickedFile?
    @State private var selectedWindowsGame: String?
    @State private var launchAtLogin = LoginItem.isEnabled

    var body: some View {
        TabView {
            tab { generalSettings }
                .tabItem { Label("General", systemImage: "gearshape") }
            tab {
                wheelSettings
                Divider()
                forceFeedbackSettings
            }
            .tabItem { Label("Wheel", systemImage: "steeringwheel") }
            tab { steamSettings }
                .tabItem { Label("Steam", systemImage: "gamecontroller") }
            tab { vrSettings }
                .tabItem { Label("VR", systemImage: "visionpro") }
        }
        .onAppear { launchAtLogin = LoginItem.isEnabled }
    }

    private func tab<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 16, content: content)
            .padding(20)
            .frame(width: 460)
    }

    // MARK: - Force feedback

    private var forceFeedbackSettings: some View {
        VStack(alignment: .leading, spacing: 12) {
            gainRow("Force", value: bridge.forceGain, set: bridge.setForceGain)
            gainRow("Spring", value: bridge.springGain, set: bridge.setSpringGain)
            gainRow("Damper", value: bridge.damperGain, set: bridge.setDamperGain)
            Text("Trims scale the game's forces before they reach the wheel (100% = unchanged).")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Divider()

            gainRow("Smoothing", value: bridge.smoothing, range: 0...1, set: bridge.setSmoothing)
            Text("Softens sharp force changes. 0% is most responsive (off).")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Divider()

            gainRow("Min. force", value: bridge.minForce, range: 0...0.5, set: bridge.setMinForce)
            Text("Lifts small forces over the wheel's dead zone so light road detail is felt. 0% is off.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func gainRow(_ title: String, value: Double, range: ClosedRange<Double> = 0...2,
                         set: @escaping (Double) -> Void) -> some View {
        HStack(spacing: 12) {
            Text(title).frame(width: 74, alignment: .leading)
            Slider(value: Binding(get: { value }, set: set), in: range, step: 0.05)
            Text("\(Int((value * 100).rounded()))%")
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .frame(width: 46, alignment: .trailing)
        }
    }

    // MARK: - Wheel

    private var wheelSettings: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 7) {
                Circle()
                    .fill(bridge.status.wheelConnected ? Color.green : Color.secondary.opacity(0.5))
                    .frame(width: 8, height: 8)
                Text(bridge.status.wheelDetail)
                Spacer()
            }
            HStack {
                Button { bridge.runSelfTest() } label: {
                    Label(bridge.isSelfTesting ? "Testing\u{2026}" : "Test Wheel",
                          systemImage: "waveform.path")
                }
                .disabled(bridge.isSelfTesting || !bridge.status.wheelConnected)

                Button { bridge.reconnect() } label: {
                    Label(bridge.isReconnecting ? "Reconnecting\u{2026}" : "Reconnect",
                          systemImage: "arrow.clockwise")
                }
                .disabled(bridge.isReconnecting)
                Spacer()
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: - Steam

    private var steamSettings: some View {
        VStack(alignment: .leading, spacing: 12) {
            Toggle("Play Windows games in Steam", isOn: Binding(
                get: { steam.enabled },
                set: { steam.setEnabled($0) }))
                .disabled(engine.state == .unavailable)

            if engine.state == .unavailable {
                Text("Windows games aren't included in this build.")
                    .font(.callout).foregroundStyle(.secondary)
            } else if steam.enabled {
                if steam.paused {
                    Text("Steam has updated, and Windows games are paused until Wheelio supports this version.")
                        .font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                engineRow
                toolkitRow
                Divider()
                windowsGamesSection
                if steam.waitingForSteam {
                    Text("Changes apply when you quit Steam.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }

            if let error = steam.lastError {
                Label(error, systemImage: "exclamationmark.triangle")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private var engineRow: some View {
        switch engine.state {
        case .downloading(let fraction):
            ProgressView("Downloading the game engine\u{2026}", value: fraction)
        case .installing:
            ProgressView("Installing the game engine\u{2026}")
        case .failed(let message):
            HStack {
                Label(message, systemImage: "exclamationmark.triangle")
                    .font(.callout).foregroundStyle(.secondary)
                Spacer()
                Button("Try Again") { engine.install() }
            }
        case .missing:
            HStack {
                Text("The game engine isn't installed.").foregroundStyle(.secondary)
                Spacer()
                Button("Install") { engine.install() }
            }
        case .ready, .unavailable:
            EmptyView()
        }
    }

    /// Games that also have a Mac version, played in their Windows version:
    /// a bordered list with add and remove buttons beneath, as in System Settings.
    private var windowsGamesSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Force Windows version")
            Text("For games that also have a Mac version. Wheelio's force feedback and VR need the Windows version.")
                .font(.callout).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            VStack(spacing: 0) {
                List(selection: $selectedWindowsGame) {
                    ForEach(listedGames, id: \.id) { game in
                        Text(game.name).tag(game.id)
                    }
                }
                .listStyle(.plain)
                .scrollContentBackground(.hidden)
                .frame(height: 96)
                .overlay {
                    if listedGames.isEmpty {
                        Text("No games").foregroundStyle(.secondary)
                    }
                }
                Divider()
                HStack(spacing: 0) {
                    Menu {
                        let installed = SteamGames.installed().filter { !steam.windowsGames.contains($0) }
                        ForEach(installed) { game in
                            Button(game.name) { steam.addWindowsGame(game) }
                        }
                        if !installed.isEmpty { Divider() }
                        Button("Store Link or App ID\u{2026}") { addingOtherGame = true }
                        Button("Non-Steam Game\u{2026}") { pickNonSteamGame() }
                    } label: {
                        Image(systemName: "plus").frame(width: 22, height: 18)
                    }
                    .menuStyle(.borderlessButton)
                    .menuIndicator(.hidden)
                    .fixedSize()
                    .help("Add a game")
                    Divider().frame(height: 14)
                    Button {
                        if let game = steam.windowsGames.first(where: { $0.id == selectedWindowsGame }) {
                            steam.removeWindowsGame(game)
                        } else if let game = steam.nonSteamGames.first(where: { $0.id == selectedWindowsGame }) {
                            steam.removeNonSteamGame(game)
                        }
                        selectedWindowsGame = nil
                    } label: {
                        Image(systemName: "minus").frame(width: 22, height: 18)
                    }
                    .buttonStyle(.borderless)
                    .disabled(selectedWindowsGame == nil)
                    .help("Remove the selected game")
                    Spacer()
                }
                .padding(.horizontal, 4)
                .padding(.vertical, 3)
            }
            .background(Color(nsColor: .controlBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color(nsColor: .separatorColor)))
        }
        .sheet(isPresented: $addingOtherGame) { AddSteamGameSheet() }
        .sheet(item: $nonSteamGameFile) { AddNonSteamGameSheet(file: $0.url) }
    }

    /// Steam games played in their Windows version and non-Steam games, by name.
    private var listedGames: [(id: String, name: String)] {
        (steam.windowsGames.map { ($0.id, $0.name) } + steam.nonSteamGames.map { ($0.id, $0.name) })
            .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    private func pickNonSteamGame() {
        let panel = NSOpenPanel()
        panel.message = "Choose a Windows game or its installer."
        panel.allowedContentTypes = NonSteamGameSetup.programTypes
        if panel.runModal() == .OK, let url = panel.url { nonSteamGameFile = PickedFile(url: url) }
    }

    private var toolkitRow: some View {
        HStack {
            Text(toolkit.installed ? "Apple's Game Porting Toolkit is added."
                                   : "Apple's Game Porting Toolkit lets more games use DirectX 12.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Spacer()
            if toolkit.installed {
                Button("Remove") { toolkit.remove() }
            } else {
                Button("Add\u{2026}") { addingToolkit = true }
            }
        }
        .sheet(isPresented: $addingToolkit) { GamePortingToolkitSheet() }
    }

    // MARK: - VR

    private var vrSettings: some View {
        VStack(alignment: .leading, spacing: 12) {
            if !WheelioPaths.isVRRuntimeBundled {
                Text("VR isn't included in this build.").font(.callout).foregroundStyle(.secondary)
            } else {
                headsetRow

                Divider()

                HStack(spacing: 12) {
                    Text("Bitrate").frame(width: 74, alignment: .leading)
                    Slider(value: Binding(get: { Double(vr.bitrateMbps) }, set: { vr.setBitrate(Int($0)) }),
                           in: Double(VRSettings.bitrateRange.lowerBound)...Double(VRSettings.bitrateRange.upperBound),
                           step: 5)
                    Text("\(vr.bitrateMbps) Mbps").monospacedDigit().foregroundStyle(.secondary)
                        .frame(width: 70, alignment: .trailing)
                }
                Toggle("Mute this Mac while streaming", isOn: Binding(
                    get: { vr.muteMac },
                    set: { vr.setMuteMac($0) }))

                Text("Add -openxr to a game's launch options in Steam to play it in VR. In Steam Link on "
                    + "the headset, choose this Mac, shown with \u{201C}(VR)\u{201D} after its name.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var headsetRow: some View {
        HStack(spacing: 7) {
            Circle()
                .fill(headsetConnected ? Color.green : Color.secondary.opacity(0.5))
                .frame(width: 8, height: 8)
            Text(headsetText)
            Spacer()
        }
    }

    private var headsetConnected: Bool {
        if case .connected = streamer.state { return true }
        return false
    }

    private var headsetText: String {
        switch streamer.state {
        case .unavailable: return "VR streaming isn't included in this build."
        case .idle: return bridge.vrGameRunning ? "Starting\u{2026}" : "No game running in VR."
        case .starting: return "Starting\u{2026}"
        case .waiting: return "Waiting for the headset."
        case .connected:
            return String(format: "Headset connected: %.0f fps, %.0f Mbps", streamer.stats.fps, streamer.stats.mbps)
        case .failed(let message): return message
        }
    }

    // MARK: - General (startup + updates)

    private var generalSettings: some View {
        VStack(alignment: .leading, spacing: 12) {
            Toggle("Launch at login", isOn: Binding(
                get: { launchAtLogin },
                set: { launchAtLogin = LoginItem.setEnabled($0) }))

            Divider()

            HStack {
                Text("Version \(updates.currentVersion)").foregroundStyle(.secondary)
                Spacer()
                Button("Check for Updates\u{2026}") { updates.check() }
                    .disabled(updates.state == .checking)
            }
            updateStatusRow
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder
    private var updateStatusRow: some View {
        switch updates.state {
        case .idle:
            EmptyView()
        case .checking:
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Checking\u{2026}").foregroundStyle(.secondary)
            }
        case .upToDate:
            Label("You're on the latest version.", systemImage: "checkmark.circle.fill")
                .foregroundStyle(.secondary)
        case .updateAvailable(let version, _):
            Button { updates.openReleasePage() } label: {
                Label("Download version \(version)\u{2026}", systemImage: "arrow.down.circle.fill")
            }
        case .failed:
            Label("Couldn't check for updates right now.", systemImage: "exclamationmark.triangle")
                .foregroundStyle(.secondary)
        }
    }
}

/// A file the player chose, for a sheet about it.
private struct PickedFile: Identifiable {
    let url: URL
    var id: URL { url }
}
