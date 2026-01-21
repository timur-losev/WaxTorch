import Foundation
import WaxCore
import WaxTextSearch
import WaxVectorSearch

public extension Wax {
    func search(_ request: SearchRequest) async throws -> SearchResponse {
        let requestedTopK = max(0, request.topK)
        if requestedTopK == 0 {
            return SearchResponse(results: [])
        }

        let trimmedQuery = request.query?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let queryType: QueryType
        if let trimmedQuery, !trimmedQuery.isEmpty {
            queryType = RuleBasedQueryClassifier.classify(trimmedQuery)
        } else {
            queryType = .exploratory
        }

        let weights = AdaptiveFusionConfig.default.weights(for: queryType)
        let filter = request.frameFilter ?? FrameFilter()

        let includeText: Bool
        let includeVector: Bool
        switch request.mode {
        case .textOnly:
            includeText = true
            includeVector = false
        case .vectorOnly:
            includeText = false
            includeVector = true
        case .hybrid:
            includeText = true
            includeVector = true
        }

        let candidateLimit = Self.candidateLimit(for: requestedTopK)
        var textEngine: FTS5SearchEngine?
        if includeText, let bytes = await readStagedLexIndexBytes() {
            textEngine = try FTS5SearchEngine.deserialize(from: bytes)
        } else if includeText, let bytes = try await readCommittedLexIndexBytes() {
            textEngine = try FTS5SearchEngine.deserialize(from: bytes)
        } else if includeText {
            textEngine = try FTS5SearchEngine.inMemory()
        }

        let pendingEmbeddings = await pendingEmbeddingMutations()
        var vectorEngine: USearchVectorEngine?
        if includeVector, let embedding = request.embedding, !embedding.isEmpty {
            if let manifest = await committedVecIndexManifest(),
               let metric = VectorMetric(vecSimilarity: manifest.similarity) {
                let engine = try USearchVectorEngine(metric: metric, dimensions: Int(manifest.dimension))
                if let bytes = try await readCommittedVecIndexBytes() {
                    try await engine.deserialize(bytes)
                }
                for mutation in pendingEmbeddings {
                    try await engine.add(frameId: mutation.frameId, vector: mutation.vector)
                }
                vectorEngine = engine
            } else if let staged = await readStagedVecIndexBytes(),
                      let metric = VectorMetric(vecSimilarity: staged.similarity) {
                let engine = try USearchVectorEngine(metric: metric, dimensions: Int(staged.dimension))
                try await engine.deserialize(staged.bytes)
                for mutation in pendingEmbeddings {
                    try await engine.add(frameId: mutation.frameId, vector: mutation.vector)
                }
                vectorEngine = engine
            } else if let first = pendingEmbeddings.first,
                      first.dimension == UInt32(embedding.count) {
                let engine = try USearchVectorEngine(metric: .cosine, dimensions: Int(first.dimension))
                for mutation in pendingEmbeddings {
                    try await engine.add(frameId: mutation.frameId, vector: mutation.vector)
                }
                vectorEngine = engine
            }
        }

        async let textResultsAsync: [TextSearchResult] = {
            guard includeText, let textEngine, let trimmedQuery, !trimmedQuery.isEmpty else { return [] }
            let base = try await textEngine.search(query: trimmedQuery, topK: candidateLimit)
            guard base.isEmpty, let fallback = Self.orExpandedQuery(from: trimmedQuery) else {
                return base
            }
            return try await textEngine.search(query: fallback, topK: candidateLimit)
        }()

        async let vectorResultsAsync: [(frameId: UInt64, score: Float)] = {
            guard includeVector, let vectorEngine, let embedding = request.embedding, !embedding.isEmpty else { return [] }
            return try await vectorEngine.search(vector: embedding, topK: candidateLimit)
        }()

        let textResults = try await textResultsAsync
        let vectorResults = try await vectorResultsAsync

        var timelineFrameIds: [UInt64] = []
        if queryType == .temporal, weights.temporal > 0 {
            let timelineQuery = TimelineQuery(
                limit: max(candidateLimit, request.timelineFallbackLimit),
                order: .reverseChronological,
                after: request.timeRange?.after,
                before: request.timeRange?.before,
                includeDeleted: filter.includeDeleted,
                includeSuperseded: filter.includeSuperseded
            )
            timelineFrameIds = await timeline(timelineQuery)
                .filter { filter.includeSurrogates || $0.kind != "surrogate" }
                .map(\.id)
        }

        let snippetByFrameId: [UInt64: String] = textResults.reduce(into: [:]) { acc, result in
            guard let snippet = result.snippet, !snippet.isEmpty else { return }
            acc[result.frameId] = snippet
        }

        let baseResults: [(frameId: UInt64, score: Float, sources: [SearchResponse.Source])]
        switch request.mode {
        case .textOnly:
            baseResults = textResults.map { (frameId: $0.frameId, score: Float($0.score), sources: [.text]) }
        case .vectorOnly:
            baseResults = vectorResults.map { (frameId: $0.frameId, score: $0.score, sources: [.vector]) }
        case .hybrid(let alpha):
            let clampedAlpha = min(1, max(0, alpha))
            let textWeight = weights.bm25 * clampedAlpha
            let vectorWeight = weights.vector * (1 - clampedAlpha)

            let textIds = textResults.map(\.frameId)
            let vectorIds = vectorResults.map(\.frameId)
            let timelineIds = timelineFrameIds

            let fused = HybridSearch.rrfFusion(
                lists: [
                    (weight: textWeight, frameIds: textIds),
                    (weight: vectorWeight, frameIds: vectorIds),
                    (weight: weights.temporal, frameIds: timelineIds),
                ],
                k: request.rrfK
            )

            let textSet = Set(textIds)
            let vectorSet = Set(vectorIds)
            let timelineSet = Set(timelineIds)

            baseResults = fused.map { (frameId, score) in
                var sources: [SearchResponse.Source] = []
                if textSet.contains(frameId) { sources.append(.text) }
                if vectorSet.contains(frameId) { sources.append(.vector) }
                if timelineSet.contains(frameId) { sources.append(.timeline) }
                return (frameId: frameId, score: score, sources: sources)
            }
        }

        var filtered: [SearchResponse.Result] = []
        filtered.reserveCapacity(min(requestedTopK, baseResults.count))

        for item in baseResults {
            if let minScore = request.minScore, item.score < minScore { continue }

            let meta: FrameMeta
            if let committed = try? await frameMeta(frameId: item.frameId) {
                meta = committed
            } else if let pending = await pendingFrameMeta(frameId: item.frameId) {
                meta = pending
            } else {
                continue
            }

            if let timeRange = request.timeRange, !timeRange.contains(meta.timestamp) { continue }
            if let allowlist = filter.frameIds, !allowlist.contains(item.frameId) { continue }
            if !filter.includeDeleted, meta.status == .deleted { continue }
            if !filter.includeSuperseded, meta.supersededBy != nil { continue }
            if !filter.includeSurrogates, meta.kind == "surrogate" { continue }

            let previewText: String?
            if let snippet = snippetByFrameId[item.frameId] {
                previewText = snippet
            } else {
                let bytes = try? await framePreview(frameId: item.frameId, maxBytes: request.previewMaxBytes)
                previewText = bytes.flatMap { String(data: $0, encoding: .utf8) }
            }

            filtered.append(
                SearchResponse.Result(
                    frameId: item.frameId,
                    score: item.score,
                    previewText: previewText,
                    sources: item.sources
                )
            )

            if filtered.count >= requestedTopK {
                break
            }
        }

        if filtered.isEmpty, request.allowTimelineFallback {
            filtered = await timelineFallbackResults(request: request, filter: filter)
        }

        return SearchResponse(results: filtered)
    }

