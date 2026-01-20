import GRDB

@usableFromInline
enum _GRDBDependencyProbe {
    static func touch() { _ = DatabaseQueue.self }
}

public enum TextSearchPlaceholder {
    public static let isEnabled = true
}
