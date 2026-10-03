import SwiftUI

struct SettingsView: View {
    @ObservedObject private var bridge = BridgeController.shared
    @ObservedObject private var updates = UpdateChecker.shared
    @ObservedObject private var vr = VRSettings.shared
    @ObservedObject private var streamer = StreamerController.shared
    @State private var launchAtLogin = LoginItem.isEnabled

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            forceFeedbackBox
            wheelBox
            vrBox
            generalBox
        }
        .padding(20)
        .frame(width: 460)
        .onAppear { launchAtLogin = LoginItem.isEnabled }
    }

    // MARK: - Force feedback

    private var forceFeedbackBox: some View {
        GroupBox {
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
            .padding(.top, 2)
        } label: {
            Label("Force Feedback", systemImage: "slider.horizontal.3").font(.headline)
        }
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

    private var wheelBox: some View {
        GroupBox {
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
            .padding(.top, 2)
        } label: {
            Label("Wheel", systemImage: "steeringwheel").font(.headline)
        }
    }

    // MARK: - VR

    private var vrBox: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 12) {
                Toggle("Stream games to a VR headset", isOn: Binding(
                    get: { vr.enabled },
                    set: { vr.setEnabled($0) }))
                    .disabled(vr.applying || !CrossOver.isVRRuntimeAvailable())

                if !CrossOver.isVRRuntimeAvailable() {
                    Text("VR isn't included in this build.").font(.callout).foregroundStyle(.secondary)
                } else if vr.enabled {
                    headsetRow

                    Divider()

                    LabeledContent("Codec") {
                        Picker("", selection: Binding(get: { vr.codec }, set: { vr.setCodec($0) })) {
                            ForEach(VRSettings.Codec.allCases) { codec in
                                Text(codec.displayName).tag(codec)
                            }
                        }
                        .labelsHidden()
                        .frame(width: 120)
                    }
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

                    Text("Install ALVR from the headset's store and open it; it connects on its own "
                        + "while a game is running in VR. Add -openxr to the game's launch options.")
                        .font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } else {
                    Text("VR support is disabled.")
                        .font(.callout).foregroundStyle(.secondary)
                }

                if let error = vr.lastError {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .font(.callout).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.top, 2)
        } label: {
            Label("VR", systemImage: "visionpro").font(.headline)
        }
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

    private var generalBox: some View {
        GroupBox {
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
            .padding(.top, 2)
        } label: {
            Label("General", systemImage: "gearshape").font(.headline)
        }
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
