import Foundation

public protocol SurrogateGenerator: Sendable {
    /// Stable identifier for persisted metadata (e.g. "extractive_v1", "smollm_360m_q4_v1")
    var algorithmID: String { get }

    /// Generate a surrogate for a source frame/chunk.
    /// Must be safe for offline caching and bounded by `maxTokens`.
    func generateSurrogate(sourceText: String, maxTokens: Int) async throws -> String
}

