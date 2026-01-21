/// Fusion weights for hybrid search.
public struct FusionWeights: Sendable, Equatable {
    public var bm25: Float
    public var vector: Float
    public var temporal: Float

    public init(bm25: Float, vector: Float, temporal: Float = 0) {
        self.bm25 = bm25
        self.vector = vector
        self.temporal = temporal
    }
}

/// Query-adaptive fusion configuration.
public struct AdaptiveFusionConfig: Sendable {
    private var weightsByType: [QueryType: FusionWeights]

    public static let `default` = AdaptiveFusionConfig()

    public init() {
        self.weightsByType = [
            .factual: FusionWeights(bm25: 0.7, vector: 0.3, temporal: 0.0),
            .semantic: FusionWeights(bm25: 0.3, vector: 0.7, temporal: 0.0),
            .temporal: FusionWeights(bm25: 0.25, vector: 0.25, temporal: 0.5),
            .exploratory: FusionWeights(bm25: 0.4, vector: 0.5, temporal: 0.1),
        ]
    }

    public init(weights: [QueryType: FusionWeights]) {
        self.weightsByType = weights
    }

    public func weights(for queryType: QueryType) -> FusionWeights {
        weightsByType[queryType] ?? FusionWeights(bm25: 0.5, vector: 0.5)
    }
}
