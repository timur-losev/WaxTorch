import Foundation
import Testing
import Wax

@Test
func memoryOrchestratorRememberFlushRecallReopenAndNoSidecars() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 80,
            expansionMaxTokens: 30,
            snippetMaxTokens: 15,
            maxSnippets: 10,
            searchTopK: 25,
            searchMode: .textOnly
        )

        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        try await orchestrator.remember(
            "Swift concurrency uses actors and tasks. Actors isolate mutable state and enable safe parallelism.",
            metadata: ["source": "test"]
        )
        try await orchestrator.flush()

        let ctx1 = try await orchestrator.recall(query: "actors")
        #expect(!ctx1.items.isEmpty)
        #expect(ctx1.items.filter { $0.kind == .expanded }.count <= 1)

        let baseName = url.deletingPathExtension().lastPathComponent
        let dir = url.deletingLastPathComponent()
        let contents = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        let related = contents.filter { $0.lastPathComponent.hasPrefix(baseName) }
        #expect(related.count == 1)

        try await orchestrator.close()

        let reopened = try await MemoryOrchestrator(at: url, config: config)
        let ctx2 = try await reopened.recall(query: "actors")
        #expect(!ctx2.items.isEmpty)
        try await reopened.close()
    }
}

@Test
func memoryOrchestratorRecallWithoutFlushFindsRecentText() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 80,
            expansionMaxTokens: 30,
            snippetMaxTokens: 15,
            maxSnippets: 10,
            searchTopK: 25,
            searchMode: .textOnly
        )

        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        try await orchestrator.remember(
            "Swift concurrency uses actors and tasks. Actors isolate mutable state.",
            metadata: ["source": "test"]
        )

        let ctx = try await orchestrator.recall(query: "actors")
        #expect(!ctx.items.isEmpty)
        #expect(ctx.items.filter { $0.kind == .expanded }.count <= 1)

        try await orchestrator.close()
    }
}

@Test
func memoryOrchestratorSessionTaggingAndChunkMetadataPersist() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 8, overlapTokens: 2)

        let content = "Swift concurrency uses actors and tasks.".repeating(times: 30)
        let expectedChunks = await TextChunker.chunk(text: content, strategy: config.chunking)

        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        let session = await orchestrator.startSession()
        try await orchestrator.remember(content, metadata: ["k": "v"])
        await orchestrator.endSession()
        try await orchestrator.close()

        let wax = try await Wax.open(at: url)
        let stats = await wax.stats()
        #expect(stats.frameCount == UInt64(expectedChunks.count + 1))

        let doc = try await wax.frameMeta(frameId: 0)
        #expect(doc.role == .document)
        #expect(doc.metadata?.entries["session_id"] == session.uuidString)
        #expect(doc.metadata?.entries["k"] == "v")

        for (idx, chunk) in expectedChunks.enumerated() {
            let frameId = UInt64(idx + 1)
            let meta = try await wax.frameMeta(frameId: frameId)
            #expect(meta.role == .chunk)
            #expect(meta.parentId == 0)
            #expect(meta.chunkIndex == UInt32(idx))
            #expect(meta.chunkCount == UInt32(expectedChunks.count))
            #expect(meta.searchText == chunk)
            #expect(meta.metadata?.entries["session_id"] == session.uuidString)
            #expect(meta.metadata?.entries["k"] == "v")
        }

        try await wax.close()
    }
}

@Test
func memoryOrchestratorEnableVectorSearchRequiresEmbedder() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = true

        do {
            _ = try await MemoryOrchestrator(at: url, config: config, embedder: nil)
            #expect(Bool(false))
        } catch {
            #expect(Bool(true))
        }
    }
}

@Test
func memoryOrchestratorVectorSearchStagesVecIndexOnFlush() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = true
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)

        let orchestrator = try await MemoryOrchestrator(at: url, config: config, embedder: TestEmbedder())
        try await orchestrator.remember("Swift concurrency uses actors and tasks.")
        try await orchestrator.close()

        let wax = try await Wax.open(at: url)
        #expect(await wax.committedVecIndexManifest() != nil)
        try await wax.close()
    }
}

