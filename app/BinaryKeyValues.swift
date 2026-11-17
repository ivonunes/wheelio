import Foundation

/// Valve's binary KeyValues format, as Steam's shortcuts.vdf uses it: typed
/// entries in order, each a type byte, a null-terminated key and its value,
/// with blocks closed by an end byte. Parsed into a tree that writes back
/// byte for byte, so entries it wasn't asked to change stay as Steam wrote
/// them. Keys match without regard to case, as Steam reads them.
struct BinaryKeyValues: Equatable {
    enum Value: Equatable {
        case block(BinaryKeyValues)
        case string(String)
        case int32(Int32)
        case float(Float32)
        case uint64(UInt64)
        case int64(Int64)
        /// The rarer four-byte types (pointer, colour), kept as they are.
        case raw(type: UInt8, Data)
    }

    struct Entry: Equatable {
        var key: String
        var value: Value
    }

    enum ParseError: Error, Equatable {
        case unexpectedEnd
        // A type the format has but Steam's files don't use; a file we can't
        // read fully is one we mustn't rewrite.
        case unsupportedType(UInt8, offset: Int)
    }

    private enum Tag {
        static let block: UInt8 = 0
        static let string: UInt8 = 1
        static let int32: UInt8 = 2
        static let float: UInt8 = 3
        static let pointer: UInt8 = 4
        static let color: UInt8 = 6
        static let uint64: UInt8 = 7
        static let end: UInt8 = 8
        static let int64: UInt8 = 10
    }

    var entries: [Entry] = []

    // MARK: - Reading and changing

    func value(_ key: String) -> Value? {
        entries.first { $0.key.caseInsensitiveCompare(key) == .orderedSame }?.value
    }

    func string(_ key: String) -> String? {
        if case .string(let text)? = value(key) { return text }
        return nil
    }

    func int32(_ key: String) -> Int32? {
        if case .int32(let number)? = value(key) { return number }
        return nil
    }

    func block(_ key: String) -> BinaryKeyValues? {
        if case .block(let block)? = value(key) { return block }
        return nil
    }

    /// Sets a key's value, in place when it's there, or added at the end.
    mutating func set(_ key: String, _ value: Value) {
        if let index = entries.firstIndex(where: { $0.key.caseInsensitiveCompare(key) == .orderedSame }) {
            entries[index].value = value
        } else {
            entries.append(Entry(key: key, value: value))
        }
    }

    // MARK: - Parsing

    static func parse(_ data: Data) throws -> BinaryKeyValues {
        var reader = Reader(bytes: [UInt8](data))
        return try reader.block(topLevel: true)
    }

    private struct Reader {
        let bytes: [UInt8]
        var offset = 0

        mutating func block(topLevel: Bool) throws -> BinaryKeyValues {
            var result = BinaryKeyValues()
            while true {
                // The file's own block may end without its end byte.
                if topLevel && offset == bytes.count { return result }
                let type = try byte()
                if type == Tag.end { return result }
                let key = try string()
                let value: Value
                switch type {
                case Tag.block: value = .block(try block(topLevel: false))
                case Tag.string: value = .string(try string())
                case Tag.int32: value = .int32(Int32(bitPattern: try littleEndian(4).uint32))
                case Tag.float: value = .float(Float32(bitPattern: try littleEndian(4).uint32))
                case Tag.pointer, Tag.color: value = .raw(type: type, Data(try take(4)))
                case Tag.uint64: value = .uint64(try littleEndian(8))
                case Tag.int64: value = .int64(Int64(bitPattern: try littleEndian(8)))
                default: throw ParseError.unsupportedType(type, offset: offset)
                }
                result.entries.append(Entry(key: key, value: value))
            }
        }

        mutating func byte() throws -> UInt8 {
            guard offset < bytes.count else { throw ParseError.unexpectedEnd }
            defer { offset += 1 }
            return bytes[offset]
        }

        mutating func take(_ count: Int) throws -> ArraySlice<UInt8> {
            guard offset + count <= bytes.count else { throw ParseError.unexpectedEnd }
            defer { offset += count }
            return bytes[offset..<offset + count]
        }

        mutating func littleEndian(_ count: Int) throws -> UInt64 {
            try take(count).reversed().reduce(0) { $0 << 8 | UInt64($1) }
        }

        mutating func string() throws -> String {
            guard let end = bytes[offset...].firstIndex(of: 0) else { throw ParseError.unexpectedEnd }
            defer { offset = end + 1 }
            return String(decoding: bytes[offset..<end], as: UTF8.self)
        }
    }

    // MARK: - Writing

    func serialized() -> Data {
        var data = Data()
        write(to: &data)
        return data
    }

    private func write(to data: inout Data) {
        func put(_ text: String) {
            data.append(contentsOf: Array(text.utf8))
            data.append(0)
        }
        func put<T: FixedWidthInteger>(_ number: T) {
            withUnsafeBytes(of: number.littleEndian) { data.append(contentsOf: $0) }
        }
        for entry in entries {
            switch entry.value {
            case .block(let block):
                data.append(Tag.block); put(entry.key)
                block.write(to: &data)
            case .string(let text):
                data.append(Tag.string); put(entry.key); put(text)
            case .int32(let number):
                data.append(Tag.int32); put(entry.key); put(number)
            case .float(let number):
                data.append(Tag.float); put(entry.key); put(number.bitPattern)
            case .uint64(let number):
                data.append(Tag.uint64); put(entry.key); put(number)
            case .int64(let number):
                data.append(Tag.int64); put(entry.key); put(number)
            case .raw(let type, let bytes):
                data.append(type); put(entry.key); data.append(bytes)
            }
        }
        data.append(Tag.end)
    }
}

private extension UInt64 {
    var uint32: UInt32 { UInt32(truncatingIfNeeded: self) }
}
