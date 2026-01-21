import Foundation

public struct LexIndexManifest: Equatable, Sendable {
    public var docCount: UInt64
    public var bytesOffset: UInt64
    public var bytesLength: UInt64
    public var checksum: Data
    public var version: UInt32

    public init(
        docCount: UInt64,
        bytesOffset: UInt64,
        bytesLength: UInt64,
        checksum: Data,
        version: UInt32
    ) {
        self.docCount = docCount
        self.bytesOffset = bytesOffset
        self.bytesLength = bytesLength
        self.checksum = checksum
        self.version = version
    }
}

extension LexIndexManifest: BinaryCodable {
    public mutating func encode(to encoder: inout BinaryEncoder) throws {
        encoder.encode(docCount)
        encoder.encode(bytesOffset)
        encoder.encode(bytesLength)
        guard checksum.count == 32 else {
            throw WaxError.encodingError(reason: "lex checksum must be 32 bytes (got \(checksum.count))")
        }
        encoder.encodeFixedBytes(checksum)
        encoder.encode(version)
    }

    public static func decode(from decoder: inout BinaryDecoder) throws -> LexIndexManifest {
        let docCount = try decoder.decode(UInt64.self)
        let bytesOffset = try decoder.decode(UInt64.self)
        let bytesLength = try decoder.decode(UInt64.self)
        let checksum = try decoder.decodeFixedBytes(count: 32)
        let version = try decoder.decode(UInt32.self)
        return LexIndexManifest(
            docCount: docCount,
            bytesOffset: bytesOffset,
            bytesLength: bytesLength,
            checksum: checksum,
            version: version
        )
    }
}

public struct VecIndexManifest: Equatable, Sendable {
    public var vectorCount: UInt64
    public var dimension: UInt32
    public var bytesOffset: UInt64
    public var bytesLength: UInt64
    public var checksum: Data
    public var similarity: VecSimilarity

    public init(
        vectorCount: UInt64,
        dimension: UInt32,
        bytesOffset: UInt64,
        bytesLength: UInt64,
        checksum: Data,
        similarity: VecSimilarity
    ) {
        self.vectorCount = vectorCount
        self.dimension = dimension
        self.bytesOffset = bytesOffset
        self.bytesLength = bytesLength
        self.checksum = checksum
        self.similarity = similarity
    }
}

extension VecIndexManifest: BinaryCodable {
    public mutating func encode(to encoder: inout BinaryEncoder) throws {
        encoder.encode(vectorCount)
        encoder.encode(dimension)
        encoder.encode(bytesOffset)
        encoder.encode(bytesLength)
        guard checksum.count == 32 else {
            throw WaxError.encodingError(reason: "vec checksum must be 32 bytes (got \(checksum.count))")
        }
        encoder.encodeFixedBytes(checksum)
        encoder.encode(similarity.rawValue)
    }

    public static func decode(from decoder: inout BinaryDecoder) throws -> VecIndexManifest {
        let vectorCount = try decoder.decode(UInt64.self)
        let dimension = try decoder.decode(UInt32.self)
        let bytesOffset = try decoder.decode(UInt64.self)
        let bytesLength = try decoder.decode(UInt64.self)
        let checksum = try decoder.decodeFixedBytes(count: 32)
        let similarityRaw = try decoder.decode(UInt8.self)
        guard let similarity = VecSimilarity(rawValue: similarityRaw) else {
            throw WaxError.invalidToc(reason: "vec similarity must be 0..2 (got \(similarityRaw))")
        }
        return VecIndexManifest(
            vectorCount: vectorCount,
            dimension: dimension,
            bytesOffset: bytesOffset,
            bytesLength: bytesLength,
            checksum: checksum,
            similarity: similarity
        )
    }
}

public struct IndexManifests: Equatable, Sendable {
    public var lex: LexIndexManifest?
    public var vec: VecIndexManifest?

    public init(lex: LexIndexManifest? = nil, vec: VecIndexManifest? = nil) {
        self.lex = lex
        self.vec = vec
    }
}

extension IndexManifests: BinaryCodable {
    public mutating func encode(to encoder: inout BinaryEncoder) throws {
        try encoder.encode(lex) { encoder, value in
            var mutable = value
            try mutable.encode(to: &encoder)
        }
        try encoder.encode(vec) { encoder, value in
            var mutable = value
            try mutable.encode(to: &encoder)
        }
        encoder.encode(UInt8(0)) // clip manifest absent in v1
    }

    public static func decode(from decoder: inout BinaryDecoder) throws -> IndexManifests {
        let lex = try decodeOptional(LexIndexManifest.self, from: &decoder)
        let vec = try decodeOptional(VecIndexManifest.self, from: &decoder)
        let clipTag = try decoder.decode(UInt8.self)
        guard clipTag == 0 else {
            throw WaxError.invalidToc(reason: "clip manifest not supported in v1")
        }
        return IndexManifests(lex: lex, vec: vec)
    }
}

private func decodeOptional<T: BinaryDecodable>(_ type: T.Type, from decoder: inout BinaryDecoder) throws -> T? {
    let tag = try decoder.decode(UInt8.self)
    switch tag {
    case 0:
        return nil
    case 1:
        return try T.decode(from: &decoder)
    default:
        throw WaxError.decodingError(reason: "invalid optional tag \(tag)")
    }
}
