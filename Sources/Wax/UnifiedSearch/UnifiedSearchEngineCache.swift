import Foundation
import WaxCore
import WaxTextSearch
import WaxVectorSearch

actor UnifiedSearchEngineCache {
    static let shared = UnifiedSearchEngineCache()

    enum TextSourceKey: Hashable, Sendable {
        case empty
        case committed(checksum: Data)
        case staged(stamp: UInt64)
    }

    enum VectorSourceKey: Hashable, Sendable {
        case none
        case pendingOnly(dimensions: Int)
        case committed(checksum: Data, similarity: VecSimilarity, dimensions: Int)
        case staged(stamp: UInt64, similarity: VecSimilarity, dimensions: Int)
    }

    struct Stats: Sendable, Equatable {
        var textDeserializations: Int = 0
        var vectorDeserializations: Int = 0
    }

    private struct CachedText {
        var key: TextSourceKey
        var engine: FTS5SearchEngine
    }

    private struct CachedVector {
        var key: VectorSourceKey
        var engine: USearchVectorEngine
        var lastPendingEmbeddingSequence: UInt64?
    }

    private var textByWax: [ObjectIdentifier: CachedText] = [:]
    private var vectorByWax: [ObjectIdentifier: CachedVector] = [:]
    private var stats = Stats()

    func snapshotStats() -> Stats { stats }

    func resetStats() {
        stats = Stats()
    }

    func textEngine(for wax: Wax) async throws -> FTS5SearchEngine {
        let waxId = ObjectIdentifier(wax)

        if let stamp = await wax.stagedLexIndexStamp(),
           let _ = await wax.readStagedLexIndexBytes() {
            let key: TextSourceKey = .staged(stamp: stamp)
            if let cached = textByWax[waxId], cached.key == key {
                return cached.engine
            }
            guard let bytes = await wax.readStagedLexIndexBytes() else {
                let engine = try FTS5SearchEngine.inMemory()
                textByWax[waxId] = CachedText(key: .empty, engine: engine)
                return engine
            }
            let engine = try FTS5SearchEngine.deserialize(from: bytes)
            stats.textDeserializations += 1
            textByWax[waxId] = CachedText(key: key, engine: engine)
            return engine
        }

        if let manifest = await wax.committedLexIndexManifest() {
            let key: TextSourceKey = .committed(checksum: manifest.checksum)
            if let cached = textByWax[waxId], cached.key == key {
                return cached.engine
            }
            if let bytes = try await wax.readCommittedLexIndexBytes() {
                let engine = try FTS5SearchEngine.deserialize(from: bytes)
                stats.textDeserializations += 1
                textByWax[waxId] = CachedText(key: key, engine: engine)
                return engine
            }
        }

        if let cached = textByWax[waxId], cached.key == .empty {
            return cached.engine
        }
        let engine = try FTS5SearchEngine.inMemory()
        textByWax[waxId] = CachedText(key: .empty, engine: engine)
        return engine
    }

    func vectorEngine(
        for wax: Wax,
        queryEmbeddingDimensions: Int
    ) async throws -> USearchVectorEngine? {
        guard queryEmbeddingDimensions > 0 else { return nil }

        let waxId = ObjectIdentifier(wax)

        if let manifest = await wax.committedVecIndexManifest(),
           let metric = VectorMetric(vecSimilarity: manifest.similarity) {
            let key: VectorSourceKey = .committed(
                checksum: manifest.checksum,
                similarity: manifest.similarity,
                dimensions: Int(manifest.dimension)
            )
            if let cached = vectorByWax[waxId], cached.key == key {
                try await applyPendingEmbeddingsIfNeeded(wax: wax, waxId: waxId, cached: cached)
                return vectorByWax[waxId]?.engine
            }
            let engine = try USearchVectorEngine(metric: metric, dimensions: Int(manifest.dimension))
            if let bytes = try await wax.readCommittedVecIndexBytes() {
                try await engine.deserialize(bytes)
            }
            stats.vectorDeserializations += 1
            let cached = CachedVector(
                key: key,
                engine: engine,
                lastPendingEmbeddingSequence: nil
            )
            vectorByWax[waxId] = cached
            try await applyPendingEmbeddingsIfNeeded(wax: wax, waxId: waxId, cached: cached)
            return engine
        }

        if let stamp = await wax.stagedVecIndexStamp(),
           let staged = await wax.readStagedVecIndexBytes(),
           let metric = VectorMetric(vecSimilarity: staged.similarity) {
            let key: VectorSourceKey = .staged(
                stamp: stamp,
                similarity: staged.similarity,
                dimensions: Int(staged.dimension)
            )
            if let cached = vectorByWax[waxId], cached.key == key {
                try await applyPendingEmbeddingsIfNeeded(wax: wax, waxId: waxId, cached: cached)
                return vectorByWax[waxId]?.engine
            }

            let engine = try USearchVectorEngine(metric: metric, dimensions: Int(staged.dimension))
            try await engine.deserialize(staged.bytes)
            stats.vectorDeserializations += 1
            let pendingSnapshot = await wax.pendingEmbeddingMutations(since: nil)
            let cached = CachedVector(
                key: key,
                engine: engine,
                lastPendingEmbeddingSequence: pendingSnapshot.latestSequence
            )
            vectorByWax[waxId] = cached
            return engine
        }

        let pendingSnapshot = await wax.pendingEmbeddingMutations(since: nil)
        if !pendingSnapshot.embeddings.isEmpty,
           pendingSnapshot.embeddings.first?.dimension == UInt32(queryEmbeddingDimensions) {
            let key: VectorSourceKey = .pendingOnly(dimensions: queryEmbeddingDimensions)
            if let cached = vectorByWax[waxId], cached.key == key {
                try await applyPendingEmbeddingsIfNeeded(
                    wax: wax,
                    waxId: waxId,
                    cached: cached,
                    pendingSnapshot: pendingSnapshot
                )
                return vectorByWax[waxId]?.engine
            }

            let engine = try USearchVectorEngine(metric: .cosine, dimensions: queryEmbeddingDimensions)
            let cached = CachedVector(key: key, engine: engine, lastPendingEmbeddingSequence: nil)
            vectorByWax[waxId] = cached
            try await applyPendingEmbeddingsIfNeeded(
                wax: wax,
                waxId: waxId,
                cached: cached,
                pendingSnapshot: pendingSnapshot
            )
            return engine
        }

        return nil
    }

    private func applyPendingEmbeddingsIfNeeded(
        wax: Wax,
        waxId: ObjectIdentifier,
        cached: CachedVector,
        pendingSnapshot: PendingEmbeddingSnapshot? = nil
    ) async throws {
        guard var current = vectorByWax[waxId], current.key == cached.key else { return }

        let snapshot: PendingEmbeddingSnapshot
        if let provided = pendingSnapshot {
            snapshot = provided
        } else {
            snapshot = await wax.pendingEmbeddingMutations(
                since: current.lastPendingEmbeddingSequence
            )
        }

        if let latest = snapshot.latestSequence,
           let last = current.lastPendingEmbeddingSequence,
           latest < last {
            current.lastPendingEmbeddingSequence = nil
        }

        if !snapshot.embeddings.isEmpty {
            for embedding in snapshot.embeddings {
                try await current.engine.add(frameId: embedding.frameId, vector: embedding.vector)
            }
        }

        current.lastPendingEmbeddingSequence = snapshot.latestSequence
        vectorByWax[waxId] = current
    }
}
