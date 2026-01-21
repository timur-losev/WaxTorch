import Foundation

public enum TextChunker {
    /// Deterministic, token-aware chunking using the same encoding as Fast RAG.
    /// - Returns: array of chunk strings (UTF-8), or `[text]` when it fits.
    public static func chunk(text: String, strategy: ChunkingStrategy) async -> [String] {
        switch strategy {
        case let .tokenCount(targetTokens, overlapTokens):
            return await tokenCountChunk(text: text, targetTokens: targetTokens, overlapTokens: overlapTokens)
        }
    }

    private static func tokenCountChunk(text: String, targetTokens: Int, overlapTokens: Int) async -> [String] {
        let cappedTarget = max(1, targetTokens)
        let cappedOverlap = max(0, overlapTokens)

        guard let counter = try? await TokenCounter.shared() else { return [text] }
        let tokens = await counter.encode(text)
        if tokens.count <= cappedTarget {
            return [text]
        }

        var chunks: [String] = []
        var start = 0
        while start < tokens.count {
            let end = min(start + cappedTarget, tokens.count)
            let slice = Array(tokens[start..<end])
            let chunk = await counter.decode(slice)
            if !chunk.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                chunks.append(chunk)
            }
            if end == tokens.count { break }
            let proposed = end - cappedOverlap
            let nextStart = proposed > start ? proposed : end
            if nextStart <= start { break } // safety
            start = nextStart
        }
        if chunks.isEmpty {
            return [text]
        }
        return chunks
    }
}
