import Foundation

public struct TicketRef: Equatable, Sendable {
    public var issuer: String
    public var seqNo: UInt64
    public var expiresInSecs: UInt64
    public var capacityBytes: UInt64
    public var verified: UInt8

    public init(
        issuer: String,
        seqNo: UInt64,
        expiresInSecs: UInt64,
        capacityBytes: UInt64,
        verified: UInt8
    ) {
        self.issuer = issuer
        self.seqNo = seqNo
        self.expiresInSecs = expiresInSecs
        self.capacityBytes = capacityBytes
        self.verified = verified
    }

    public static func emptyV1() -> TicketRef {
        TicketRef(issuer: "", seqNo: 0, expiresInSecs: 0, capacityBytes: 0, verified: 0)
    }
}

extension TicketRef: BinaryCodable {
    public mutating func encode(to encoder: inout BinaryEncoder) throws {
        guard verified <= 1 else {
            throw WaxError.encodingError(reason: "ticket verified must be 0 or 1 (got \(verified))")
        }
        try encoder.encode(issuer)
        encoder.encode(seqNo)
        encoder.encode(expiresInSecs)
        encoder.encode(capacityBytes)
        encoder.encode(verified)
    }

    public static func decode(from decoder: inout BinaryDecoder) throws -> TicketRef {
        let issuer = try decoder.decode(String.self)
        let seqNo = try decoder.decode(UInt64.self)
        let expiresInSecs = try decoder.decode(UInt64.self)
        let capacityBytes = try decoder.decode(UInt64.self)
        let verified = try decoder.decode(UInt8.self)
        guard verified <= 1 else {
            throw WaxError.invalidToc(reason: "ticket verified must be 0 or 1 (got \(verified))")
        }
        return TicketRef(
            issuer: issuer,
            seqNo: seqNo,
            expiresInSecs: expiresInSecs,
            capacityBytes: capacityBytes,
            verified: verified
        )
    }
}
