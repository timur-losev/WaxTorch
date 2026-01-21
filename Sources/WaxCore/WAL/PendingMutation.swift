import Foundation

public struct PutFrame: Equatable, Sendable {
    public var frameId: UInt64
    public var timestampMs: Int64
    public var options: FrameMetaSubset
    public var payloadOffset: UInt64
    public var payloadLength: UInt64
    public var canonicalEncoding: CanonicalEncoding
    public var canonicalLength: UInt64
    public var canonicalChecksum: Data
    public var storedChecksum: Data

    public init(
        frameId: UInt64,
        timestampMs: Int64,
        options: FrameMetaSubset,
        payloadOffset: UInt64,
        payloadLength: UInt64,
        canonicalEncoding: CanonicalEncoding,
        canonicalLength: UInt64,
        canonicalChecksum: Data,
        storedChecksum: Data
    ) {
        self.frameId = frameId
        self.timestampMs = timestampMs
        self.options = options
        self.payloadOffset = payloadOffset
        self.payloadLength = payloadLength
        self.canonicalEncoding = canonicalEncoding
        self.canonicalLength = canonicalLength
        self.canonicalChecksum = canonicalChecksum
        self.storedChecksum = storedChecksum
    }
}

public struct DeleteFrame: Equatable, Sendable {
    public var frameId: UInt64

    public init(frameId: UInt64) {
        self.frameId = frameId
    }
}

public struct SupersedeFrame: Equatable, Sendable {
    /// The older frame being superseded.
    public var supersededId: UInt64
    /// The newer frame that supersedes the old one.
    public var supersedingId: UInt64

    public init(supersededId: UInt64, supersedingId: UInt64) {
        self.supersededId = supersededId
        self.supersedingId = supersedingId
    }
}

public struct PutEmbedding: Equatable, Sendable {
    public var frameId: UInt64
    public var dimension: UInt32
    public var vector: [Float]

    public init(frameId: UInt64, dimension: UInt32, vector: [Float]) {
        self.frameId = frameId
        self.dimension = dimension
        self.vector = vector
    }
}

public enum WALEntry: Equatable, Sendable {
    case putFrame(PutFrame)
    case deleteFrame(DeleteFrame)
    case supersedeFrame(SupersedeFrame)
    case putEmbedding(PutEmbedding)
}

public struct PendingMutation: Equatable, Sendable {
    public var sequence: UInt64
    public var entry: WALEntry

    public init(sequence: UInt64, entry: WALEntry) {
        self.sequence = sequence
        self.entry = entry
    }
}
