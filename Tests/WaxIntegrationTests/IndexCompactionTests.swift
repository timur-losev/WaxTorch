import Foundation
import Testing
import Wax

@Test
func compactIndexesDoesNotCreateSidecarsAndRecallStillWorks() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 120,
            expansionMaxTokens: 40,
            snippetMaxTokens: 20,
            maxSnippets: 12,
            searchTopK: 25,
            searchMode: .textOnly
        )

        let sentence = "Swift concurrency uses actors and tasks. Actors isolate mutable state."
        let content = Array(repeating: sentence, count: 60).joined(separator: " ")

        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        try await orchestrator.remember(content)
        try await orchestrator.flush()
        _ = try await orchestrator.compactIndexes()
        try await orchestrator.close()

        let baseName = url.deletingPathExtension().lastPathComponent
        let dir = url.deletingLastPathComponent()
        let contents = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        let related = contents.filter { $0.lastPathComponent.hasPrefix(baseName) }
        #expect(related.count == 1)

        let reopened = try await MemoryOrchestrator(at: url, config: config)
        let ctx = try await reopened.recall(query: "actors")
        #expect(!ctx.items.isEmpty)
        try await reopened.close()
    }
}

@Test
func compactIndexesStagesPendingTextIndex() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 10, overlapTokens: 2)
        config.rag = FastRAGConfig(
            maxContextTokens: 120,
            expansionMaxTokens: 40,
            snippetMaxTokens: 20,
            maxSnippets: 12,
            searchTopK: 25,
            searchMode: .textOnly
        )

        let sentence = "Swift concurrency uses actors and tasks. Actors isolate mutable state."
        let content = Array(repeating: sentence, count: 40).joined(separator: " ")

        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        try await orchestrator.remember(content)

        _ = try await orchestrator.compactIndexes()
        try await orchestrator.close()

        let reopened = try await MemoryOrchestrator(at: url, config: config)
        let ctx = try await reopened.recall(query: "actors")
        #expect(!ctx.items.isEmpty)
        try await reopened.close()

        let baseName = url.deletingPathExtension().lastPathComponent
        let dir = url.deletingLastPathComponent()
        let contents = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        let related = contents.filter { $0.lastPathComponent.hasPrefix(baseName) }
        #expect(related.count == 1)
    }
}

@Test
func compactIndexesPreservesDenseCachedSurrogateRecall() async throws {
    try await TempFiles.withTempFile { url in
        var config = OrchestratorConfig.default
        config.enableVectorSearch = false
        config.chunking = .tokenCount(targetTokens: 8, overlapTokens: 2)
        config.rag = FastRAGConfig(
            mode: .denseCached,
            maxContextTokens: 120,
            expansionMaxTokens: 20,
            snippetMaxTokens: 12,
            maxSnippets: 10,
            maxSurrogates: 4,
            surrogateMaxTokens: 18,
            searchTopK: 25,
            searchMode: .textOnly
        )

        let sentence = "Swift concurrency uses actors and tasks. Actors isolate mutable state."
        let content = Array(repeating: sentence, count: 50).joined(separator: " ")

        let orchestrator = try await MemoryOrchestrator(at: url, config: config)
        try await orchestrator.remember(content)
        try await orchestrator.flush()
        _ = try await orchestrator.optimizeSurrogates()
        _ = try await orchestrator.compactIndexes()
        try await orchestrator.close()

        let reopened = try await MemoryOrchestrator(at: url, config: config)
        let ctx = try await reopened.recall(query: "actors")
        #expect(ctx.items.contains { $0.kind == .surrogate })
        try await reopened.close()

        let baseName = url.deletingPathExtension().lastPathComponent
        let dir = url.deletingLastPathComponent()
        let contents = try FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)
        let related = contents.filter { $0.lastPathComponent.hasPrefix(baseName) }
        #expect(related.count == 1)
    }
}
