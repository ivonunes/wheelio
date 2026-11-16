import AppKit
import Combine

/// Windows games in the Mac Steam client: Wheelio installs its engine and
/// its Steam Play patch, registers itself as Steam's compatibility tool for
/// Windows games, and keeps all of it in step with its settings. Off until the
/// player switches it on, since it changes Steam.
///
/// Steam rewrites its config.vdf as it quits, and its app can't be changed
/// while it runs, so those changes wait until Steam isn't running and are
/// applied as soon as it quits.
@MainActor
final class SteamIntegration: ObservableObject {
    static let shared = SteamIntegration()

    @Published private(set) var enabled: Bool
    @Published private(set) var lastError: String?
    /// A change to Steam is waiting for Steam to quit.
    @Published private(set) var waitingForSteam = false
    /// Steam has updated to a version the patch doesn't support yet, so
    /// Windows games are unavailable in it for now.
    @Published private(set) var paused = false
    /// Games with a Mac version that the player plays in their Windows
    /// version instead (Steam plays Windows-only games through Wheelio anyway).
    @Published private(set) var windowsGames: [SteamGame]
    /// Windows games from elsewhere, added to Steam as non-Steam games.
    @Published private(set) var nonSteamGames: [NonSteamGame]
    /// Non-Steam games taken out of Wheelio, to take out of Steam.
    private var removedShortcuts: Set<String>

    private static let enabledKey = "steamIntegration"
    private static let windowsGamesKey = "steamWindowsGames"
    private static let nonSteamGamesKey = "steamNonSteamGames"
    private static let removedShortcutsKey = "steamRemovedShortcuts"
    private var observers: [AnyCancellable] = []

    private init() {
        enabled = UserDefaults.standard.bool(forKey: Self.enabledKey)
        windowsGames = UserDefaults.standard.data(forKey: Self.windowsGamesKey)
            .flatMap { try? JSONDecoder().decode([SteamGame].self, from: $0) } ?? []
        nonSteamGames = UserDefaults.standard.data(forKey: Self.nonSteamGamesKey)
            .flatMap { try? JSONDecoder().decode([NonSteamGame].self, from: $0) } ?? []
        removedShortcuts = Set(UserDefaults.standard.stringArray(forKey: Self.removedShortcutsKey) ?? [])
    }

    /// Starts following Wheelio's settings and Steam. Called once at launch.
    func start() {
        Engine.shared.$state.map { _ in () }
            .merge(with: GamePortingToolkit.shared.$installed.map { _ in () })
            .receive(on: DispatchQueue.main)
            .sink { [weak self] in self?.apply() }
            .store(in: &observers)

        let workspace = NSWorkspace.shared.notificationCenter
        func steam(_ name: Notification.Name) -> AnyPublisher<Void, Never> {
            workspace.publisher(for: name)
                .compactMap { $0.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication }
                .filter { $0.bundleIdentifier == Steam.bundleIdentifier }
                .map { _ in () }
                .eraseToAnyPublisher()
        }
        steam(NSWorkspace.didTerminateApplicationNotification)
            .sink { [weak self] in self?.apply() }
            .store(in: &observers)
        // The patch reports once Steam's client has loaded.
        steam(NSWorkspace.didLaunchApplicationNotification)
            .delay(for: .seconds(20), scheduler: DispatchQueue.main)
            .sink { [weak self] in self?.refreshPaused() }
            .store(in: &observers)
    }

    func setEnabled(_ value: Bool) {
        guard value != enabled else { return }
        enabled = value
        UserDefaults.standard.set(value, forKey: Self.enabledKey)
        if value { Engine.shared.install() }
        apply()
    }

    func addWindowsGame(_ game: SteamGame) {
        guard !windowsGames.contains(where: { $0.appID == game.appID }) else { return }
        setWindowsGames(windowsGames + [game])
    }

    func removeWindowsGame(_ game: SteamGame) {
        setWindowsGames(windowsGames.filter { $0.appID != game.appID })
    }

    private func setWindowsGames(_ games: [SteamGame]) {
        windowsGames = games.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        UserDefaults.standard.set(try? JSONEncoder().encode(windowsGames), forKey: Self.windowsGamesKey)
        apply()
    }

    func addNonSteamGame(_ game: NonSteamGame) {
        setNonSteamGames(nonSteamGames + [game])
        apply()
    }

