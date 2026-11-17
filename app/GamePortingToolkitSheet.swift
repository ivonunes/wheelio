import SwiftUI

/// Walks the player through adding Apple's Game Porting Toolkit: download it
/// from Apple, then choose the disk image. Closes itself once it's added.
struct GamePortingToolkitSheet: View {
    @ObservedObject private var toolkit = GamePortingToolkit.shared
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Add Apple's Game Porting Toolkit").font(.headline)
            Text("It lets more games use DirectX 12. Apple offers it to download; Wheelio can't include it.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            step(1, "Download it from Apple. You'll need to sign in with your Apple ID.") {
                Button("Open Apple's Page") { NSWorkspace.shared.open(GamePortingToolkit.downloadPage) }
            }
            step(2, "Then choose the disk image you downloaded.") {
                Button("Choose\u{2026}") { chooseImage() }
                    .disabled(toolkit.adding)
            }

            if toolkit.adding {
                ProgressView("Adding\u{2026}").controlSize(.small)
            } else if let error = toolkit.lastError {
                Label(error, systemImage: "exclamationmark.triangle")
                    .font(.callout).foregroundStyle(.secondary)
            }

            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(20)
        .frame(width: 420)
        .onChange(of: toolkit.installed) { _, installed in
            if installed { dismiss() }
        }
    }

    private func step(_ number: Int, _ text: String, @ViewBuilder action: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text("\(number).").monospacedDigit().foregroundStyle(.secondary)
            Text(text).fixedSize(horizontal: false, vertical: true)
            Spacer()
            action()
        }
    }

    private func chooseImage() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.diskImage]
        panel.canChooseDirectories = true
        panel.prompt = "Add"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        toolkit.add(from: url)
    }
}
