import Foundation
import Testing
@testable import WaxCore

@Test func uint8Roundtrip() throws {
    var encoder = BinaryEncoder()
    encoder.encode(UInt8(42))
    encoder.encode(UInt8(255))

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decode(UInt8.self) == 42)
    #expect(try decoder.decode(UInt8.self) == 255)
    try decoder.finalize()
}

@Test func uint16LittleEndian() throws {
    var encoder = BinaryEncoder()
    encoder.encode(UInt16(0x1234))
    #expect(encoder.data == Data([0x34, 0x12]))

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decode(UInt16.self) == 0x1234)
    try decoder.finalize()
}

@Test func uint32Roundtrip() throws {
    var encoder = BinaryEncoder()
    encoder.encode(UInt32(0xDEADBEEF))

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decode(UInt32.self) == 0xDEADBEEF)
    try decoder.finalize()
}

@Test func uint64Roundtrip() throws {
    var encoder = BinaryEncoder()
    encoder.encode(UInt64.max)

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decode(UInt64.self) == UInt64.max)
    try decoder.finalize()
}

@Test func int64Roundtrip() throws {
    var encoder = BinaryEncoder()
    encoder.encode(Int64(-1_000_000))

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decode(Int64.self) == -1_000_000)
    try decoder.finalize()
}

@Test func stringRoundtrip() throws {
    var encoder = BinaryEncoder()
    try encoder.encode("Hello, 世界!")

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decode(String.self) == "Hello, 世界!")
    try decoder.finalize()
}

@Test func emptyStringRoundtrip() throws {
    var encoder = BinaryEncoder()
    try encoder.encode("")
    #expect(encoder.data.count == 4)

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decode(String.self) == "")
    try decoder.finalize()
}

@Test func arrayRoundtrip() throws {
    var encoder = BinaryEncoder()
    try encoder.encode([UInt8(1), UInt8(2), UInt8(3)])

    var decoder = try BinaryDecoder(data: encoder.data)
    let result: [UInt8] = try decoder.decodeArray()
    #expect(result == [1, 2, 3])
    try decoder.finalize()
}

@Test func emptyArrayRoundtrip() throws {
    var encoder = BinaryEncoder()
    try encoder.encode([UInt64]())

    var decoder = try BinaryDecoder(data: encoder.data)
    let result: [UInt64] = try decoder.decodeArray()
    #expect(result == [])
    try decoder.finalize()
}

@Test func optionalPresentRoundtrip() throws {
    var encoder = BinaryEncoder()
    encoder.encode(Optional<UInt32>.some(42))

    #expect(encoder.data.count == 5)
    #expect(encoder.data[0] == 1)

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decodeOptional(UInt32.self) == 42)
    try decoder.finalize()
}

@Test func optionalAbsentRoundtrip() throws {
    var encoder = BinaryEncoder()
    encoder.encode(Optional<UInt32>.none)

    #expect(encoder.data.count == 1)
    #expect(encoder.data[0] == 0)

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decodeOptional(UInt32.self) == nil)
    try decoder.finalize()
}

@Test func fixedBytesRoundtrip() throws {
    let hash = Data(repeating: 0xAB, count: 32)
    var encoder = BinaryEncoder()
    encoder.encodeFixedBytes(hash)
    #expect(encoder.data.count == 32)

    var decoder = try BinaryDecoder(data: encoder.data)
    #expect(try decoder.decodeFixedBytes(count: 32) == hash)
    try decoder.finalize()
}

@Test func truncatedBufferThrows() throws {
    let bytes = Data([0x01, 0x02, 0x03]) // too short for UInt64
    var decoder = try BinaryDecoder(data: bytes)
    do {
        _ = try decoder.decode(UInt64.self)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .decodingError(let reason) = error else {
            #expect(Bool(false))
            return
        }
        #expect(reason.contains("truncated"))
    }
}

@Test func excessBytesThrowsOnFinalize() throws {
    var encoder = BinaryEncoder()
    encoder.encode(UInt8(1))
    encoder.encode(UInt8(2))

    var decoder = try BinaryDecoder(data: encoder.data)
    _ = try decoder.decode(UInt8.self)
    do {
        try decoder.finalize()
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .decodingError(let reason) = error else {
            #expect(Bool(false))
            return
        }
        #expect(reason.contains("excess"))
    }
}

@Test func invalidUTF8Throws() throws {
    var encoder = BinaryEncoder()
    encoder.encode(UInt32(3))
    var bytes = encoder.data
    bytes.append(contentsOf: [0xFF, 0xFE, 0x80])

    var decoder = try BinaryDecoder(data: bytes)
    do {
        _ = try decoder.decode(String.self)
        #expect(Bool(false))
    } catch let error as WaxError {
        guard case .decodingError(let reason) = error else {
            #expect(Bool(false))
            return
        }
        #expect(reason.contains("UTF-8"))
    }
}
