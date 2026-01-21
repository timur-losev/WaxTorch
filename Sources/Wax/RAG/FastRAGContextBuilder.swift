import Foundation
import WaxCore
import WaxTextSearch
import WaxVectorSearch

public struct FastRAGContextBuilder: Sendable {
    public init() {}

    /// Build a deterministic RAG context: at most one expansion + ranked snippets.
    /// - Parameters:
    ///   - query: user query string
    ///   - embedding: optional caller-supplied embedding (no query-time embedding inside Wax)
    ///   - wax: Wax handle
    ///   - config: Fast RAG configuration
    public func build(
        query: String,
        embedding: [Float]? = nil,
        wax: Wax,
        config: FastRAGConfig = .init()
    ) async throws -> RAGContext {
        let clamped = clamp(config)
        let counter = try await TokenCounter.shared()

        // 1) Run unified search
        let request = SearchRequest(
            query: query,
            embedding: embedding,
            mode: clamped.searchMode,
            topK: clamped.searchTopK,
            rrfK: clamped.rrfK,
            previewMaxBytes: clamped.previewMaxBytes
        )
        let response = try await wax.search(request)

        var items: [RAGContext.Item] = []
        var usedTokens = 0
        var expandedFrameId: UInt64?
        var surrogateSourceFrameIds: Set<UInt64> = []

        // 2) Expansion: first result with valid UTF-8 frame content
        if clamped.expansionMaxTokens > 0, clamped.expansionMaxBytes > 0 {
            for result in response.results {
                if let expanded = try await expansionText(
                    frameId: result.frameId,
                    wax: wax,
                    counter: counter,
                    maxTokens: clamped.expansionMaxTokens,
                    maxBytes: clamped.expansionMaxBytes
                ) {
                    usedTokens += await counter.count(expanded)
                    expandedFrameId = result.frameId
                    items.append(
                        .init(
                            kind: .expanded,
                            frameId: result.frameId,
                            score: result.score,
                            sources: result.sources,
                            text: expanded
                        )
                    )
                    break
                }
            }
        }

        // 3) Surrogates (denseCached mode)
        if clamped.mode == .denseCached,
           clamped.maxContextTokens > usedTokens,
           clamped.maxSurrogates > 0,
           clamped.surrogateMaxTokens > 0 {
            var remainingTokens = clamped.maxContextTokens - usedTokens
            var surrogateCount = 0

            for result in response.results {
                if let expandedFrameId, result.frameId == expandedFrameId { continue }
                guard surrogateCount < clamped.maxSurrogates else { break }
                guard remainingTokens > 0 else { break }

                guard let surrogateFrameId = await wax.surrogateFrameId(sourceFrameId: result.frameId) else { continue }
                let data = try await wax.frameContent(frameId: surrogateFrameId)
                guard let text = String(data: data, encoding: .utf8),
                      !text.isEmpty else { continue }

                let capped = await counter.truncate(text, maxTokens: min(clamped.surrogateMaxTokens, remainingTokens))
                guard !capped.isEmpty else { continue }

                let tokens = await counter.count(capped)
                guard tokens <= remainingTokens else { continue }

                items.append(
                    .init(
                        kind: .surrogate,
                        frameId: surrogateFrameId,
                        score: result.score,
                        sources: result.sources,
                        text: capped
                    )
                )
                surrogateSourceFrameIds.insert(result.frameId)
                surrogateCount += 1
                remainingTokens -= tokens
                if remainingTokens == 0 { break }
            }

            usedTokens = clamped.maxContextTokens - remainingTokens
        }

        // 4) Snippets
        if clamped.maxContextTokens > usedTokens {
            var remainingTokens = clamped.maxContextTokens - usedTokens
            var snippetCount = 0

            for result in response.results {
                if let expandedFrameId, result.frameId == expandedFrameId { continue }
                if surrogateSourceFrameIds.contains(result.frameId) { continue }
                guard snippetCount < clamped.maxSnippets else { break }
                guard let preview = result.previewText, !preview.isEmpty else { continue }

                let capped = await counter.truncate(preview, maxTokens: min(clamped.snippetMaxTokens, remainingTokens))
                guard !capped.isEmpty else { continue }

                let tokens = await counter.count(capped)
                guard tokens <= remainingTokens else { continue }

                items.append(
                    .init(
                        kind: .snippet,
                        frameId: result.frameId,
                        score: result.score,
                        sources: result.sources,
                        text: capped
                    )
                )
                snippetCount += 1
                remainingTokens -= tokens
                if remainingTokens == 0 { break }
            }
            usedTokens = clamped.maxContextTokens - remainingTokens
        }

        return RAGContext(query: query, items: items, totalTokens: usedTokens)
    }

    // MARK: - Helpers

    private func clamp(_ config: FastRAGConfig) -> FastRAGConfig {
        var c = config
        c.maxContextTokens = max(0, c.maxContextTokens)
        c.expansionMaxTokens = min(max(0, c.expansionMaxTokens), c.maxContextTokens)
        c.expansionMaxBytes = max(0, c.expansionMaxBytes)
        c.snippetMaxTokens = max(0, c.snippetMaxTokens)
        c.maxSnippets = max(0, c.maxSnippets)
        c.maxSurrogates = max(0, c.maxSurrogates)
        c.surrogateMaxTokens = max(0, c.surrogateMaxTokens)
        c.searchTopK = max(0, c.searchTopK)
        c.rrfK = max(0, c.rrfK)
        c.previewMaxBytes = max(0, c.previewMaxBytes)
        return c
    }

    private func expansionText(
        frameId: UInt64,
        wax: Wax,
        counter: TokenCounter,
        maxTokens: Int,
        maxBytes: Int
    ) async throws -> String? {
        guard maxTokens > 0, maxBytes > 0 else { return nil }

        let meta = try await wax.frameMetaIncludingPending(frameId: frameId)
        let canonicalBytes: UInt64
        if meta.canonicalEncoding == .plain {
            canonicalBytes = meta.payloadLength
        } else if let length = meta.canonicalLength {
            canonicalBytes = length
        } else {
            throw WaxError.invalidToc(reason: "missing canonical_length for frame \(frameId)")
        }
        guard canonicalBytes > 0 else { return nil }
        guard canonicalBytes <= UInt64(maxBytes) else { return nil }

        let data = try await wax.frameContentIncludingPending(frameId: frameId)
        try Self.validateExpansionPayloadSize(
            expectedBytes: canonicalBytes,
            actualBytes: data.count,
            maxBytes: maxBytes
        )
        guard let text = String(data: data, encoding: .utf8),
              !text.isEmpty else { return nil }
        let truncated = await counter.truncate(text, maxTokens: maxTokens)
        return truncated.isEmpty ? nil : truncated
    }

    static func validateExpansionPayloadSize(
        expectedBytes: UInt64,
        actualBytes: Int,
        maxBytes: Int
    ) throws {
        guard maxBytes > 0 else { return }
        if actualBytes > maxBytes {
            throw WaxError.io("expansion payload exceeds cap: \(actualBytes) > \(maxBytes)")
        }
        if actualBytes != Int(expectedBytes) {
            throw WaxError.io("expansion payload length mismatch: expected \(expectedBytes), got \(actualBytes)")
        }
    }
}
