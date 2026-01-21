import Foundation
import Testing
import Wax

@Test
func optimizeSurrogatesCreatesSurrogateFramesAndExcludesFromDefaultSearch() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 8, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 80,
            expansionMaxTokens: 30,
            snippetMaxTokens: 15,
            maxSnippets: 10,
            searchTopK: 25,
            searchMode: .textOnly
        )

        let sentence = "Swift concurrency uses actors and tasks. Actors isolate mutable state."
        let content = Array(repeating: sentence, count: 40).joined(separator: " ")
        let expectedChunks = await TextChunker.chunk(text: content, strategy: config.chunking)

        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        try await orchestrator.remember(content, metadata: ["source": "test"])
        try await orchestrator.flush()

        let report = try await orchestrator.optimizeSurrogates()
        #expect(report.generatedSurrogates == expectedChunks.count)

        try await orchestrator.close()

        let wax = try await Wax.open(at: url)
        let stats = await wax.stats()
        #expect(stats.frameCount == UInt64(1 + expectedChunks.count + expectedChunks.count))

        let metas = await wax.frameMetas()
        let surrogates = metas.filter { $0.kind == "surrogate" }
        #expect(surrogates.count == expectedChunks.count)
        #expect(surrogates.allSatisfy { $0.metadata?.entries["source_frame_id"] != nil })
        #expect(surrogates.allSatisfy { $0.metadata?.entries["surrogate_algo"] != nil })
        #expect(surrogates.allSatisfy { $0.metadata?.entries["surrogate_version"] != nil })
        #expect(surrogates.allSatisfy { $0.metadata?.entries["source_content_hash"] != nil })

        let search = try await wax.search(.init(query: "actors", mode: .textOnly, topK: 50))
        for result in search.results {
            let meta = try await wax.frameMeta(frameId: result.frameId)
            #expect(meta.kind != "surrogate")
        }

        try await wax.close()
    }
}

@Test
func denseCachedRecallIncludesSurrogatesInContext() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 8, overlapTokens: 2)
        config.rag = FastRAGConfig(
            mode: .denseCached,
            maxContextTokens: 120,
            expansionMaxTokens: 30,
            snippetMaxTokens: 12,
            maxSnippets: 10,
            maxSurrogates: 4,
            surrogateMaxTokens: 20,
            searchTopK: 25,
            searchMode: .textOnly
        )

        let sentence = "Swift concurrency uses actors and tasks. Actors isolate mutable state."
        let content = Array(repeating: sentence, count: 40).joined(separator: " ")
        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        try await orchestrator.remember(content)
        try await orchestrator.flush()
        _ = try await orchestrator.optimizeSurrogates()

        let ctx = try await orchestrator.recall(query: "actors")
        #expect(ctx.items.contains { $0.kind == .surrogate })
        #expect(ctx.items.filter { $0.kind == .expanded }.count <= 1)

        // Ensure packing order: expansion first (if present), then surrogates, then snippets.
        if ctx.items.contains(where: { $0.kind == .expanded }) {
            #expect(ctx.items.first?.kind == .expanded)
            #expect(ctx.items.dropFirst().contains { $0.kind == .surrogate })
        }

        try await orchestrator.close()
    }
}
