import Foundation

public enum ChunkingStrategy: Sendable, Equatable {
    case tokenCount(targetTokens: Int, overlapTokens: Int)
}
