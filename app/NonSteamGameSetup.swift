import AppKit
import UniformTypeIdentifiers

/// Adding a Windows game that isn't from Steam: a game's own program is
/// added to Steam as it is; an installer runs first, in the Wine environment
/// Steam will give the game, and the game is then found among the programs it
/// put in the Start menu.
@MainActor
final class NonSteamGameSetup: ObservableObject {
    enum Step: Equatable {
        /// An .exe: is it the game or its installer?
        case askingKind
        case installing
        /// Which of the programs the installer added is the game?
        case choosing([WinePrefix.Program])
        case failed(String)
        case done
    }

    @Published private(set) var step: Step
    let file: URL
    private var appID: String?
    private var installer: Process?
    private var cancelled = false

    init(file: URL) {
        self.file = file
        step = file.pathExtension.lowercased() == "msi" ? .installing : .askingKind
    }

    var fileName: String { file.lastPathComponent }

    /// Starts an .msi straight away; an .exe waits for the answer.
    func start() {
        if step == .installing { install() }
    }

    func addAsGame() {
        let name = file.deletingPathExtension().lastPathComponent
        finish(appID: newAppID(), name: name, exe: file, startDir: file.deletingLastPathComponent(), arguments: "")
    }

    func install() {
        step = .installing
        let appID = newAppID()
        self.appID = appID
        Task {
            do {
                try await runInstaller(appID: appID)
            } catch {
                if !cancelled { step = .failed("The installer couldn't run: \(error.localizedDescription)") }
                return
            }
            guard !cancelled else { return }
            let programs = WinePrefix.forSteamApp(appID).programs()
            // Even one is shown, in case it's a launcher rather than the game.
            if programs.isEmpty { pickProgram() } else { step = .choosing(programs) }
        }
    }

    func choose(_ program: WinePrefix.Program) {
        guard let appID else { return }
        finish(appID: appID, name: program.name, exe: program.exe, startDir: program.startDir,
               arguments: program.arguments)
    }

    /// Lets the player find the game in its environment, when the installer
    /// added nothing to the Start menu or none of what it added is the game.
    func pickProgram() {
        guard let appID else { return }
        let panel = NSOpenPanel()
        panel.message = "Choose the game's program."
        panel.allowedContentTypes = NonSteamGameSetup.programTypes.filter { $0.preferredFilenameExtension == "exe" }
        panel.directoryURL = WinePrefix.forSteamApp(appID).driveC
        guard panel.runModal() == .OK, let exe = panel.url else {
            // From the list, back to it; with nothing found, the install is abandoned.
            if case .choosing = step { return }
            cancel()
            return
        }
        finish(appID: appID, name: exe.deletingPathExtension().lastPathComponent, exe: exe,
               startDir: exe.deletingLastPathComponent(), arguments: "")
    }

    /// Stops a running installer. What it installed goes to the Bin.
    func cancel() {
        cancelled = true
        installer?.terminate()
        if let appID, step != .done {
            killWine(appID: appID)
            try? FileManager.default.trashItem(at: WinePrefix.compatData(appID), resultingItemURL: nil)
        }
        step = .done
    }

    // MARK: - Steps

    private func finish(appID: String, name: String, exe: URL, startDir: URL, arguments: String) {
        SteamIntegration.shared.addNonSteamGame(NonSteamGame(
            appID: appID, name: name, exe: exe.path, startDir: startDir.path, launchOptions: arguments))
        step = .done
    }

    private func newAppID() -> String {
        let shortcuts = SteamShortcuts.file().flatMap { try? SteamShortcuts.read($0) } ?? BinaryKeyValues()
        var taken = SteamShortcuts.appIDs(in: shortcuts)
        taken.formUnion(SteamIntegration.shared.nonSteamGames.map(\.appID))
        let compatData = Steam.root.appendingPathComponent("steamapps/compatdata", isDirectory: true)
        taken.formUnion((try? FileManager.default.contentsOfDirectory(atPath: compatData.path)) ?? [])
        return SteamShortcuts.newAppID(taken: taken)
    }

    /// Runs the installer through Wheelio's Steam compatibility tool, as
    /// Steam runs a game's own setup, and waits until everything it started
    /// has finished.
    private func runInstaller(appID: String) async throws {
        let compatData = WinePrefix.compatData(appID)
        try FileManager.default.createDirectory(at: compatData, withIntermediateDirectories: true)
        let command = file.pathExtension.lowercased() == "msi"
            ? ["msiexec", "/i", "Z:" + file.path.replacingOccurrences(of: "/", with: "\\")]
            : [file.path]
        try await run(SteamCompatTool.folder.appendingPathComponent("run"), ["runinprefix"] + command,
                      environment: ["STEAM_COMPAT_DATA_PATH": compatData.path],
                      in: file.deletingLastPathComponent(), keep: true)
        if let wineserver = wineserver {
            try await run(wineserver, ["-w"], environment: ["WINEPREFIX": compatData.appendingPathComponent("pfx").path])
        }
    }

    private func killWine(appID: String) {
        guard let wineserver else { return }
        let process = Process()
        process.executableURL = wineserver
        process.arguments = ["-k"]
        process.environment = ["WINEPREFIX": WinePrefix.forSteamApp(appID).folder.path]
        try? process.run()
    }

    private var wineserver: URL? {
        Engine.shared.launcher?.deletingLastPathComponent().appendingPathComponent("bin/wineserver")
    }

    private func run(_ executable: URL, _ arguments: [String], environment: [String: String],
                     in directory: URL? = nil, keep: Bool = false) async throws {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.environment = ProcessInfo.processInfo.environment.merging(environment) { $1 }
        if let directory { process.currentDirectoryURL = directory }
        if keep { installer = process }
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in
            process.terminationHandler = { _ in done.resume() }
            do {
                try process.run()
            } catch {
                process.terminationHandler = nil
                done.resume(throwing: error)
            }
        }
        if keep { installer = nil }
    }

    /// What the picker offers: Windows programs and installers.
    static let programTypes: [UTType] = ["exe", "msi"].compactMap { UTType(filenameExtension: $0) }
}
