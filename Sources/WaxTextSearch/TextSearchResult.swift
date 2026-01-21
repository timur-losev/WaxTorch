public struct TextSearchResult: Equatable, Sendable {
    public let frameId: UInt64
    public let score: Double
    public let snippet: String?

    public init(frameId: UInt64, score: Double, snippet: String?) {
        self.frameId = frameId
        self.score = score
        self.snippet = snippet
    }
}
