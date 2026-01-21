#if canImport(WaxVectorSearchMiniLM)
import Foundation
import WaxVectorSearchMiniLM

public extension MemoryOrchestrator {
    static func openMiniLM(
        at url: URL,
        config: OrchestratorConfig = .default
    ) async throws -> MemoryOrchestrator {
        try await MemoryOrchestrator(at: url, config: config, embedder: MiniLMEmbedder())
    }
}
#endif

