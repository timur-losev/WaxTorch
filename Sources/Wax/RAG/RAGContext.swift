import Foundation
import WaxTextSearch

public struct RAGContext: Sendable, Equatable {
    public enum ItemKind: Sendable, Equatable { case snippet, expanded, surrogate }

    public struct Item: Sendable, Equatable {
        public var kind: ItemKind
        public var frameId: UInt64
        public var score: Float
        public var sources: [SearchResponse.Source]
        public var text: String

        public init(kind: ItemKind, frameId: UInt64, score: Float, sources: [SearchResponse.Source], text: String) {
            self.kind = kind
            self.frameId = frameId
            self.score = score
            self.sources = sources
            self.text = text
        }
    }

    public var query: String
    public var items: [Item]
    public var totalTokens: Int

    public init(query: String, items: [Item], totalTokens: Int) {
        self.query = query
        self.items = items
        self.totalTokens = totalTokens
    }
}
