import Foundation
import WaxCore
import WaxTextSearch
import WaxVectorSearch

public actor MemoryOrchestrator {
    public enum QueryEmbeddingPolicy: Sendable, Equatable {
        case never
        case ifAvailable
        case always
    }

    let wax: Wax
    private let config: OrchestratorConfig
    private let ragBuilder: FastRAGContextBuilder

    let text: WaxTextSearchSession?
    let vec: WaxVectorSearchSession?
    private let embedder: (any EmbeddingProvider)?
    private let embeddingCache: EmbeddingMemoizer?

    private var currentSessionId: UUID?

    public init(
        at url: URL,
        config: OrchestratorConfig = .default,
        embedder: (any EmbeddingProvider)? = nil
    ) async throws {
        if FileManager.default.fileExists(atPath: url.path) {
            self.wax = try await Wax.open(at: url)
        } else {
            self.wax = try await Wax.create(at: url)
        }

        self.config = config
        self.ragBuilder = FastRAGContextBuilder()
        self.embedder = embedder
        if embedder != nil, config.embeddingCacheCapacity > 0 {
            self.embeddingCache = EmbeddingMemoizer(capacity: config.embeddingCacheCapacity)
        } else {
            self.embeddingCache = nil
        }

        if config.enableTextSearch {
            self.text = try await wax.enableTextSearch()
        } else {
            self.text = nil
        }

        if config.enableVectorSearch {
            if let embedder {
                self.vec = try await wax.enableVectorSearch(dimensions: embedder.dimensions)
            } else if await wax.committedVecIndexManifest() != nil {
                self.vec = try await wax.enableVectorSearchFromManifest()
            } else {
                throw WaxError.io("enableVectorSearch=true requires an EmbeddingProvider for ingest-time embeddings")
            }
        } else {
            self.vec = nil
        }
    }

    // MARK: - Session tagging (v1)

    public func startSession() -> UUID {
        let id = UUID()
        currentSessionId = id
        return id
    }

    public func endSession() {
        currentSessionId = nil
    }

    // MARK: - Ingestion

    /// Default batch size for chunk ingestion. Balances memory usage with I/O amortization.
    private static let defaultIngestBatchSize = 32

    public func remember(_ content: String, metadata: [String: String] = [:]) async throws {
        let chunks = await TextChunker.chunk(text: content, strategy: config.chunking)

        var docMeta = Metadata(metadata)
        if let session = currentSessionId {
            docMeta.entries["session_id"] = session.uuidString
        }

        let docId = try await wax.put(
            Data(content.utf8),
            options: FrameMetaSubset(
                role: .document,
                metadata: docMeta
            )
        )

        guard !chunks.isEmpty else { return }

        let chunkCount = chunks.count
        let localWax = wax
        let localText = text
        let localVec = vec
        let localEmbedder = embedder
        let cache = embeddingCache
        let sessionId = currentSessionId
        let batchSize = Self.defaultIngestBatchSize

        // Process chunks in batches to amortize actor/IO overhead
        for batchStart in stride(from: 0, to: chunkCount, by: batchSize) {
            let batchEnd = min(batchStart + batchSize, chunkCount)
            let batchChunks = Array(chunks[batchStart..<batchEnd])
            let batchIndices = batchStart..<batchEnd

            // Prepare options for all chunks in batch
            var batchOptions: [FrameMetaSubset] = []
            batchOptions.reserveCapacity(batchChunks.count)

            for (localIdx, globalIdx) in batchIndices.enumerated() {
                var options = FrameMetaSubset()
                options.role = .chunk
                options.parentId = docId
                options.chunkIndex = UInt32(globalIdx)
                options.chunkCount = UInt32(chunkCount)
                options.searchText = batchChunks[localIdx]

                var meta = Metadata(metadata)
                if let sessionId {
                    meta.entries["session_id"] = sessionId.uuidString
                }
                options.metadata = meta
                batchOptions.append(options)
            }

            let batchContents = batchChunks.map { Data($0.utf8) }

            // Path: With vector search enabled
            if let localVec, let localEmbedder {
                // Batch compute embeddings
                let embeddings = try await Self.prepareEmbeddingsBatch(
                    chunks: batchChunks,
                    embedder: localEmbedder,
                    cache: cache
                )

                // Batch put with embeddings
                let frameIds = try await localVec.putWithEmbeddingBatch(
                    contents: batchContents,
                    embeddings: embeddings,
                    options: batchOptions,
                    identity: localEmbedder.identity
                )

                // Batch text index
                if let localText {
                    try await localText.indexBatch(frameIds: frameIds, texts: batchChunks)
                }
            } else {
                // Path: Text-only (no vector search)
                let frameIds = try await localWax.putBatch(batchContents, options: batchOptions)

                // Batch text index
                if let localText {
                    try await localText.indexBatch(frameIds: frameIds, texts: batchChunks)
                }
            }
        }
    }

    /// Prepare embeddings for a batch of chunks, using cache where available.
    private static func prepareEmbeddingsBatch(
        chunks: [String],
        embedder: any EmbeddingProvider,
        cache: EmbeddingMemoizer?
    ) async throws -> [[Float]] {
        var results: [[Float]] = Array(repeating: [], count: chunks.count)
        var missingIndices: [Int] = []
        var missingTexts: [String] = []

        // Check cache first
        for (index, chunk) in chunks.enumerated() {
            let key = EmbeddingKey.make(
                text: chunk,
                identity: embedder.identity,
                dimensions: embedder.dimensions,
                normalized: embedder.normalize
            )
            if let cached = await cache?.get(key) {
                results[index] = cached
            } else {
                missingIndices.append(index)
                missingTexts.append(chunk)
            }
        }

        // Compute missing embeddings
        if !missingTexts.isEmpty {
            let vectors: [[Float]]
            if let batchEmbedder = embedder as? any BatchEmbeddingProvider {
                vectors = try await batchEmbedder.embed(batch: missingTexts)
            } else {
                // Fallback to parallel individual embeds
                vectors = try await withThrowingTaskGroup(of: (Int, [Float]).self) { group in
                    for (localIdx, text) in missingTexts.enumerated() {
                        group.addTask {
                            var vec = try await embedder.embed(text)
                            if embedder.normalize {
                                vec = normalizedL2(vec)
                            }
                            return (localIdx, vec)
                        }
                    }
                    var out = [[Float]](repeating: [], count: missingTexts.count)
                    for try await (idx, vec) in group {
                        out[idx] = vec
                    }
                    return out
                }
            }

            // Normalize and cache
            for (localIdx, globalIdx) in missingIndices.enumerated() {
                var vec = vectors[localIdx]
                if embedder.normalize {
                    vec = normalizedL2(vec)
                }
                results[globalIdx] = vec

                // Cache the result
                let key = EmbeddingKey.make(
                    text: chunks[globalIdx],
                    identity: embedder.identity,
                    dimensions: embedder.dimensions,
                    normalized: embedder.normalize
                )
                await cache?.set(key, value: vec)
            }
        }

        return results
    }

    // MARK: - Recall (Fast RAG)

    public func recall(query: String) async throws -> RAGContext {
        try await stageTextForRecall()

        var embedding: [Float]?
        if self.vec != nil, let embedder {
            var vector = try await embedder.embed(query)
            if embedder.normalize {
                vector = Self.normalizedL2(vector)
            }
            embedding = vector
        }
        return try await ragBuilder.build(
            query: query,
            embedding: embedding,
            wax: wax,
            config: config.rag
        )
    }

    public func recall(query: String, embedding: [Float]) async throws -> RAGContext {
        try await stageTextForRecall()
        return try await ragBuilder.build(query: query, embedding: embedding, wax: wax, config: config.rag)
    }

    public func recall(query: String, embeddingPolicy: QueryEmbeddingPolicy) async throws -> RAGContext {
        try await stageTextForRecall()

        let embedding = try await queryEmbedding(for: query, policy: embeddingPolicy)
        if let embedding {
            return try await ragBuilder.build(query: query, embedding: embedding, wax: wax, config: config.rag)
        }
        return try await ragBuilder.build(query: query, wax: wax, config: config.rag)
    }

    // MARK: - Persistence lifecycle

    public func flush() async throws {
        if let text {
            try await text.stageForCommit()
        }

        if let vec {
            let hasPendingEmbeddings = !(await wax.pendingEmbeddingMutations()).isEmpty
            let hasCommittedIndex = (await wax.committedVecIndexManifest()) != nil
            if hasPendingEmbeddings || hasCommittedIndex {
                try await vec.stageForCommit()
            }
        }

        try await wax.commit()
    }

    public func close() async throws {
        try await flush()
        try await wax.close()
    }

    // MARK: - Math helpers

    /// L2 normalization using Accelerate framework for optimal SIMD performance.
    @inline(__always)
    private static func normalizedL2(_ vector: [Float]) -> [Float] {
        VectorMath.normalizeL2(vector)
    }

    private func stageTextForRecall() async throws {
        if let text {
            try await text.stageForCommit()
        }
    }

    private func queryEmbedding(for query: String, policy: QueryEmbeddingPolicy) async throws -> [Float]? {
        switch policy {
        case .never:
            return nil
        case .ifAvailable:
            guard config.enableVectorSearch, let embedder else { return nil }
            var vector = try await embedder.embed(query)
            if embedder.normalize {
                vector = Self.normalizedL2(vector)
            }
            return vector
        case .always:
            guard config.enableVectorSearch else {
                throw WaxError.io("query embedding requested but vector search is disabled")
            }
            guard let embedder else {
                throw WaxError.io("query embedding requested but no EmbeddingProvider configured")
            }
            var vector = try await embedder.embed(query)
            if embedder.normalize {
                vector = Self.normalizedL2(vector)
            }
            return vector
        }
    }

    private static func embedOne(
        _ text: String,
        embedder: any EmbeddingProvider,
        cache: EmbeddingMemoizer?
    ) async throws -> [Float] {
        let key = EmbeddingKey.make(
            text: text,
            identity: embedder.identity,
            dimensions: embedder.dimensions,
            normalized: embedder.normalize
        )
        if let cached = await cache?.get(key) {
            return cached
        }

        var vector = try await embedder.embed(text)
        if embedder.normalize {
            vector = normalizedL2(vector)
        }
        await cache?.set(key, value: vector)
        return vector
    }

    private static func prepareEmbeddings(
        chunks: [String],
        embedder: any EmbeddingProvider,
        cache: EmbeddingMemoizer?
    ) async throws -> [Int: [Float]] {
        var out: [Int: [Float]] = [:]
        out.reserveCapacity(chunks.count)

        var missingTexts: [String] = []
        var missingIndices: [Int] = []
        missingTexts.reserveCapacity(chunks.count)
        missingIndices.reserveCapacity(chunks.count)

        for (idx, chunk) in chunks.enumerated() {
            let key = EmbeddingKey.make(
                text: chunk,
                identity: embedder.identity,
                dimensions: embedder.dimensions,
                normalized: embedder.normalize
            )
            if let cached = await cache?.get(key) {
                out[idx] = cached
            } else {
                missingTexts.append(chunk)
                missingIndices.append(idx)
            }
        }

        if missingTexts.isEmpty {
            return out
        }

        if let batch = embedder as? any BatchEmbeddingProvider {
            let vectors = try await batch.embed(batch: missingTexts)
            guard vectors.count == missingTexts.count else {
                throw WaxError.io("batch embedding count mismatch: expected \(missingTexts.count), got \(vectors.count)")
            }
            for (position, idx) in missingIndices.enumerated() {
                var vector = vectors[position]
                if embedder.normalize {
                    vector = normalizedL2(vector)
                }
                out[idx] = vector
                let key = EmbeddingKey.make(
                    text: chunks[idx],
                    identity: embedder.identity,
                    dimensions: embedder.dimensions,
                    normalized: embedder.normalize
                )
                await cache?.set(key, value: vector)
            }
        } else {
            for (position, idx) in missingIndices.enumerated() {
                let chunk = missingTexts[position]
                let vector = try await embedOne(chunk, embedder: embedder, cache: cache)
                out[idx] = vector
            }
        }

        return out
    }
}