@Test
func memoryOrchestratorVectorRecallWithEmbeddingUsesVectorResults() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = true
        config.enableTextSearch = false
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 80,
            expansionMaxTokens: 30,
            snippetMaxTokens: 15,
            maxSnippets: 10,
            searchTopK: 25,
            searchMode: .vectorOnly
        )

        let embedder = TestEmbedder()
        let orchestrator = try await MemoryOrchestrator(at: url, config: config, embedder: embedder)
        try await orchestrator.remember("Swift concurrency uses actors and tasks.")
        try await orchestrator.flush()

        let embedding = try await embedder.embed("Swift concurrency uses actors and tasks.")
        let ctx = try await orchestrator.recall(query: "irrelevant", embedding: embedding)
        #expect(!ctx.items.isEmpty)
        #expect(ctx.items.contains { $0.sources.contains(.vector) })

        try await orchestrator.close()
    }
}

@Test
func memoryOrchestratorReopenVectorSearchWithoutEmbedderAllowsRecallWithEmbedding() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = true
        config.enableTextSearch = false
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 80,
            expansionMaxTokens: 30,
            snippetMaxTokens: 15,
            maxSnippets: 10,
            searchTopK: 25,
            searchMode: .vectorOnly
        )

        let embedder = TestEmbedder()
        let orchestrator = try await MemoryOrchestrator(at: url, config: config, embedder: embedder)
        try await orchestrator.remember("Swift concurrency uses actors and tasks.")
        try await orchestrator.close()

        let reopened = try await MemoryOrchestrator(at: url, config: config, embedder: nil)
        let queryEmbedding = try await embedder.embed("Swift concurrency uses actors and tasks.")
        let ctx = try await reopened.recall(query: "irrelevant", embedding: queryEmbedding)
        #expect(!ctx.items.isEmpty)
        #expect(ctx.items.contains { $0.sources.contains(.vector) })
        try await reopened.close()
    }
}

@Test
func memoryOrchestratorRecallWithEmbeddingPolicyUsesEmbedderWhenAvailable() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = true
        config.enableTextSearch = false
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 80,
            expansionMaxTokens: 30,
            snippetMaxTokens: 15,
            maxSnippets: 10,
            searchTopK: 25,
            searchMode: .vectorOnly
        )

        let orchestrator = try await MemoryOrchestrator(at: url, config: config, embedder: TestEmbedder())
        try await orchestrator.remember("Swift concurrency uses actors and tasks.")
        try await orchestrator.flush()

        let ctx = try await orchestrator.recall(query: "irrelevant", embeddingPolicy: .ifAvailable)
        #expect(!ctx.items.isEmpty)
        #expect(ctx.items.contains { $0.sources.contains(.vector) })

        try await orchestrator.close()
    }
}

private struct TestEmbedder: EmbeddingProvider, Sendable {
    let dimensions: Int = 2
    let normalize: Bool = false
    let identity: EmbeddingIdentity? = EmbeddingIdentity(
        provider: "Test",
        model: "Deterministic",
        dimensions: 2,
        normalized: false
    )

    func embed(_ text: String) async throws -> [Float] {
        let a = Float(text.utf8.count % 97) / 97.0
        let b = Float(text.unicodeScalars.count % 89) / 89.0
        return [a, b]
    }
}

private extension String {
    func repeating(times: Int) -> String {
        guard times > 1 else { return self }
        return Array(repeating: self, count: times).joined(separator: " ")
    }
}

#if canImport(WaxVectorSearchMiniLM)
import WaxVectorSearchMiniLM

@Test
func miniLMAdapterSymbolsExistWhenAvailable() async {
    _ = MiniLMEmbedder.self
    _ = MemoryOrchestrator.openMiniLM
    #expect(Bool(true))
}
#endif
