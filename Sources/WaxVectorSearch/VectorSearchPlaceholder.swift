import USearch

@usableFromInline
enum _USearchDependencyProbe {
    static func touch() { _ = USearchIndex.self }
}

public enum VectorSearchPlaceholder {
    public static let isEnabled = true
}
