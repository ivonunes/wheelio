import Foundation
import ServiceManagement

/// Launch-at-login toggle backed by SMAppService (macOS 13+). Registration needs
/// the app to be in /Applications and ideally signed; failures are logged and the
/// reported status reflects the real state.
enum LoginItem {
    static var isEnabled: Bool {
        SMAppService.mainApp.status == .enabled
    }

    @discardableResult
    static func setEnabled(_ enabled: Bool) -> Bool {
        do {
            if enabled {
                if SMAppService.mainApp.status != .enabled {
                    try SMAppService.mainApp.register()
                }
            } else {
                if SMAppService.mainApp.status == .enabled {
                    try SMAppService.mainApp.unregister()
                }
            }
        } catch {
            NSLog("Wheelio: failed to set launch-at-login to \(enabled): \(error.localizedDescription)")
        }
        return isEnabled
    }
}
