import SwiftUI

/// Adds a Windows game from outside Steam to Steam: the game's own program,
/// or its installer, run first (NonSteamGameSetup).
struct AddNonSteamGameSheet: View {
    @StateObject private var setup: NonSteamGameSetup
    @Environment(\.dismiss) private var dismiss
    @State private var selected: WinePrefix.Program.ID?

    init(file: URL) {
        _setup = StateObject(wrappedValue: NonSteamGameSetup(file: file))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            content
        }
        .padding(20)
        .frame(width: 420)
        .onAppear { setup.start() }
        .onChange(of: setup.step) { _, step in
            if step == .done { dismiss() }
        }
    }

    @ViewBuilder
    private var content: some View {
        switch setup.step {
        case .askingKind:
            Text("Is this the game or its installer?").font(.headline)
            Text(setup.fileName).foregroundStyle(.secondary)
            HStack {
                Spacer()
                Button("Cancel") { setup.cancel() }
                    .keyboardShortcut(.cancelAction)
                // Neither is the default: a quick Return mustn't guess.
                Button("Installer") { setup.install() }
                Button("Game") { setup.addAsGame() }
            }
        case .installing:
            HStack(spacing: 10) {
                ProgressView().controlSize(.small)
                Text("Waiting for the installer to finish\u{2026}")
            }
            HStack {
                Spacer()
                Button("Cancel") { setup.cancel() }
                    .keyboardShortcut(.cancelAction)
            }
        case .choosing(let programs):
            Text("Which of these starts the game?").font(.headline)
            List(programs, selection: $selected) { program in
                Text(program.name).tag(program.id)
            }
            .frame(height: 140)
            .onAppear { if programs.count == 1 { selected = programs[0].id } }
            HStack {
                Button("Other\u{2026}") { setup.pickProgram() }
                Spacer()
                Button("Cancel") { setup.cancel() }
                    .keyboardShortcut(.cancelAction)
                Button("Add") {
                    if let program = programs.first(where: { $0.id == selected }) { setup.choose(program) }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(selected == nil)
            }
        case .failed(let message):
            Label(message, systemImage: "exclamationmark.triangle")
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer()
                Button("OK") { setup.cancel() }
                    .keyboardShortcut(.defaultAction)
            }
        case .done:
            EmptyView()
        }
    }
}
