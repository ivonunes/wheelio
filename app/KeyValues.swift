import Foundation

/// Valve's KeyValues text format (.vdf), as Steam's config files use it: keys
/// with a string value or a braced block, in order. Parsed into a tree that
/// writes back as Steam writes it, keeping every entry it wasn't asked to
/// change. Keys match without regard to case, as Steam reads them.
struct KeyValues: Equatable {
    enum Value: Equatable {
        case string(String)
        case block(KeyValues)
    }

    struct Entry: Equatable {
        var key: String
        var value: Value
    }

    enum ParseError: Error, Equatable {
        case unexpectedEnd
        case unexpected(String, line: Int)
        // Platform conditionals ("[$WIN32]") aren't used by the files we edit,
        // and a file we can't read fully is one we mustn't rewrite.
        case unsupportedConditional(line: Int)
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

    func block(_ key: String) -> KeyValues? {
        if case .block(let block)? = value(key) { return block }
        return nil
    }

    func block(at path: [String]) -> KeyValues? {
        path.reduce(Optional(self)) { $0?.block($1) }
    }

    /// Sets a key's value, in place when it's there, or added at the end.
    mutating func set(_ key: String, _ value: Value) {
        if let index = entries.firstIndex(where: { $0.key.caseInsensitiveCompare(key) == .orderedSame }) {
            entries[index].value = value
        } else {
            entries.append(Entry(key: key, value: value))
        }
    }

    mutating func remove(_ key: String) {
        entries.removeAll { $0.key.caseInsensitiveCompare(key) == .orderedSame }
    }

    /// Changes the block at `path`, making any block on the way that's missing.
    mutating func updateBlock(at path: [String], _ change: (inout KeyValues) -> Void) {
        guard let first = path.first else {
            change(&self)
            return
        }
        var child = block(first) ?? KeyValues()
        child.updateBlock(at: Array(path.dropFirst()), change)
        set(first, .block(child))
    }

    // MARK: - Text

    static func parse(_ text: String) throws -> KeyValues {
        var parser = Parser(text: Array(text.unicodeScalars))
        return try parser.parseEntries(topLevel: true)
    }

    /// The file's text, laid out as Steam writes it.
    func serialized() -> String {
        var out = ""
        write(into: &out, depth: 0)
        return out
    }

    private func write(into out: inout String, depth: Int) {
        let indent = String(repeating: "\t", count: depth)
        for entry in entries {
            switch entry.value {
            case .string(let text):
                out += "\(indent)\(Self.quoted(entry.key))\t\t\(Self.quoted(text))\n"
            case .block(let block):
                out += "\(indent)\(Self.quoted(entry.key))\n\(indent){\n"
                block.write(into: &out, depth: depth + 1)
                out += "\(indent)}\n"
            }
        }
    }

    private static func quoted(_ text: String) -> String {
        "\"" + text.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
    }

    private struct Parser {
        let text: [Unicode.Scalar]
        var index = 0
        var line = 1

        enum Token: Equatable {
            case text(String)
            case open
            case close
        }

        mutating func parseEntries(topLevel: Bool) throws -> KeyValues {
            var result = KeyValues()
            while true {
                guard let token = try nextToken() else {
                    if topLevel { return result }
                    throw ParseError.unexpectedEnd
                }
                switch token {
                case .close:
                    if topLevel { throw ParseError.unexpected("}", line: line) }
                    return result
                case .open:
                    throw ParseError.unexpected("{", line: line)
                case .text(let key):
                    guard let next = try nextToken() else { throw ParseError.unexpectedEnd }
                    switch next {
                    case .text(let value):
                        result.entries.append(Entry(key: key, value: .string(value)))
                    case .open:
                        result.entries.append(Entry(key: key, value: .block(try parseEntries(topLevel: false))))
                    case .close:
                        throw ParseError.unexpected("}", line: line)
                    }
                }
            }
        }

        private mutating func nextToken() throws -> Token? {
            skipSpaceAndComments()
            guard index < text.count else { return nil }
            let scalar = text[index]
            switch scalar {
            case "{":
                index += 1
                return .open
            case "}":
                index += 1
                return .close
            case "[":
                throw ParseError.unsupportedConditional(line: line)
            case "\"":
                index += 1
                return .text(try quotedText())
            default:
                return .text(bareText())
            }
        }

        private mutating func quotedText() throws -> String {
            var out = String.UnicodeScalarView()
            while index < text.count {
                let scalar = text[index]
                index += 1
                switch scalar {
                case "\"":
                    return String(out)
                case "\\" where index < text.count:
                    let escaped = text[index]
                    index += 1
                    switch escaped {
                    case "n": out.append("\n")
                    case "t": out.append("\t")
                    default: out.append(escaped)
                    }
                default:
                    if scalar == "\n" { line += 1 }
                    out.append(scalar)
                }
            }
            throw ParseError.unexpectedEnd
        }

        private mutating func bareText() -> String {
            var out = String.UnicodeScalarView()
            while index < text.count {
                let scalar = text[index]
                if scalar.properties.isWhitespace || scalar == "{" || scalar == "}" || scalar == "\"" { break }
                out.append(scalar)
                index += 1
            }
            return String(out)
        }

        private mutating func skipSpaceAndComments() {
            while index < text.count {
                let scalar = text[index]
                if scalar == "\n" {
                    line += 1
                    index += 1
                } else if scalar.properties.isWhitespace {
                    index += 1
                } else if scalar == "/", index + 1 < text.count, text[index + 1] == "/" {
                    while index < text.count, text[index] != "\n" { index += 1 }
                } else {
                    return
                }
            }
        }
    }
}
