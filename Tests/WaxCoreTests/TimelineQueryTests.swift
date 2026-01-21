import Foundation
import Testing
import WaxCore

@Test func timelineReturnsAllFrames() {
    let frames = makeTestFrames(count: 10)
    let query = TimelineQuery(limit: 100, order: .chronological)
    let results = TimelineQuery.filter(frames: frames, query: query)
    #expect(results.count == 10)
}

@Test func timelineLimitRespected() {
    let frames = makeTestFrames(count: 100)
    let query = TimelineQuery(limit: 10)
    let results = TimelineQuery.filter(frames: frames, query: query)
    #expect(results.count == 10)
}

@Test func timelineChronologicalOrder() {
    let frames = makeTestFrames(count: 5)
    let query = TimelineQuery(limit: 5, order: .chronological)
    let results = TimelineQuery.filter(frames: frames, query: query)
    for i in 1..<results.count {
        #expect(results[i].timestamp >= results[i - 1].timestamp)
    }
}

@Test func timelineReverseChronological() {
    let frames = makeTestFrames(count: 5)
    let query = TimelineQuery(limit: 5, order: .reverseChronological)
    let results = TimelineQuery.filter(frames: frames, query: query)
    for i in 1..<results.count {
        #expect(results[i].timestamp <= results[i - 1].timestamp)
    }
}

@Test func timelineDateRange() {
    let frames = makeTestFrames(count: 10)
    let midTimestamp = frames[4].timestamp
    let query = TimelineQuery(limit: 100, after: midTimestamp - 1000, before: midTimestamp + 1000)
    let results = TimelineQuery.filter(frames: frames, query: query)

    for frame in results {
        #expect(frame.timestamp >= midTimestamp - 1000)
        #expect(frame.timestamp < midTimestamp + 1000)
    }
}

@Test func timelineExcludesDeletedFrames() {
    var frames = makeTestFrames(count: 5)
    frames[2].status = .deleted
    let query = TimelineQuery(limit: 100)
    let results = TimelineQuery.filter(frames: frames, query: query)
    #expect(results.count == 4)
    #expect(results.contains { $0.id == frames[2].id } == false)
}

@Test func timelineIncludesDeletedFramesWhenEnabled() {
    var frames = makeTestFrames(count: 5)
    frames[2].status = .deleted
    let query = TimelineQuery(limit: 100, includeDeleted: true)
    let results = TimelineQuery.filter(frames: frames, query: query)
    #expect(results.count == 5)
    #expect(results.contains { $0.id == frames[2].id })
}

private func makeTestFrames(count: Int) -> [FrameMeta] {
    let baseTime: Int64 = 1_000_000
    return (0..<count).map { i in
        FrameMeta(
            id: UInt64(i),
            timestamp: baseTime + Int64(i) * 1_000,
            anchorTs: nil,
            kind: nil,
            track: nil,
            payloadOffset: 0,
            payloadLength: 0,
            checksum: Data(repeating: 0, count: 32),
            uri: nil,
            title: nil,
            canonicalEncoding: .plain,
            canonicalLength: nil,
            storedChecksum: nil,
            metadata: nil,
            searchText: nil,
            tags: [],
            labels: [],
            contentDates: [],
            role: .document,
            parentId: nil,
            chunkIndex: nil,
            chunkCount: nil,
            chunkManifest: nil,
            status: .active,
            supersedes: nil,
            supersededBy: nil
        )
    }
}
