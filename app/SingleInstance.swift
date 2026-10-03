import AppKit

/// Wheelio owns the wheel and the bridge port, so only one copy may run. A
/// second copy (launched with `open -n`, or another build at a different path
/// with the same bundle id) hands over to the one already running and quits
/// before it touches either. Only an earlier-launched copy counts, so two
/// copies starting at once can't both quit.
enum SingleInstance {
    static func handOverIfAlreadyRunning() {
        guard let bundleID = Bundle.main.bundleIdentifier else { return }
        let current = NSRunningApplication.current
        let launched = current.launchDate ?? Date()
        let earlier = NSRunningApplication.runningApplications(withBundleIdentifier: bundleID).first {
            $0.processIdentifier != current.processIdentifier && !$0.isTerminated
                && ($0.launchDate ?? .distantPast) <= launched
        }
        guard let earlier else { return }
        earlier.activate()
        exit(0)
    }
}
