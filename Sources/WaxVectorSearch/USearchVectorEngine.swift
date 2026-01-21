import Foundation
@preconcurrency import USearch
import WaxCore

public actor USearchVectorEngine {
    private static let maxResults = 10_000
    private static let connectivity: UInt32 = 16
    private static let initialReserve: UInt32 = 64

    private let metric: VectorMetric
    public let dimensions: Int

    private var vectorCount: UInt64
    private var reservedCapacity: UInt32
    private let index: USearchIndex
    private let io: BlockingIOExecutor
    private let opLock = AsyncMutex()
    private var dirty: Bool

    private func withOpLock<T>(_ body: () async throws -> T) async rethrows -> T {
        await opLock.lock()
        do {
            let value = try await body()
            await opLock.unlock()
            return value
        } catch {
            await opLock.unlock()
            throw error
        }
    }

    public init(metric: VectorMetric, dimensions: Int) throws {
        guard dimensions > 0 else {
            throw WaxError.invalidToc(reason: "dimensions must be > 0")
        }
        guard dimensions <= Constants.maxEmbeddingDimensions else {
            throw WaxError.capacityExceeded(
                limit: UInt64(Constants.maxEmbeddingDimensions),
                requested: UInt64(dimensions)
            )
        }

        self.metric = metric
        self.dimensions = dimensions
        self.vectorCount = 0
        self.reservedCapacity = Self.initialReserve
        self.dirty = false
        self.index = try USearchIndex.make(
            metric: metric.toUSearchMetric(),
            dimensions: UInt32(dimensions),
            connectivity: Self.connectivity,
            quantization: .f32
        )
        self.io = BlockingIOExecutor(label: "com.wax.usearch", qos: .userInitiated)
        try index.reserve(reservedCapacity)
    }

    public static func load(from wax: Wax, metric: VectorMetric, dimensions: Int) async throws -> USearchVectorEngine {
        let engine = try USearchVectorEngine(metric: metric, dimensions: dimensions)
        if let bytes = try await wax.readCommittedVecIndexBytes() {
            try await engine.deserialize(bytes)
        }
        let pending = await wax.pendingEmbeddingMutations()
        for embedding in pending {
            try await engine.add(frameId: embedding.frameId, vector: embedding.vector)
        }
        return engine
    }

    public func add(frameId: UInt64, vector: [Float]) async throws {
        try await withOpLock {
            try validate(vector)
            let index = self.index
            let isEmpty = vectorCount == 0
            let removed: UInt32 = try await io.run {
                if isEmpty { return 0 }
                return try index.remove(key: frameId)
            }
            if removed == 0 {
                try await reserveIfNeeded(for: vectorCount &+ 1)
                vectorCount &+= 1
            }
            try await io.run {
                try index.add(key: frameId, vector: vector)
            }
            dirty = true
        }
    }

    /// Batch add multiple vectors in a single operation.
    /// This amortizes lock acquisition and I/O overhead across all vectors.
    public func addBatch(frameIds: [UInt64], vectors: [[Float]]) async throws {
        guard !frameIds.isEmpty else { return }
        guard frameIds.count == vectors.count else {
            throw WaxError.encodingError(reason: "addBatch: frameIds.count != vectors.count")
        }

        try await withOpLock {
            // Validate all vectors first
            for vector in vectors {
                try validate(vector)
            }

            let index = self.index
            let isEmpty = vectorCount == 0
            let frameIdArray = frameIds

            // Reserve capacity for all new vectors
            let maxNewCount = vectorCount &+ UInt64(frameIds.count)
            try await reserveIfNeeded(for: maxNewCount)

            // Single I/O block for all operations
            let addedCount = try await io.run { () throws -> Int in
                var added = 0
                for (frameId, vector) in zip(frameIdArray, vectors) {
                    // Try to remove existing (if not empty)
                    let removed: UInt32 = if isEmpty { 0 } else { try index.remove(key: frameId) }
                    if removed == 0 {
                        added += 1
                    }
                    try index.add(key: frameId, vector: vector)
                }
                return added
            }

            vectorCount &+= UInt64(addedCount)
            dirty = true
        }
    }

    public func remove(frameId: UInt64) async throws {
        try await withOpLock {
            guard vectorCount > 0 else { return }
            let index = self.index
            let removed = try await io.run { try index.remove(key: frameId) }
            if removed > 0 {
                vectorCount = vectorCount == 0 ? 0 : (vectorCount &- 1)
                dirty = true
            }
        }
    }

    public func search(vector: [Float], topK: Int) async throws -> [(frameId: UInt64, score: Float)] {
        try await withOpLock {
            guard vectorCount > 0 else { return [] }
            try validate(vector)
            let limit = Self.clampTopK(topK)

            let index = self.index
            let (keys, distances) = try await io.run { try index.search(vector: vector, count: limit) }
            var results: [(UInt64, Float)] = []
            results.reserveCapacity(min(keys.count, distances.count))
            for (key, distance) in zip(keys, distances) {
                results.append((key, metric.score(fromDistance: distance)))
            }
            return results
        }
    }

    public func serialize() async throws -> Data {
        try await withOpLock {
            let index = self.index
            let metric = self.metric
            let dimensions = self.dimensions
            let vectorCount = self.vectorCount
            return try await io.run {
                try VectorSerializer.serializeUSearchIndex(
                    index,
                    metric: metric,
                    dimensions: dimensions,
                    vectorCount: vectorCount
                )
            }
        }
    }

    public func deserialize(_ data: Data) async throws {
        try await withOpLock {
            let decoded = try VectorSerializer.decodeUSearchPayload(from: data)
            guard decoded.info.dimension == UInt32(dimensions) else {
                throw WaxError.invalidToc(reason: "vec dimension mismatch: expected \(dimensions), got \(decoded.info.dimension)")
            }
            guard decoded.info.similarity == metric.toVecSimilarity() else {
                throw WaxError.invalidToc(reason: "vec similarity mismatch: expected \(metric.toVecSimilarity()), got \(decoded.info.similarity)")
            }

            let index = self.index
            let payload = decoded.payload
            try await io.run { try VectorSerializer.loadUSearchIndex(index, fromPayload: payload) }
            vectorCount = decoded.info.vectorCount
            reservedCapacity = max(reservedCapacity, UInt32(min(vectorCount, UInt64(UInt32.max))))
            let reserve = reservedCapacity
            try await io.run { try index.reserve(reserve) }
            dirty = false
        }
    }

    public func stageForCommit(into wax: Wax) async throws {
        if !dirty { return }
        let blob = try await serialize()
        try await wax.stageVecIndexForNextCommit(
            bytes: blob,
            vectorCount: vectorCount,
            dimension: UInt32(dimensions),
            similarity: metric.toVecSimilarity()
        )
        dirty = false
    }

    private func validate(_ vector: [Float]) throws {
        guard vector.count == dimensions else {
            throw WaxError.encodingError(reason: "vector dimension mismatch: expected \(dimensions), got \(vector.count)")
        }
        guard vector.count <= Constants.maxEmbeddingDimensions else {
            throw WaxError.capacityExceeded(
                limit: UInt64(Constants.maxEmbeddingDimensions),
                requested: UInt64(vector.count)
            )
        }
    }

    private static func clampTopK(_ topK: Int) -> Int {
        if topK < 1 { return 1 }
        if topK > maxResults { return maxResults }
        return topK
    }

    private func reserveIfNeeded(for requiredCount: UInt64) async throws {
        guard requiredCount <= UInt64(UInt32.max) else {
            throw WaxError.capacityExceeded(limit: UInt64(UInt32.max), requested: requiredCount)
        }
        if requiredCount <= UInt64(reservedCapacity) { return }
        var next = reservedCapacity == 0 ? Self.initialReserve : reservedCapacity
        while requiredCount > UInt64(next) {
            let doubled = next &* 2
            next = doubled > next ? doubled : UInt32.max
            if next == UInt32.max { break }
        }
        reservedCapacity = max(reservedCapacity, next)
        let index = self.index
        let reserve = reservedCapacity
        try await io.run { try index.reserve(reserve) }
    }
}
