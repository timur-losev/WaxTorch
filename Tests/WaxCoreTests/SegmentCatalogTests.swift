import Foundation
import Testing
@testable import WaxCore

private func encodeSegmentCatalogBytes(entries: [SegmentCatalogEntry]) throws -> Data {
    var encoder = BinaryEncoder()
    encoder.encode(UInt32(entries.count))
    for entry in entries {
        encoder.encode(entry.segmentId)
        encoder.encode(entry.bytesOffset)
        encoder.encode(entry.bytesLength)
        guard entry.checksum.count == 32 else {
            throw WaxError.encodingError(reason: "segment checksum must be 32 bytes")
        }
        encoder.encodeFixedBytes(entry.checksum)
        encoder.encode(entry.compression.rawValue)
        encoder.encode(entry.kind.rawValue)
    }
    return encoder.data
}

@Test func segmentCatalogEncodeSortsByBytesOffset() throws {
    let entryA = SegmentCatalogEntry(
        segmentId: 2,
        bytesOffset: 200,
        bytesLength: 10,
        checksum: Data(repeating: 0xAA, count: 32),
        compression: .none,
        kind: .lex
    )
    let entryB = SegmentCatalogEntry(
        segmentId: 1,
        bytesOffset: 100,
        bytesLength: 10,
        checksum: Data(repeating: 0xBB, count: 32),
        compression: .none,
        kind: .lex
    )

    var catalog = SegmentCatalog(entries: [entryA, entryB])
    var encoder = BinaryEncoder()
    try catalog.encode(to: &encoder)

    var decoder = try BinaryDecoder(data: encoder.data)
    let decoded = try SegmentCatalog.decode(from: &decoder)
    try decoder.finalize()

    #expect(decoded.entries.map(\.bytesOffset) == [100, 200])
}

@Test func segmentCatalogDecodeRejectsUnsortedEntries() throws {
    let entryA = SegmentCatalogEntry(
        segmentId: 2,
        bytesOffset: 200,
        bytesLength: 10,
        checksum: Data(repeating: 0xAA, count: 32),
        compression: .none,
        kind: .lex
    )
    let entryB = SegmentCatalogEntry(
        segmentId: 1,
        bytesOffset: 100,
        bytesLength: 10,
        checksum: Data(repeating: 0xBB, count: 32),
        compression: .none,
        kind: .lex
    )
    let bytes = try encodeSegmentCatalogBytes(entries: [entryA, entryB])

    do {
        var decoder = try BinaryDecoder(data: bytes)
        _ = try SegmentCatalog.decode(from: &decoder)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .invalidToc = error else {
            #expect(Bool(false))
            return
        }
    }
}

@Test func segmentCatalogDecodeRejectsOverlappingEntries() throws {
    let entryA = SegmentCatalogEntry(
        segmentId: 1,
        bytesOffset: 100,
        bytesLength: 50,
        checksum: Data(repeating: 0xAA, count: 32),
        compression: .none,
        kind: .lex
    )
    let entryB = SegmentCatalogEntry(
        segmentId: 2,
        bytesOffset: 120,
        bytesLength: 10,
        checksum: Data(repeating: 0xBB, count: 32),
        compression: .none,
        kind: .lex
    )
    let bytes = try encodeSegmentCatalogBytes(entries: [entryA, entryB])

    do {
        var decoder = try BinaryDecoder(data: bytes)
        _ = try SegmentCatalog.decode(from: &decoder)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .invalidToc = error else {
            #expect(Bool(false))
            return
        }
    }
}

@Test func segmentCatalogDecodeRejectsInvalidEnums() throws {
    var encoder = BinaryEncoder()
    encoder.encode(UInt32(1))
    encoder.encode(UInt64(1))
    encoder.encode(UInt64(100))
    encoder.encode(UInt64(10))
    encoder.encodeFixedBytes(Data(repeating: 0xAA, count: 32))
    encoder.encode(UInt8(9)) // invalid compression
    encoder.encode(UInt8(9)) // invalid kind
    let bytes = encoder.data

    do {
        var decoder = try BinaryDecoder(data: bytes)
        _ = try SegmentCatalog.decode(from: &decoder)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .invalidToc = error else {
            #expect(Bool(false))
            return
        }
    }
}
