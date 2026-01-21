import Foundation
import USearch
import WaxCore

public enum VectorSerializer {
    public struct SegmentInfo: Sendable, Equatable {
        public var similarity: VecSimilarity
        public var dimension: UInt32
        public var vectorCount: UInt64
        public var payloadLength: UInt64

        public init(similarity: VecSimilarity, dimension: UInt32, vectorCount: UInt64, payloadLength: UInt64) {
            self.similarity = similarity
            self.dimension = dimension
            self.vectorCount = vectorCount
            self.payloadLength = payloadLength
        }
    }

    public static func serializeUSearchIndex(
        _ index: USearchIndex,
        metric: VectorMetric,
        dimensions: Int,
        vectorCount: UInt64
    ) throws -> Data {
        let payload = try saveUSearchPayload(index)
        let header = VecSegmentHeaderV1(
            similarity: metric.toVecSimilarity(),
            dimension: UInt32(dimensions),
            vectorCount: vectorCount,
            payloadLength: UInt64(payload.count)
        )
        var encoder = BinaryEncoder()
        header.encode(to: &encoder)
        var data = encoder.data
        data.append(payload)
        return data
    }

    public static func decodeUSearchPayload(from data: Data) throws -> (info: SegmentInfo, payload: Data) {
        guard data.count >= VecSegmentHeaderV1.encodedSize else {
            throw WaxError.invalidToc(reason: "vec segment too small: \(data.count) bytes")
        }

        let headerBytes = data.prefix(VecSegmentHeaderV1.encodedSize)
        var decoder = try BinaryDecoder(data: Data(headerBytes))
        let header = try VecSegmentHeaderV1.decode(from: &decoder)
        try decoder.finalize()

        guard header.payloadLength <= UInt64(Int.max) else {
            throw WaxError.invalidToc(reason: "vec payload_length exceeds Int.max: \(header.payloadLength)")
        }
        let expectedTotal = VecSegmentHeaderV1.encodedSize + Int(header.payloadLength)
        guard data.count == expectedTotal else {
            throw WaxError.invalidToc(reason: "vec segment length mismatch: expected \(expectedTotal), got \(data.count)")
        }

        let payload = data.suffix(Int(header.payloadLength))
        let info = SegmentInfo(
            similarity: header.similarity,
            dimension: header.dimension,
            vectorCount: header.vectorCount,
            payloadLength: header.payloadLength
        )
        return (info, payload)
    }

    public static func loadUSearchIndex(_ index: USearchIndex, fromPayload payload: Data) throws {
        let url = tempURL(suffix: "usearch")
        defer { try? FileManager.default.removeItem(at: url) }
        try payload.write(to: url, options: [.atomic])
        try index.load(path: url.path)
    }

    // MARK: - Private

    private static func saveUSearchPayload(_ index: USearchIndex) throws -> Data {
        let url = tempURL(suffix: "usearch")
        defer { try? FileManager.default.removeItem(at: url) }
        try index.save(path: url.path)
        return try Data(contentsOf: url)
    }

    private static func tempURL(suffix: String) -> URL {
        let dir = FileManager.default.temporaryDirectory
        return dir.appendingPathComponent("wax-\(UUID().uuidString).\(suffix)")
    }

    private struct VecSegmentHeaderV1 {
        static let encodedSize: Int = 36
        static let magic = Data([0x4D, 0x56, 0x32, 0x56]) // "MV2V"

        var version: UInt16 = 1
        var encoding: UInt8 = 1 // 1 = usearch
        var similarity: VecSimilarity
        var dimension: UInt32
        var vectorCount: UInt64
        var payloadLength: UInt64

        init(similarity: VecSimilarity, dimension: UInt32, vectorCount: UInt64, payloadLength: UInt64) {
            self.similarity = similarity
            self.dimension = dimension
            self.vectorCount = vectorCount
            self.payloadLength = payloadLength
        }

        func encode(to encoder: inout BinaryEncoder) {
            encoder.encodeFixedBytes(Self.magic)
            encoder.encode(version)
            encoder.encode(encoding)
            encoder.encode(similarity.rawValue)
            encoder.encode(dimension)
            encoder.encode(vectorCount)
            encoder.encode(payloadLength)
            encoder.encodeFixedBytes(Data(repeating: 0, count: 8))
        }

        static func decode(from decoder: inout BinaryDecoder) throws -> VecSegmentHeaderV1 {
            let magic = try decoder.decodeFixedBytes(count: 4)
            guard magic == Self.magic else {
                throw WaxError.invalidToc(reason: "vec segment magic mismatch")
            }

            let version = try decoder.decode(UInt16.self)
            guard version == 1 else {
                throw WaxError.invalidToc(reason: "unsupported vec segment version \(version)")
            }

            let encoding = try decoder.decode(UInt8.self)
            guard encoding == 1 else {
                throw WaxError.invalidToc(reason: "unsupported vec segment encoding \(encoding)")
            }

            let similarityRaw = try decoder.decode(UInt8.self)
            guard let similarity = VecSimilarity(rawValue: similarityRaw) else {
                throw WaxError.invalidToc(reason: "vec similarity must be 0..2 (got \(similarityRaw))")
            }

            let dimension = try decoder.decode(UInt32.self)
            let vectorCount = try decoder.decode(UInt64.self)
            let payloadLength = try decoder.decode(UInt64.self)
            let reserved = try decoder.decodeFixedBytes(count: 8)
            guard reserved == Data(repeating: 0, count: 8) else {
                throw WaxError.invalidToc(reason: "vec segment reserved bytes must be zero")
            }

            var header = VecSegmentHeaderV1(
                similarity: similarity,
                dimension: dimension,
                vectorCount: vectorCount,
                payloadLength: payloadLength
            )
            header.version = version
            header.encoding = encoding
            return header
        }
    }
}

