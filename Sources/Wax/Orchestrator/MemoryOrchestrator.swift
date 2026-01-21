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

        for (idx, chunk) in chunks.enumerated() {
            var options = FrameMetaSubset()
            options.role = .chunk
            options.parentId = docId
            options.chunkIndex = UInt32(idx)
            options.chunkCount = UInt32(chunks.count)
            options.searchText = chunk

            var meta = Metadata(metadata)
            if let session = currentSessionId {
                meta.entries["session_id"] = session.uuidString
            }
            options.metadata = meta

            let frameId: UInt64
            if let vec, let embedder {
                var vector = try await embedder.embed(chunk)
                if embedder.normalize {
                    vector = Self.normalizedL2(vector)
                }
                frameId = try await vec.putWithEmbedding(
                    Data(chunk.utf8),
                    embedding: vector,
                    options: options,
                    identity: embedder.identity
                )
            } else {
                frameId = try await wax.put(Data(chunk.utf8), options: options)
            }

            if let text {
                try await text.index(frameId: frameId, text: chunk)
            }
        }
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

    private static func normalizedL2(_ vector: [Float]) -> [Float] {
        guard !vector.isEmpty else { return vector }

        var sum: Double = 0
        for v in vector {
            let d = Double(v)
            sum += d * d
        }

        let norm = sqrt(sum)
        guard norm > 0 else { return vector }

        let inv = Float(1.0 / norm)
        return vector.map { $0 * inv }
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
}
