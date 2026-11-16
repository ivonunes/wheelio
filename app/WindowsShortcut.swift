import Foundation

/// What a Windows shortcut (.lnk, Microsoft's Shell Link format) starts:
/// enough to find the game an installer put in the Start menu.
struct WindowsShortcut: Equatable {
    /// The target's Windows path ("C:\Games\..."), or nil when the link only
    /// has one relative to itself.
    var target: String?
    var relativePath: String?
    var workingDirectory: String?
    var arguments: String?

    private enum Flag {
        static let hasTargetIDList: UInt32 = 0x1
        static let hasLinkInfo: UInt32 = 0x2
        static let hasName: UInt32 = 0x4
        static let hasRelativePath: UInt32 = 0x8
        static let hasWorkingDirectory: UInt32 = 0x10
        static let hasArguments: UInt32 = 0x20
        static let isUnicode: UInt32 = 0x80
    }

    private static let headerSize = 0x4C

    /// The shortcut in a .lnk file's bytes, or nil when they aren't one.
    init?(data: Data) {
        let bytes = [UInt8](data)
        func u16(_ at: Int) -> Int? {
            at + 2 <= bytes.count ? Int(bytes[at]) | Int(bytes[at + 1]) << 8 : nil
        }
        func u32(_ at: Int) -> UInt32? {
            guard at + 4 <= bytes.count else { return nil }
            return (0..<4).reduce(UInt32(0)) { $0 | UInt32(bytes[at + $1]) << (8 * $1) }
        }
        func ansi(_ at: Int) -> String? {
            guard at < bytes.count, let end = bytes[at...].firstIndex(of: 0) else { return nil }
            return String(decoding: bytes[at..<end], as: UTF8.self)
        }
        func utf16(_ at: Int, count: Int? = nil) -> String? {
            var units: [UInt16] = []
            var position = at
            while let unit = u16(position), count.map({ units.count < $0 }) ?? (unit != 0) {
                units.append(UInt16(unit))
                position += 2
            }
            if let count, units.count < count { return nil }
            return String(decoding: units, as: UTF16.self)
        }

        guard u32(0) == UInt32(Self.headerSize), let flags = u32(0x14) else { return nil }
        var offset = Self.headerSize

        if flags & Flag.hasTargetIDList != 0 {
            guard let size = u16(offset) else { return nil }
            offset += 2 + size
        }

        if flags & Flag.hasLinkInfo != 0 {
            guard let size = u32(offset), let header = u32(offset + 4),
                  let infoFlags = u32(offset + 8) else { return nil }
            // A local path (bit 0); a network share's has none.
            if infoFlags & 1 != 0, let base = u32(offset + 16), let suffix = u32(offset + 24) {
                if header >= 0x24, let unicodeBase = u32(offset + 28), let unicodeSuffix = u32(offset + 32) {
                    target = (utf16(offset + Int(unicodeBase)) ?? "") + (utf16(offset + Int(unicodeSuffix)) ?? "")
                } else {
                    target = (ansi(offset + Int(base)) ?? "") + (ansi(offset + Int(suffix)) ?? "")
                }
                if target?.isEmpty == true { target = nil }
            }
            offset += Int(size)
        }

        let unicode = flags & Flag.isUnicode != 0
        func next() -> String? {
            guard let count = u16(offset) else { return nil }
            let length = unicode ? count * 2 : count
            guard offset + 2 + length <= bytes.count else { return nil }
            defer { offset += 2 + length }
            if unicode { return utf16(offset + 2, count: count) }
            return String(decoding: bytes[offset + 2..<offset + 2 + count], as: UTF8.self)
        }
        if flags & Flag.hasName != 0 { _ = next() }
        if flags & Flag.hasRelativePath != 0 { relativePath = next() }
        if flags & Flag.hasWorkingDirectory != 0 { workingDirectory = next() }
        if flags & Flag.hasArguments != 0 { arguments = next() }

        guard target != nil || relativePath != nil else { return nil }
    }
}
