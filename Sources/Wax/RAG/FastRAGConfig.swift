import Foundation
import WaxCore
import WaxTextSearch

public struct FastRAGConfig: Sendable, Equatable {
    public enum Mode: Sendable, Equatable {
        case fast
        case denseCached
    }

    public var mode: Mode = .fast

    /// Total token budget for the returned context (snippets + expansion).
    public var maxContextTokens: Int = 1_500

    /// Token budget for the single “expanded” item.
    public var expansionMaxTokens: Int = 600

    /// Hard cap on expansion bytes before UTF-8 decode/tokenization.
    public var expansionMaxBytes: Int = 2 * 1024 * 1024

    /// Per-snippet token cap to avoid one snippet consuming the entire budget.
    public var snippetMaxTokens: Int = 200

    /// Max snippet items included (after expansion).
    public var maxSnippets: Int = 24

    /// Max surrogate items included (after expansion) when `mode == .denseCached`.
    public var maxSurrogates: Int = 8

    /// Per-surrogate token cap when `mode == .denseCached`.
    public var surrogateMaxTokens: Int = 60

    /// Search parameters used to collect candidates.
    public var searchTopK: Int = 24
    public var searchMode: SearchMode = .hybrid(alpha: 1.0)
    public var rrfK: Int = 60
    public var previewMaxBytes: Int = 512

    public init(
        mode: Mode = .fast,
        maxContextTokens: Int = 1_500,
        expansionMaxTokens: Int = 600,
        expansionMaxBytes: Int = 2 * 1024 * 1024,
        snippetMaxTokens: Int = 200,
        maxSnippets: Int = 24,
        maxSurrogates: Int = 8,
        surrogateMaxTokens: Int = 60,
        searchTopK: Int = 24,
        searchMode: SearchMode = .hybrid(alpha: 1.0),
        rrfK: Int = 60,
        previewMaxBytes: Int = 512
    ) {
        self.mode = mode
        self.maxContextTokens = maxContextTokens
        self.expansionMaxTokens = expansionMaxTokens
        self.expansionMaxBytes = expansionMaxBytes
        self.snippetMaxTokens = snippetMaxTokens
        self.maxSnippets = maxSnippets
        self.maxSurrogates = maxSurrogates
        self.surrogateMaxTokens = surrogateMaxTokens
        self.searchTopK = searchTopK
        self.searchMode = searchMode
        self.rrfK = rrfK
        self.previewMaxBytes = previewMaxBytes
    }
}
