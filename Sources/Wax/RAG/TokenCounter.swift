import Foundation
import TiktokenSwift

/// Deterministic token counter with a fixed encoding for all Phase 9 flows.
public actor TokenCounter {
    public enum Encoding: String, Sendable {
        case cl100kBase = "cl100k_base"
    }

    private static let sharedCache = TokenCounterCache()
    public static let maxTokenizationBytes = 8 * 1024 * 1024

    private let bpe: CoreBpe

    public init(encoding: Encoding = .cl100kBase) async throws {
        self.bpe = try await CoreBpe.loadEncoding(named: encoding.rawValue)
    }

    public static func shared(encoding: Encoding = .cl100kBase) async throws -> TokenCounter {
        try await sharedCache.counter(for: encoding)
    }

    public func count(_ text: String) -> Int {
        encode(text).count
    }

    public func truncate(_ text: String, maxTokens: Int) -> String {
        guard maxTokens > 0 else { return "" }
        let tokens = encode(text)
        if tokens.count <= maxTokens {
            return text
        }
        let sliced = Array(tokens.prefix(maxTokens))
        return decode(sliced)
    }

    public func encode(_ text: String) -> [UInt32] {
        bpe.encode(text: cappedInput(text), allowedSpecial: [])
    }

    public func decode(_ tokens: [UInt32]) -> String {
        (try? bpe.decode(tokens: tokens)) ?? ""
    }

    private func cappedInput(_ text: String) -> String {
        Self.cappedUTF8Prefix(text, maxBytes: Self.maxTokenizationBytes)
    }

    private static func cappedUTF8Prefix(_ text: String, maxBytes: Int) -> String {
        guard maxBytes > 0 else { return "" }

        var bytes = 0
        var endScalars = text.unicodeScalars.startIndex
        var idx = endScalars
        while idx < text.unicodeScalars.endIndex {
            let scalar = text.unicodeScalars[idx]
            let scalarBytes = scalar.utf8.count
            if bytes + scalarBytes > maxBytes { break }
            bytes += scalarBytes
            idx = text.unicodeScalars.index(after: idx)
            endScalars = idx
        }

        guard endScalars != text.unicodeScalars.endIndex else { return text }
        let end = endScalars.samePosition(in: text) ?? text.endIndex
        return String(text[..<end])
    }
}

private actor TokenCounterCache {
    private var counters: [TokenCounter.Encoding: TokenCounter] = [:]

    func counter(for encoding: TokenCounter.Encoding) async throws -> TokenCounter {
        if let cached = counters[encoding] { return cached }
        let created = try await TokenCounter(encoding: encoding)
        counters[encoding] = created
        return created
    }
}
