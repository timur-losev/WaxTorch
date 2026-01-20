import CryptoKit
import Foundation
import Testing
@testable import WaxCore

private func buildTocBytes(body: Data) -> Data {
    let zero32 = Data(repeating: 0, count: 32)
    let tocChecksum = Data(SHA256.hash(data: body + zero32))

    var tocBytes = Data()
    tocBytes.append(body)
    tocBytes.append(tocChecksum)
    return tocBytes
}

private func buildFileSegment(tocBody: Data, generation: UInt64) throws -> Data {
    let tocBytes = buildTocBytes(body: tocBody)
    let tocChecksum = Data(tocBytes.suffix(32))
    let footer = MV2SFooter(
        tocLen: UInt64(tocBytes.count),
        tocHash: tocChecksum,
        generation: generation,
        walCommittedSeq: generation
    )

    var result = Data()
    result.append(tocBytes)
    result.append(try footer.encode())
    return result
}

@Test func findsValidFooterAtEnd() throws {
    let tocBody = Data([0xAA, 0xBB, 0xCC, 0xDD])
    let expectedTocBytes = buildTocBytes(body: tocBody)
    let fileBytes = try buildFileSegment(tocBody: tocBody, generation: 7)

    try TempFiles.withTempFile { url in
        try fileBytes.write(to: url)
        let slice = try FooterScanner.findLastValidFooter(in: url)
        #expect(slice != nil)
        #expect(slice?.footer.generation == 7)
        #expect(slice?.tocBytes == expectedTocBytes)
    }
}

@Test func findsHighestGenerationWhenMultipleExist() throws {
    let tocBody1 = Data([0x01, 0x02, 0x03])
    let segment1 = try buildFileSegment(tocBody: tocBody1, generation: 1)

    let tocBody2 = Data([0x04, 0x05, 0x06, 0x07])
    let tocBytes2 = buildTocBytes(body: tocBody2)
    let segment2 = try buildFileSegment(tocBody: tocBody2, generation: 5)

    var fileBytes = Data()
    fileBytes.append(segment1)
    fileBytes.append(segment2)

    try TempFiles.withTempFile { url in
        try fileBytes.write(to: url)
        let slice = try FooterScanner.findLastValidFooter(in: url)
        #expect(slice != nil)
        #expect(slice?.footer.generation == 5)
        #expect(slice?.tocBytes == tocBytes2)
    }
}

@Test func skipsCorruptFooterFindsPrior() throws {
    let tocBody1 = Data([0x01, 0x02, 0x03])
    let segment1 = try buildFileSegment(tocBody: tocBody1, generation: 1)

    let tocBody2 = Data([0x04, 0x05, 0x06])
    var segment2 = try buildFileSegment(tocBody: tocBody2, generation: 2)

    // Corrupt the footer's toc_hash field.
    let footerStart = segment2.count - MV2SFooter.size
    let tocHashStart = footerStart + 16
    segment2[tocHashStart] ^= 0xFF

    var fileBytes = Data()
    fileBytes.append(segment1)
    fileBytes.append(segment2)

    try TempFiles.withTempFile { url in
        try fileBytes.write(to: url)
        let slice = try FooterScanner.findLastValidFooter(in: url)
        #expect(slice != nil)
        #expect(slice?.footer.generation == 1)
    }
}

@Test func handlesPartialFooterAtEnd() throws {
    let tocBody = Data([0xAA, 0xBB])
    var fileBytes = try buildFileSegment(tocBody: tocBody, generation: 3)

    fileBytes.append(Constants.footerMagic)
    fileBytes.append(Data([0x00, 0x01, 0x02]))

    try TempFiles.withTempFile { url in
        try fileBytes.write(to: url)
        let slice = try FooterScanner.findLastValidFooter(in: url)
        #expect(slice != nil)
        #expect(slice?.footer.generation == 3)
    }
}

@Test func returnsNilForEmptyFile() throws {
    try TempFiles.withTempFile { url in
        FileManager.default.createFile(atPath: url.path, contents: Data())
        let slice = try FooterScanner.findLastValidFooter(in: url)
        #expect(slice == nil)
    }
}

@Test func returnsNilForFileTooSmall() throws {
    let tooSmall = Data(repeating: 0, count: 32)
    try TempFiles.withTempFile { url in
        try tooSmall.write(to: url)
        let slice = try FooterScanner.findLastValidFooter(in: url)
        #expect(slice == nil)
    }
}

@Test func boundedScanDoesNotSearchEarlierBytes() throws {
    let segment = try buildFileSegment(tocBody: Data([0xAA]), generation: 1)
    var fileBytes = Data()
    fileBytes.append(segment)
    fileBytes.append(Data(repeating: 0x00, count: 2048))

    var limits = FooterScanner.Limits()
    limits.maxFooterScanBytes = 256

    try TempFiles.withTempFile { url in
        try fileBytes.write(to: url)
        let slice = try FooterScanner.findLastValidFooter(in: url, limits: limits)
        #expect(slice == nil)
    }
}

@Test func boundedScanFindsFooterNearEnd() throws {
    let segment = try buildFileSegment(tocBody: Data([0xDE, 0xAD, 0xBE, 0xEF]), generation: 42)
    var fileBytes = Data(repeating: 0x00, count: 2048)
    fileBytes.append(segment)

    var limits = FooterScanner.Limits()
    limits.maxFooterScanBytes = 256

    try TempFiles.withTempFile { url in
        try fileBytes.write(to: url)
        let slice = try FooterScanner.findLastValidFooter(in: url, limits: limits)
        #expect(slice != nil)
        #expect(slice?.footer.generation == 42)
    }
}

