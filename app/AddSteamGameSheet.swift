import SwiftUI

/// Adds a game to play in its Windows version by its Steam store link or app
/// ID, for one that isn't installed yet: choosing it first spares Steam from
/// downloading the Mac version only to replace it.
struct AddSteamGameSheet: View {
    @ObservedObject private var steam = SteamIntegration.shared
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""
    @State private var looking = false
    @State private var problem: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Add a game").font(.headline)
            Text("Paste the game's Steam store link, or its app ID.")
                .foregroundStyle(.secondary)
            TextField("https://store.steampowered.com/app/227300/…", text: $text)
                .textFieldStyle(.roundedBorder)
                .onSubmit(add)
            if let problem {
                Label(problem, systemImage: "exclamationmark.triangle")
                    .font(.callout).foregroundStyle(.secondary)
            }
            HStack {
                if looking { ProgressView().controlSize(.small) }
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Add", action: add)
                    .keyboardShortcut(.defaultAction)
                    .disabled(SteamGames.appID(from: text) == nil || looking)
            }
        }
        .padding(20)
        .frame(width: 420)
    }

    private func add() {
        guard let appID = SteamGames.appID(from: text), !looking else { return }
        looking = true
        problem = nil
        Task {
            let name = await SteamGames.name(of: appID)
            looking = false
            guard let name else {
                problem = "Steam doesn't know a game with that link or ID."
                return
            }
            steam.addWindowsGame(SteamGame(appID: appID, name: name))
            dismiss()
        }
    }
}
