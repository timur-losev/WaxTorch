import Foundation
@preconcurrency import GRDB
import WaxCore

public actor FTS5SearchEngine {
    private static let maxResults = 10_000
    private let dbQueue: DatabaseQueue
    private let io: BlockingIOExecutor

    private init(dbQueue: DatabaseQueue, io: BlockingIOExecutor) {
        self.dbQueue = dbQueue
        self.io = io
    }

    public static func inMemory() throws -> FTS5SearchEngine {
        let io = BlockingIOExecutor(label: "com.wax.fts", qos: .userInitiated)
        let config = makeConfiguration()
        let queue = try DatabaseQueue(configuration: config)
        try queue.write { db in
            try FTS5Schema.create(in: db)
        }
        return FTS5SearchEngine(dbQueue: queue, io: io)
    }

    public static func deserialize(from data: Data) throws -> FTS5SearchEngine {
        let io = BlockingIOExecutor(label: "com.wax.fts", qos: .userInitiated)
        let config = makeConfiguration()
        let queue = try DatabaseQueue(configuration: config)
        try queue.writeWithoutTransaction { db in
            let connection = try requireConnection(db)
            try FTS5Serializer.deserialize(data, into: connection)
            try applyPragmas(db)
            try FTS5Schema.validateOrUpgrade(in: db)
        }
        return FTS5SearchEngine(dbQueue: queue, io: io)
    }

    public static func load(from wax: Wax) async throws -> FTS5SearchEngine {
        if let bytes = try await wax.readCommittedLexIndexBytes() {
            return try FTS5SearchEngine.deserialize(from: bytes)
        }
        return try FTS5SearchEngine.inMemory()
    }

    public func count() async throws -> Int {
        let dbQueue = self.dbQueue
        return try await io.run {
            try dbQueue.read { db in
                try Int.fetchOne(db, sql: "SELECT COUNT(*) FROM frame_mapping") ?? 0
            }
        }
    }

    public func index(frameId: UInt64, text: String) async throws {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            try await remove(frameId: frameId)
            return
        }
        let frameIdValue = try Self.toInt64(frameId)
        let dbQueue = self.dbQueue
        try await io.run {
            try dbQueue.write { db in
                if let rowid: Int64 = try Int64.fetchOne(
                    db,
                    sql: "SELECT rowid_ref FROM frame_mapping WHERE frame_id = ?",
                    arguments: [frameIdValue]
                ) {
                    try db.execute(sql: "DELETE FROM frames_fts WHERE rowid = ?", arguments: [rowid])
                    try db.execute(sql: "DELETE FROM frame_mapping WHERE frame_id = ?", arguments: [frameIdValue])
                }
                try db.execute(sql: "INSERT INTO frames_fts(content) VALUES (?)", arguments: [trimmed])
                let rowid = db.lastInsertedRowID
                try db.execute(
                    sql: "INSERT INTO frame_mapping(frame_id, rowid_ref) VALUES (?, ?)",
                    arguments: [frameIdValue, rowid]
                )
            }
        }
    }

    public func remove(frameId: UInt64) async throws {
        let frameIdValue = try Self.toInt64(frameId)
        let dbQueue = self.dbQueue
        try await io.run {
            try dbQueue.write { db in
                if let rowid: Int64 = try Int64.fetchOne(
                    db,
                    sql: "SELECT rowid_ref FROM frame_mapping WHERE frame_id = ?",
                    arguments: [frameIdValue]
                ) {
                    try db.execute(sql: "DELETE FROM frames_fts WHERE rowid = ?", arguments: [rowid])
                    try db.execute(sql: "DELETE FROM frame_mapping WHERE frame_id = ?", arguments: [frameIdValue])
                }
            }
        }
    }

    public func search(query: String, topK: Int) async throws -> [TextSearchResult] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return [] }
        let limit = Self.clampTopK(topK)
        let dbQueue = self.dbQueue
        return try await io.run {
            try dbQueue.read { db in
                let sql = """
                    SELECT m.frame_id AS frame_id,
                           bm25(frames_fts) AS rank,
                           snippet(frames_fts, 0, '[', ']', '...', 10) AS snippet
                    FROM frames_fts
                    JOIN frame_mapping m ON m.rowid_ref = frames_fts.rowid
                    WHERE frames_fts MATCH ?
                    ORDER BY rank ASC
                    LIMIT ?
                    """
                let rows = try Row.fetchAll(db, sql: sql, arguments: [trimmed, limit])
                return rows.compactMap { row in
                    guard let frameIdValue: Int64 = row["frame_id"], frameIdValue >= 0 else { return nil }
                    let rank: Double = row["rank"] ?? 0
                    let snippet: String? = row["snippet"]
                    return TextSearchResult(
                        frameId: UInt64(frameIdValue),
                        score: Self.scoreFromBM25Rank(rank),
                        snippet: snippet
                    )
                }
            }
        }
    }

    public func serialize(compact: Bool = false) async throws -> Data {
        let dbQueue = self.dbQueue
        return try await io.run {
            try dbQueue.writeWithoutTransaction { db in
                if compact {
                    try db.execute(sql: "VACUUM")
                }
                let connection = try Self.requireConnection(db)
                return try FTS5Serializer.serialize(connection: connection)
            }
        }
    }

    public func stageForCommit(into wax: Wax, compact: Bool = false) async throws {
        let blob = try await serialize(compact: compact)
        let count = try await count()
        guard count >= 0 else {
            throw WaxError.io("fts doc_count invalid: \(count)")
        }
        try await wax.stageLexIndexForNextCommit(bytes: blob, docCount: UInt64(count))
    }

    private static func makeConfiguration() -> Configuration {
        var config = Configuration()
        config.prepareDatabase { db in
            try applyPragmas(db)
        }
        return config
    }

    private static func applyPragmas(_ db: Database) throws {
        try db.execute(sql: "PRAGMA journal_mode=DELETE")
        try db.execute(sql: "PRAGMA temp_store=MEMORY")
    }

    private static func requireConnection(_ db: Database) throws -> OpaquePointer {
        guard let connection = db.sqliteConnection else {
            throw WaxError.io("sqlite connection unavailable")
        }
        return connection
    }

    private static func scoreFromBM25Rank(_ rank: Double) -> Double {
        // SQLite FTS5 bm25() rank is "lower is better" (often negative).
        // Expose a score where "higher is better".
        guard rank.isFinite else { return 0 }
        return -rank
    }

    private static func clampTopK(_ topK: Int) -> Int {
        if topK < 1 { return 1 }
        if topK > maxResults { return maxResults }
        return topK
    }

    private static func toInt64(_ value: UInt64) throws -> Int64 {
        guard value <= UInt64(Int64.max) else {
            throw WaxError.io("frameId exceeds sqlite int64 range: \(value)")
        }
        return Int64(value)
    }
}
