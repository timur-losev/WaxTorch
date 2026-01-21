import Foundation
import WaxCore
import WaxVectorSearch

public actor WaxVectorSearchSession {
    public let wax: Wax
    public let engine: USearchVectorEngine
    public let dimensions: Int

    public init(wax: Wax, metric: VectorMetric = .cosine, dimensions: Int) async throws {
        self.wax = wax
        self.dimensions = dimensions
        self.engine = try await USearchVectorEngine.load(from: wax, metric: metric, dimensions: dimensions)

        let pending = await wax.pendingEmbeddingMutations()
        for embedding in pending {
            try await engine.add(frameId: embedding.frameId, vector: embedding.vector)
        }
    }

    public func add(frameId: UInt64, vector: [Float]) async throws {
        try await engine.add(frameId: frameId, vector: vector)
        try await wax.putEmbedding(frameId: frameId, vector: vector)
    }

    public func remove(frameId: UInt64) async throws {
        try await engine.remove(frameId: frameId)
    }

    public func search(vector: [Float], topK: Int) async throws -> [(frameId: UInt64, score: Float)] {
        try await engine.search(vector: vector, topK: topK)
    }

    public func putWithEmbedding(
        _ content: Data,
        embedding: [Float],
        options: FrameMetaSubset = .init(),
        compression: CanonicalEncoding = .plain,
        identity: EmbeddingIdentity? = nil
    ) async throws -> UInt64 {
        guard embedding.count == dimensions else {
            throw WaxError.encodingError(reason: "vector dimension mismatch: expected \(dimensions), got \(embedding.count)")
        }

        var merged = options
        if let identity {
            if let expectedDims = identity.dimensions, expectedDims != embedding.count {
                throw WaxError.io("embedding identity dimension mismatch: expected \(expectedDims), got \(embedding.count)")
            }
            var metadata = merged.metadata ?? Metadata()
            if let provider = identity.provider { metadata.entries["memvid.embedding.provider"] = provider }
            if let model = identity.model { metadata.entries["memvid.embedding.model"] = model }
            if let dims = identity.dimensions { metadata.entries["memvid.embedding.dimension"] = String(dims) }
            if let normalized = identity.normalized { metadata.entries["memvid.embedding.normalized"] = String(normalized) }
            merged.metadata = metadata
        }

        let frameId = try await wax.put(content, options: merged, compression: compression)
        try await engine.add(frameId: frameId, vector: embedding)
        try await wax.putEmbedding(frameId: frameId, vector: embedding)
        return frameId
    }

    public func commit() async throws {
        try await stageForCommit()
        try await wax.commit()
    }

    public func stageForCommit() async throws {
        let pending = await wax.pendingEmbeddingMutations()
        for embedding in pending {
            try await engine.add(frameId: embedding.frameId, vector: embedding.vector)
        }
        try await engine.stageForCommit(into: wax)
    }
}

public extension Wax {
    func enableVectorSearch(metric: VectorMetric = .cosine, dimensions: Int) async throws -> WaxVectorSearchSession {
        try await WaxVectorSearchSession(wax: self, metric: metric, dimensions: dimensions)
    }

    func enableVectorSearchFromManifest() async throws -> WaxVectorSearchSession {
        guard let manifest = await committedVecIndexManifest() else {
            throw WaxError.io("vec index manifest missing; enableVectorSearch(dimensions:) required")
        }
        guard let metric = VectorMetric(vecSimilarity: manifest.similarity) else {
            throw WaxError.invalidToc(reason: "unsupported vec similarity \(manifest.similarity)")
        }
        return try await WaxVectorSearchSession(
            wax: self,
            metric: metric,
            dimensions: Int(manifest.dimension)
        )
    }
}
