import Foundation
import Testing
@testable import WaxCore

@Test func frameMetaSubsetRoundTrip() throws {
    let metadata = Metadata([
        "author": "Ada",
        "lang": "swift"
    ])

    var subset = FrameMetaSubset(
        uri: "mv2://doc/123",
        title: "Title",
        kind: "text",
        track: "main",
        tags: [TagPair(key: "k1", value: "v1"), TagPair(key: "k2", value: "v2")],
        labels: ["l1", "l2"],
        contentDates: ["2024-01-01"],
        role: .chunk,
        parentId: 99,
        chunkIndex: 1,
        chunkCount: 2,
        chunkManifest: Data([0x01, 0x02, 0x03]),
        status: .active,
        supersedes: 55,
        supersededBy: 77,
        searchText: "hello",
        metadata: metadata
    )

    var encoder = BinaryEncoder()
    try subset.encode(to: &encoder)

    var decoder = try BinaryDecoder(data: encoder.data)
    let decoded = try FrameMetaSubset.decode(from: &decoder)
    try decoder.finalize()

    #expect(decoded == subset)
}

@Test func frameMetaSubsetTagsAreKeyValuePairs() throws {
    let subset = FrameMetaSubset(tags: [TagPair(key: "topic", value: "swift")])
    var encoder = BinaryEncoder()
    var mutable = subset
    try mutable.encode(to: &encoder)

    var decoder = try BinaryDecoder(data: encoder.data)
    _ = try decoder.decodeOptional(String.self) // uri
    _ = try decoder.decodeOptional(String.self) // title
    _ = try decoder.decodeOptional(String.self) // kind
    _ = try decoder.decodeOptional(String.self) // track

    let tagCount = Int(try decoder.decode(UInt32.self))
    #expect(tagCount == 1)
    let key = try decoder.decode(String.self)
    let value = try decoder.decode(String.self)

    #expect(key == "topic")
    #expect(value == "swift")
}

@Test func metadataEncodesKeysInLexOrder() throws {
    var metadata = Metadata(["b": "2", "a": "1", "c": "3"])
    var encoder = BinaryEncoder()
    try metadata.encode(to: &encoder)

    var decoder = try BinaryDecoder(data: encoder.data)
    let count = try decoder.decode(UInt32.self)
    #expect(count == 3)

    let firstKey = try decoder.decode(String.self)
    let firstValue = try decoder.decode(String.self)
    let secondKey = try decoder.decode(String.self)
    let secondValue = try decoder.decode(String.self)
    let thirdKey = try decoder.decode(String.self)
    let thirdValue = try decoder.decode(String.self)

    #expect(firstKey == "a")
    #expect(firstValue == "1")
    #expect(secondKey == "b")
    #expect(secondValue == "2")
    #expect(thirdKey == "c")
    #expect(thirdValue == "3")
}