    /// Takes the game out of Steam, and its Wine environment (with anything
    /// its installer put there) to the Bin.
    func removeNonSteamGame(_ game: NonSteamGame) {
        setNonSteamGames(nonSteamGames.filter { $0.appID != game.appID })
        if game.inSteam { setRemovedShortcuts(removedShortcuts.union([game.appID])) }
        try? FileManager.default.trashItem(at: WinePrefix.compatData(game.appID), resultingItemURL: nil)
        apply()
    }

    private func setNonSteamGames(_ games: [NonSteamGame]) {
        nonSteamGames = games.sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
        UserDefaults.standard.set(try? JSONEncoder().encode(nonSteamGames), forKey: Self.nonSteamGamesKey)
    }

    private func setRemovedShortcuts(_ appIDs: Set<String>) {
        removedShortcuts = appIDs
        UserDefaults.standard.set(appIDs.sorted(), forKey: Self.removedShortcutsKey)
    }

    /// Brings Steam's shortcuts in line with the non-Steam games: new ones
    /// added, removed ones taken out. One the player removed in Steam itself
    /// leaves Wheelio too. Only while Steam isn't running, since Steam
    /// rewrites the file; false while it waits for that, or for a first
    /// sign-in to Steam.
    private func updateShortcuts(running: Bool) throws -> Bool {
        let pending = nonSteamGames.contains { !$0.inSteam } || !removedShortcuts.isEmpty
        guard !running, let file = SteamShortcuts.file() else { return !pending }
        let shortcuts = try SteamShortcuts.read(file)
        let inSteam = SteamShortcuts.appIDs(in: shortcuts)
        let adding = nonSteamGames.filter { !$0.inSteam && !inSteam.contains($0.appID) }
        let removing = removedShortcuts.intersection(inSteam)
        if !adding.isEmpty || !removing.isEmpty {
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            try SteamShortcuts.updated(shortcuts, adding: adding, removing: removing).serialized()
                .write(to: file, options: .atomic)
        }
        let written = inSteam.union(adding.map(\.appID))
        setNonSteamGames(nonSteamGames.compactMap { game in
            if !game.inSteam { var added = game; added.inSteam = true; return added }
            return written.contains(game.appID) ? game : nil
        })
        setRemovedShortcuts([])
        return true
    }

    /// Brings Steam's side in line with the settings: the tool's folder now,
    /// Steam's choice of tool and the patch once Steam isn't running.
    private func apply() {
        lastError = nil
        let running = Steam.isRunning
        do {
            if enabled {
                guard let settings = toolSettings() else { return }  // the engine isn't ready yet
                try SteamCompatTool.install(settings)
                let shortcutsDone = try updateShortcuts(running: running)
                let games = Set(windowsGames.map(\.appID) + nonSteamGames.map(\.appID))
                let configured = try updateSteamConfig(running: running) {
                    try SteamCompatTool.configChoosingWheelio($0, windowsGames: games)
                }
                if !running { try SteamPatch.install() }
                waitingForSteam = !configured || !shortcutsDone || (running && SteamPatch.state() != .installed)
            } else {
                SteamCompatTool.uninstall()
                let configured = try updateSteamConfig(running: running, SteamCompatTool.configWithoutWheelio)
                if !running { try SteamPatch.remove() }
                waitingForSteam = !configured || (running && SteamPatch.state() == .installed)
            }
        } catch {
            lastError = "Couldn't update Steam: \(error.localizedDescription)"
        }
        refreshPaused()
    }

    /// Whether Steam's config is as wanted (false while it waits for Steam).
    private func updateSteamConfig(running: Bool, _ change: (String) throws -> String?) throws -> Bool {
        // Before Steam's first run there's no config yet, and nothing to change.
        guard FileManager.default.fileExists(atPath: Steam.configFile.path) else { return true }
        return try SteamCompatTool.updateConfig(steamRunning: running, change)
    }

    /// Paused when the patch said so as Steam last started, or when it
    /// wouldn't apply to the Steam client now on disk.
    private func refreshPaused() {
        guard enabled, SteamPatch.state() == .installed else {
            paused = false
            return
        }
        if let status = SteamPatch.status(), !status.patchApplied {
            paused = true
            return
        }
        Task.detached(priority: .utility) {
            let ready = SteamPatch.check().ready
            await MainActor.run { self.paused = !ready }
        }
    }

    private func toolSettings() -> SteamCompatTool.Settings? {
        guard let launcher = Engine.shared.launcher else { return nil }
        return SteamCompatTool.Settings(
            launcher: launcher,
            support: WheelioPaths.windowsSupport,
            d3dMetal: GamePortingToolkit.shared.installed ? GamePortingToolkit.folder : nil)
    }
}
