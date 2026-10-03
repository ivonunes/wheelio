import Foundation

/// A discovered CrossOver bottle.
struct Bottle: Identifiable, Hashable {
    let name: String
    let url: URL
    var id: String { name }

    // Identity is the bottle name, so a Picker selection keeps matching even if
    // the url representation differs slightly between refreshes.
    static func == (lhs: Bottle, rhs: Bottle) -> Bool { lhs.name == rhs.name }
    func hash(into hasher: inout Hasher) { hasher.combine(name) }
}

/// Architecture of a Windows PE executable.
enum PEMachine: Equatable {
    case x64
    case x86
    case arm64
    case unknown

    var displayName: String {
        switch self {
        case .x64: return "64-bit (x64)"
        case .x86: return "32-bit (x86)"
        case .arm64: return "ARM64"
        case .unknown: return "unknown"
        }
    }
}

/// A game we've installed the proxy for (persisted so it can be listed/uninstalled).
struct InstallRecord: Codable, Identifiable, Hashable {
    let bottleName: String
    let exePath: String
    let dllPath: String
    /// The OpenXR runtime is installed next to the game (VR on at install time
    /// or enabled later). Optional so records from before VR still decode.
    var vrInstalled: Bool?

    var id: String { dllPath }
    var exeName: String { (exePath as NSString).lastPathComponent }
    var gameDirectory: URL { URL(fileURLWithPath: exePath).deletingLastPathComponent() }
    var bottle: Bottle { Bottle(name: bottleName, url: CrossOver.bottlesDirectory.appendingPathComponent(bottleName)) }
}

enum InstallError: LocalizedError {
    case noProxyDLL
    case noVRRuntime
    case unsupportedArchitecture(PEMachine)
    case copyFailed(String)
    case overrideFailed(String)
    case vrRegistrationFailed(String)

    var errorDescription: String? {
        switch self {
        case .noProxyDLL:
            return "The proxy dinput8.dll isn't bundled in this build. "
                + "Build the Windows proxy and set WHEELIO_PROXY_DLL, or use a release build."
        case .noVRRuntime:
            return "The VR runtime isn't bundled in this build."
        case .unsupportedArchitecture(let machine):
            return "This game is \(machine.displayName); only 64-bit and 32-bit x86 games are supported."
        case .copyFailed(let detail):
            return "Couldn't copy the proxy next to the game: \(detail)"
        case .overrideFailed(let detail):
            return "Couldn't set the Wine dinput8 override.\n\(detail)"
        case .vrRegistrationFailed(let detail):
            return "Couldn't register the VR runtime in the bottle.\n\(detail)"
        }
    }
}

