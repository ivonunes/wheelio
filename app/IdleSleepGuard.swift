import Foundation
import IOKit.pwr_mgt

/// Keeps the Mac from idle-sleeping the display, and so from locking, while a
/// game is being driven. Wheel input arrives through HID and does not count as
/// user activity, so a long drive with no mouse or keyboard would otherwise
/// end in a locked screen that stops delivering input to the game.
final class IdleSleepGuard {
    private var assertion: IOPMAssertionID = 0

    var active: Bool {
        get { assertion != 0 }
        set {
            guard newValue != active else { return }
            if newValue {
                var id: IOPMAssertionID = 0
                let result = IOPMAssertionCreateWithName(
                    kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
                    IOPMAssertionLevel(kIOPMAssertionLevelOn),
                    "Wheelio is driving a game" as CFString,
                    &id)
                if result == kIOReturnSuccess {
                    assertion = id
                }
            } else {
                IOPMAssertionRelease(assertion)
                assertion = 0
            }
        }
    }

    deinit {
        active = false
    }
}
