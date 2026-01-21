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

