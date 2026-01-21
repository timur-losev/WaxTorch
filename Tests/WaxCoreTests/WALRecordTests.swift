import Foundation
import Testing
@testable import WaxCore

@Test func walHeaderSizeIsFixed() {
    #expect(WALRecord.headerSize == 48)
    #expect(WALRecordHeader.size == 48)
}

@Test func walSentinelDetection() throws {
    let zeros = Data(repeating: 0, count: WALRecordHeader.size)
    let header = try WALRecordHeader.decode(from: zeros, offset: 0)
    #expect(header.isSentinel)

    let record = try WALRecord.decodeRecord(from: zeros, walSize: 1024)
    if case .sentinel = record {
        #expect(Bool(true))
    } else {
        #expect(Bool(false))
    }
}

@Test func walDataRecordHashVerification() throws {
    let payload = Data("hello".utf8)
    let record = WALRecord.data(sequence: 1, flags: [], payload: payload)
    let encoded = try record.encode()

    let decoded = try WALRecord.decodeRecord(from: encoded, walSize: 1024)
    if case .data(let sequence, _, let decodedPayload) = decoded {
        #expect(sequence == 1)
        #expect(decodedPayload == payload)
    } else {
        #expect(Bool(false))
    }

    var corrupted = encoded
    corrupted[WALRecord.headerSize] ^= 0xFF

    do {
        _ = try WALRecord.decodeRecord(from: corrupted, walSize: 1024)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .walCorruption = error else {
            #expect(Bool(false))
            return
        }
    }
}

@Test func walPaddingRoundTrip() throws {
    let record = WALRecord.padding(sequence: 5, skipBytes: 12)
    let encoded = try record.encode()

    let header = try WALRecordHeader.decode(from: encoded, offset: 0)
    #expect(header.flags.contains(.isPadding))
    #expect(header.length == 12)
    #expect(header.checksum == WALRecord.paddingChecksum)

    let decoded = try WALRecord.decodeRecord(from: encoded, walSize: 256)
    if case .padding(let sequence, let skipBytes) = decoded {
        #expect(sequence == 5)
        #expect(skipBytes == 12)
    } else {
        #expect(Bool(false))
    }
}

@Test func walRejectsInvalidLengths() throws {
    let headerZeroLen = WALRecordHeader(
        sequence: 1,
        length: 0,
        flags: [],
        checksum: WALRecord.paddingChecksum
    )
    let zeroLenData = try headerZeroLen.encode()

    do {
        _ = try WALRecord.decodeRecord(from: zeroLenData, walSize: 256)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .walCorruption = error else {
            #expect(Bool(false))
            return
        }
    }

    let walSize: UInt64 = 64
    let oversizedPayload = Data(repeating: 0xAA, count: 32)
    let oversizedHeader = WALRecordHeader(
        sequence: 2,
        length: UInt32(oversizedPayload.count),
        flags: [],
        checksum: SHA256Checksum.digest(oversizedPayload)
    )
    var oversizedData = try oversizedHeader.encode()
    oversizedData.append(oversizedPayload)

    do {
        _ = try WALRecord.decodeRecord(from: oversizedData, walSize: walSize)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .walCorruption = error else {
            #expect(Bool(false))
            return
        }
    }
}
