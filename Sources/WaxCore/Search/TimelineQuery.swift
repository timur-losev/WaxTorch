import Foundation

public struct TimelineQuery: Sendable, Equatable {
    public enum Order: Sendable, Equatable {
        case chronological
        case reverseChronological
    }

    public var limit: Int
    public var order: Order
    public var after: Int64?
    public var before: Int64?
    public var includeDeleted: Bool
    public var includeSuperseded: Bool

    public init(
        limit: Int,
        order: Order = .reverseChronological,
        after: Int64? = nil,
        before: Int64? = nil,
        includeDeleted: Bool = false,
        includeSuperseded: Bool = false
    ) {
        self.limit = limit
        self.order = order
        self.after = after
        self.before = before
        self.includeDeleted = includeDeleted
        self.includeSuperseded = includeSuperseded
    }

    public func contains(_ timestamp: Int64) -> Bool {
        if let after, timestamp < after { return false }
        if let before, timestamp >= before { return false }
        return true
    }

    public static func filter(frames: [FrameMeta], query: TimelineQuery) -> [FrameMeta] {
        let filtered = frames
            .filter { query.contains($0.timestamp) }
            .filter { query.includeDeleted || $0.status != .deleted }
            .filter { query.includeSuperseded || $0.supersededBy == nil }

        let ordered: [FrameMeta]
        switch query.order {
        case .chronological:
            ordered = filtered.sorted { $0.timestamp < $1.timestamp }
        case .reverseChronological:
            ordered = filtered.sorted { $0.timestamp > $1.timestamp }
        }
        if query.limit <= 0 { return [] }
        return Array(ordered.prefix(query.limit))
    }
}