    private func timelineFallbackResults(request: SearchRequest, filter: FrameFilter) async -> [SearchResponse.Result] {
        if request.timelineFallbackLimit <= 0 { return [] }
        let query = TimelineQuery(
            limit: request.timelineFallbackLimit,
            order: .reverseChronological,
            after: request.timeRange?.after,
            before: request.timeRange?.before,
            includeDeleted: filter.includeDeleted,
            includeSuperseded: filter.includeSuperseded
        )

        var results: [SearchResponse.Result] = []
        results.reserveCapacity(max(0, request.timelineFallbackLimit))

        let frames = await timeline(query)
        for (rank, meta) in frames.enumerated() {
            let frameId = meta.id

            if let allowlist = filter.frameIds, !allowlist.contains(frameId) { continue }
            if !filter.includeDeleted, meta.status == .deleted { continue }
            if !filter.includeSuperseded, meta.supersededBy != nil { continue }
            if !filter.includeSurrogates, meta.kind == "surrogate" { continue }

            let score = 1 / Float(max(0, request.rrfK) + rank + 1)
            let bytes = try? await framePreview(frameId: frameId, maxBytes: request.previewMaxBytes)
            let previewText = bytes.flatMap { String(data: $0, encoding: .utf8) }

            results.append(
                SearchResponse.Result(
                    frameId: frameId,
                    score: score,
                    previewText: previewText,
                    sources: [.timeline]
                )
            )

            if results.count >= request.timelineFallbackLimit {
                break
            }
        }

        return results
    }

    private static func orExpandedQuery(from query: String, maxTokens: Int = 16) -> String? {
        let tokens = query.split(whereSeparator: { $0.isWhitespace })
        if tokens.isEmpty { return nil }
        let capped = tokens.prefix(max(0, maxTokens))

        let quoted = capped.map { raw -> String in
            let token = String(raw)
            let escaped = token.replacingOccurrences(of: "\"", with: "\"\"")
            return "\"\(escaped)\""
        }
        return quoted.joined(separator: " OR ")
    }

    private static func candidateLimit(for topK: Int) -> Int {
        guard topK > 0 else { return 0 }
        let expanded = topK > Int.max / 3 ? Int.max : topK * 3
        let capped = min(expanded, 1000)
        return max(topK, capped)
    }
}
