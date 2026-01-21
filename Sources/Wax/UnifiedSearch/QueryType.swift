/// Query type for adaptive retrieval strategies.
public enum QueryType: String, Sendable, CaseIterable {
    case factual
    case semantic
    case temporal
    case exploratory
}

