import Foundation

/// Where Wheelio keeps its own files.
enum WheelioPaths {
    /// ~/Library/Application Support/Wheelio
    static var support: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Application Support/Wheelio", isDirectory: true)
    }

    /// The Windows files each game gets from Wheelio (the wheel proxy, 64- and
    /// 32-bit, and the OpenXR runtime), bundled in the app's Resources.
    static var windowsSupport: URL {
        Bundle.main.resourceURL ?? Bundle.main.bundleURL
    }

    static var isVRRuntimeBundled: Bool {
        Bundle.main.url(forResource: "wheelio_openxr", withExtension: "dll") != nil
    }
}