/// CrossOver integration: discover bottles, detect game architecture, and install
/// the FFB proxy (copy the DLL next to the game exe + set the bottle-wide
/// `dinput8 = native,builtin` override) without the user touching winecfg.
enum CrossOver {
    static var bottlesDirectory: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/CrossOver/Bottles", isDirectory: true)
    }

    /// CrossOver's bundled wine binary, used to set the registry override.
    static func wineBinaryURL() -> URL? {
        let path = "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
        return FileManager.default.isExecutableFile(atPath: path) ? URL(fileURLWithPath: path) : nil
    }

    static func isCrossOverInstalled() -> Bool { wineBinaryURL() != nil }

    /// Directories under Bottles/ that look like real bottles (have a drive_c).
    static func discoverBottles() -> [Bottle] {
        let fm = FileManager.default
        guard let entries = try? fm.contentsOfDirectory(
            at: bottlesDirectory, includingPropertiesForKeys: [.isDirectoryKey]) else {
            return []
        }
        return entries.compactMap { url -> Bottle? in
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue else { return nil }
            guard fm.fileExists(atPath: url.appendingPathComponent("drive_c").path) else { return nil }
            return Bottle(name: url.lastPathComponent, url: url)
        }
        .sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    /// The proxy DLL to install for a game of the given architecture: bundled
    /// in the app (dinput8.dll for 64-bit, dinput8_x86.dll for 32-bit), or the
    /// 64-bit one pointed at by WHEELIO_PROXY_DLL for local development builds.
    static func proxyDLLURL(for machine: PEMachine = .x64) -> URL? {
        let resource = machine == .x86 ? "dinput8_x86" : "dinput8"
        if let bundled = Bundle.main.url(forResource: resource, withExtension: "dll") {
            return bundled
        }
        if machine == .x64 {
            let env = ProcessInfo.processInfo.environment["WHEELIO_PROXY_DLL"] ?? ""
            if !env.isEmpty, FileManager.default.fileExists(atPath: env) {
                return URL(fileURLWithPath: env)
            }
        }
        return nil
    }

    static func supportsInstall(for machine: PEMachine) -> Bool {
        (machine == .x64 || machine == .x86) && proxyDLLURL(for: machine) != nil
    }

    /// Read the PE machine type from a Windows executable header.
    static func peMachine(of url: URL) -> PEMachine {
        guard let handle = try? FileHandle(forReadingFrom: url) else { return .unknown }
        defer { try? handle.close() }

        guard let dos = try? handle.read(upToCount: 0x40), dos.count >= 0x40 else { return .unknown }
        let header = [UInt8](dos)
        guard header[0] == 0x4D, header[1] == 0x5A else { return .unknown }  // "MZ"

        let lfanew = UInt64(header[0x3C]) | (UInt64(header[0x3D]) << 8)
            | (UInt64(header[0x3E]) << 16) | (UInt64(header[0x3F]) << 24)

        guard (try? handle.seek(toOffset: lfanew)) != nil,
              let peData = try? handle.read(upToCount: 6), peData.count >= 6 else {
            return .unknown
        }
        let pe = [UInt8](peData)
        guard pe[0] == 0x50, pe[1] == 0x45, pe[2] == 0, pe[3] == 0 else { return .unknown }  // "PE\0\0"

        switch UInt16(pe[4]) | (UInt16(pe[5]) << 8) {
        case 0x8664: return .x64
        case 0x014C: return .x86
        case 0xAA64: return .arm64
        default: return .unknown
        }
    }

    private static let crossOverRoot = "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver"

    /// Run a wine command against a bottle, returning (exitOK, combined output).
    private static func runWine(_ arguments: [String], bottle: Bottle) -> (ok: Bool, output: String) {
        guard let wine = wineBinaryURL() else {
            return (false, "CrossOver's wine binary was not found.")
        }
        let process = Process()
        process.executableURL = wine
        process.arguments = arguments
        var environment = ProcessInfo.processInfo.environment
        // CrossOver's wine wrapper resolves the bottle by NAME via CX_BOTTLE,
        // not by WINEPREFIX path — without it, it looks for a "default" bottle.
        environment["CX_ROOT"] = crossOverRoot
        environment["CX_BOTTLE"] = bottle.name
        environment["WINEDEBUG"] = "-all"
        process.environment = environment

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do {
            try process.run()
        } catch {
            return (false, "Failed to launch wine: \(error.localizedDescription)")
        }
        process.waitUntilExit()
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        return (process.terminationStatus == 0, String(data: data, encoding: .utf8) ?? "")
    }

    /// Set the bottle-wide `dinput8 = native,builtin` DLL override via wine's
    /// registry, then read it back to confirm it actually landed (wine can exit 0
    /// without applying). Idempotent. Returns wine output on failure.
    static func setDinput8Override(bottle: Bottle) -> (ok: Bool, output: String) {
        let add = runWine(
            ["reg", "add", #"HKCU\Software\Wine\DllOverrides"#,
             "/v", "dinput8", "/d", "native,builtin", "/f"],
            bottle: bottle)
        if !add.ok {
            return add
        }

        let query = runWine(
            ["reg", "query", #"HKCU\Software\Wine\DllOverrides"#, "/v", "dinput8"],
            bottle: bottle)
        // Wine may store the value as "native, builtin" (with a space), so
        // normalize before checking.
        let landed = query.output
            .replacingOccurrences(of: " ", with: "")
            .localizedCaseInsensitiveContains("native,builtin")
        if !landed {
            return (false, "The override did not take effect.\n\(query.output)")
        }
        return (true, query.output)
    }

    /// The bottle environment Wine needs to see the wheel. Wine reaches game
    /// controllers through SDL, and SDL leaves any controller Apple's
    /// GameController framework recognises (the G923 among them) to that
    /// framework, which only serves the frontmost app; Wine's device host
    /// never is, so the game saw no wheel. Without that backend SDL reads the
    /// wheel through IOKit as before.
    static let bottleEnvironment: [(name: String, value: String)] = [("SDL_JOYSTICK_MFI", "0")]

    /// `cxbottle.conf` with `bottleEnvironment` set in its
    /// `[EnvironmentVariables]` section: added when missing, corrected when
    /// set otherwise, the section created when absent. Returns nil when the
    /// file already says so.
    static func configWithBottleEnvironment(_ config: String) -> String? {
        var lines = config.components(separatedBy: "\n")
        var changed = false
        for (name, value) in bottleEnvironment {
            let entry = "\"\(name)\" = \"\(value)\""
            let isEntry = { (line: String) in
                line.trimmingCharacters(in: .whitespaces).hasPrefix("\"\(name)\"")
            }
            guard let section = lines.firstIndex(where: {
                $0.trimmingCharacters(in: .whitespaces) == "[EnvironmentVariables]"
            }) else {
                if lines.last == "" { lines.removeLast() }
                lines += ["", "[EnvironmentVariables]", entry, ""]
                changed = true
                continue
            }
            let end = lines[(section + 1)...].firstIndex { $0.hasPrefix("[") } ?? lines.endIndex
            if let existing = lines[(section + 1)..<end].firstIndex(where: isEntry) {
                if lines[existing] != entry {
                    lines[existing] = entry
                    changed = true
                }
            } else {
                // After the section's last entry, before any blank lines.
                var insertAt = end
                while insertAt > section + 1, lines[insertAt - 1].trimmingCharacters(in: .whitespaces).isEmpty {
                    insertAt -= 1
                }
                lines.insert(entry, at: insertAt)
                changed = true
            }
        }
        return changed ? lines.joined(separator: "\n") : nil
    }

    /// Writes `bottleEnvironment` into the bottle's config. CrossOver applies
    /// it the next time the bottle starts. Idempotent; failures are left for
    /// the next launch's refresh.
    @discardableResult
    static func applyBottleEnvironment(_ bottle: Bottle) -> Bool {
        let url = bottle.url.appendingPathComponent("cxbottle.conf")
        guard let config = try? String(contentsOf: url, encoding: .utf8) else { return false }
        guard let updated = configWithBottleEnvironment(config) else { return true }
        return (try? updated.write(to: url, atomically: true, encoding: .utf8)) != nil
    }

    /// Install the proxy for a game: verify arch, copy the DLL next to the exe,
    /// set the override, and record it. With VR on, the OpenXR runtime goes in
    /// too. Throws InstallError on failure.
    static func install(exe: URL, bottle: Bottle, vr: Bool) throws -> InstallRecord {
        let machine = peMachine(of: exe)
        guard machine == .x64 || machine == .x86 else { throw InstallError.unsupportedArchitecture(machine) }
        guard let proxy = proxyDLLURL(for: machine) else { throw InstallError.noProxyDLL }

        let destination = exe.deletingLastPathComponent().appendingPathComponent("dinput8.dll")
        let fm = FileManager.default
        do {
            if fm.fileExists(atPath: destination.path) {
                try fm.removeItem(at: destination)
            }
            try fm.copyItem(at: proxy, to: destination)
        } catch {
            throw InstallError.copyFailed(error.localizedDescription)
        }

        let result = setDinput8Override(bottle: bottle)
        guard result.ok else { throw InstallError.overrideFailed(result.output) }
        applyBottleEnvironment(bottle)

        var record = InstallRecord(bottleName: bottle.name, exePath: exe.path, dllPath: destination.path,
                                   vrInstalled: false)
        // VR is 64-bit only: the OpenXR runtime is a 64-bit DLL and no 32-bit
        // game ships OpenXR support.
        if vr && machine == .x64 {
            try installVRRuntime(for: record)
            record.vrInstalled = true
        } else {
            // Leftovers from an earlier setup would put the game in VR mode
            // with VR switched off.
            removeVRRuntime(for: record)
        }
        addRecord(record)
        if record.vrInstalled != true {
            repairOpenXRRegistration(bottle: bottle)
        }
        return record
    }

    /// Remove the installed proxy DLL and any VR runtime. Leaves the bottle-wide
    /// dinput8 override in place (harmless without a native DLL, and other
    /// games in the bottle may rely on it).
    static func uninstall(_ record: InstallRecord) {
        try? FileManager.default.removeItem(atPath: record.dllPath)
        if record.vrInstalled == true {
            removeVRRuntime(for: record)
        }
        removeRecord(record)
        repairOpenXRRegistration(bottle: record.bottle)
    }

    // MARK: - VR runtime
    //
    // VR support is an OpenXR runtime (wheelio_openxr.dll + manifest) next to
    // the game, plus the bottle-wide Khronos registry key pointing at that
    // manifest, which is how the game's OpenXR loader finds it. Without the key
    // and files the game runs exactly as it did before: no VR at all.

    private static let runtimeDLLName = "wheelio_openxr.dll"
    private static let runtimeManifestName = "wheelio_openxr.json"
    private static let openXRKey = #"HKLM\SOFTWARE\Khronos\OpenXR\1"#

    /// The bundled runtime files, or nil in a build without them.
    static func vrRuntimeURLs() -> (dll: URL, manifest: URL)? {
        guard let dll = Bundle.main.url(forResource: "wheelio_openxr", withExtension: "dll"),
              let manifest = Bundle.main.url(forResource: "wheelio_openxr", withExtension: "json") else {
            return nil
        }
        return (dll, manifest)
    }

    static func isVRRuntimeAvailable() -> Bool { vrRuntimeURLs() != nil }

    /// A path inside the bottle's drive_c as the game sees it (C:\...).
    private static func windowsPath(_ url: URL, in bottle: Bottle) -> String? {
        let driveC = bottle.url.appendingPathComponent("drive_c").path
        guard url.path.hasPrefix(driveC + "/") else { return nil }
        let relative = String(url.path.dropFirst(driveC.count + 1))
        return "C:\\" + relative.replacingOccurrences(of: "/", with: "\\")
    }

    static func installVRRuntime(for record: InstallRecord) throws {
        guard let runtime = vrRuntimeURLs() else { throw InstallError.noVRRuntime }
        let fm = FileManager.default
        let dll = record.gameDirectory.appendingPathComponent(runtimeDLLName)
        let manifest = record.gameDirectory.appendingPathComponent(runtimeManifestName)
        do {
            for (source, destination) in [(runtime.dll, dll), (runtime.manifest, manifest)] {
                if fm.fileExists(atPath: destination.path) {
                    try fm.removeItem(at: destination)
                }
                try fm.copyItem(at: source, to: destination)
            }
        } catch {
            throw InstallError.copyFailed(error.localizedDescription)
        }

        guard let manifestPath = windowsPath(manifest, in: record.bottle) else {
            throw InstallError.vrRegistrationFailed("The game is not inside the bottle's C: drive.")
        }
        let result = runWine(
            ["reg", "add", openXRKey, "/v", "ActiveRuntime", "/t", "REG_SZ", "/d", manifestPath, "/f"],
            bottle: record.bottle)
        guard result.ok else { throw InstallError.vrRegistrationFailed(result.output) }
    }

    static func removeVRRuntime(for record: InstallRecord) {
        let fm = FileManager.default
        try? fm.removeItem(at: record.gameDirectory.appendingPathComponent(runtimeDLLName))
        try? fm.removeItem(at: record.gameDirectory.appendingPathComponent(runtimeManifestName))
    }

    /// The registry key is per bottle. Point it at a game that still has the
    /// runtime, or remove it when none does.
    private static func repairOpenXRRegistration(bottle: Bottle) {
        let remaining = records().first { $0.bottleName == bottle.name && $0.vrInstalled == true }
        if let remaining,
           let manifestPath = windowsPath(remaining.gameDirectory.appendingPathComponent(runtimeManifestName),
                                          in: bottle) {
            _ = runWine(["reg", "add", openXRKey, "/v", "ActiveRuntime", "/t", "REG_SZ", "/d", manifestPath, "/f"],
                        bottle: bottle)
        } else {
            _ = runWine(["reg", "delete", openXRKey, "/v", "ActiveRuntime", "/f"], bottle: bottle)
        }
    }

    /// Turn VR on or off for every game set up here. Returns one message per
    /// game that could not be changed.
    static func applyVR(enabled: Bool) -> [String] {
        var failures: [String] = []
        var touchedBottles = Set<String>()
        for var record in records() {
            guard FileManager.default.fileExists(atPath: record.exePath) else { continue }
            if enabled && peMachine(of: URL(fileURLWithPath: record.exePath)) != .x64 {
                continue
            }
            if enabled {
                do {
                    try installVRRuntime(for: record)
                    record.vrInstalled = true
                    addRecord(record)
                } catch {
                    failures.append("\(record.exeName): \(error.localizedDescription)")
                }
            } else {
                removeVRRuntime(for: record)
                record.vrInstalled = false
                addRecord(record)
                touchedBottles.insert(record.bottleName)
            }
        }
        for name in touchedBottles {
            repairOpenXRRegistration(bottle: Bottle(name: name, url: bottlesDirectory.appendingPathComponent(name)))
        }
        return failures
    }

    /// Re-copy the current bundled proxy (and VR runtime, where installed) over
    /// any previously installed copies whose bytes differ — e.g. after the app
    /// updates to a newer DLL. Silent and safe: only touches files we recorded
    /// installing, only when the content actually differs, and skips entries
    /// whose game/bottle is gone. Returns the number of files refreshed.
    @discardableResult
    static func refreshInstalledProxies() -> Int {
        let fm = FileManager.default
        var refreshed = 0

        func refresh(_ source: URL, at destination: URL) {
            guard fm.fileExists(atPath: destination.path), let sourceData = try? Data(contentsOf: source) else { return }
            if let existing = try? Data(contentsOf: destination), existing == sourceData { return }
            do {
                try fm.removeItem(at: destination)
                try fm.copyItem(at: source, to: destination)
                refreshed += 1
            } catch {
                // Leave the stale copy in place; retried on the next launch.
            }
        }

        let runtime = vrRuntimeURLs()
        let records = records()
        for bottle in Set(records.map(\.bottle)) {
            applyBottleEnvironment(bottle)
        }
        for record in records {
            if let proxy = proxyDLLURL(for: peMachine(of: URL(fileURLWithPath: record.exePath))) {
                refresh(proxy, at: URL(fileURLWithPath: record.dllPath))
            }
            if record.vrInstalled == true, let runtime {
                refresh(runtime.dll, at: record.gameDirectory.appendingPathComponent(runtimeDLLName))
                refresh(runtime.manifest, at: record.gameDirectory.appendingPathComponent(runtimeManifestName))
            }
        }
        return refreshed
    }

    // MARK: - Persistence

    private static let recordsKey = "installedGames"
    // Serializes record read-modify-write so a launch-time refresh can't race an
    // install (both mutate the same UserDefaults key from background threads).
    private static let recordsQueue = DispatchQueue(label: "uk.ivonunes.wheelio.records")

    static func records() -> [InstallRecord] {
        recordsQueue.sync { loadRecordsUnsynced() }
    }

    private static func addRecord(_ record: InstallRecord) {
        recordsQueue.sync {
            var records = loadRecordsUnsynced().filter { $0.dllPath != record.dllPath }
            records.append(record)
            saveUnsynced(records)
        }
    }

    private static func removeRecord(_ record: InstallRecord) {
        recordsQueue.sync {
            saveUnsynced(loadRecordsUnsynced().filter { $0.id != record.id })
        }
    }

    private static func loadRecordsUnsynced() -> [InstallRecord] {
        guard let data = UserDefaults.standard.data(forKey: recordsKey),
              let records = try? JSONDecoder().decode([InstallRecord].self, from: data) else {
            return []
        }
        return records
    }

    private static func saveUnsynced(_ records: [InstallRecord]) {
        if let data = try? JSONEncoder().encode(records) {
            UserDefaults.standard.set(data, forKey: recordsKey)
        }
    }
}
