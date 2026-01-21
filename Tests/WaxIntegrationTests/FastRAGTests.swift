import Foundation
import Testing
@testable import Wax

@Test
func fastRAGProducesSnippetsAndSingleExpansionWhenAvailable() async throws {
    try await TempFiles.withTempFile { url in
        let wax = try await Wax.create(at: url)
        let text = try await wax.enableTextSearch()

        let id0 = try await wax.put(Data("Swift is safe and fast.".utf8))
        try await text.index(frameId: id0, text: "Swift is safe and fast.")
        let id1 = try await wax.put(Data("Rust is fearless.".utf8))
        try await text.index(frameId: id1, text: "Rust is fearless.")

        try await text.commit()

        let builder = FastRAGContextBuilder()
        let config = FastRAGConfig(maxContextTokens: 40, expansionMaxTokens: 20, snippetMaxTokens: 10, maxSnippets: 5, searchTopK: 4)
        let ctx = try await builder.build(query: "Swift", wax: wax, config: config)

        #expect(!ctx.items.isEmpty)
        #expect(ctx.items.contains { $0.kind == .expanded })
        #expect(ctx.items.filter { $0.kind == .expanded }.count == 1)

        try await wax.close()
    }
}

@Test
func fastRAGIsDeterministicAndEnforcesTokenBudgets() async throws {
    try await TempFiles.withTempFile { url in
        let wax = try await Wax.create(at: url)
        let text = try await wax.enableTextSearch()

        let long = String(repeating: "Swift concurrency uses actors and tasks. ", count: 80)
        let id0 = try await wax.put(Data(long.utf8), options: FrameMetaSubset(searchText: long))
        try await text.index(frameId: id0, text: long)

        let snippet1 = "Rust uses ownership and borrowing to prevent data races."
        let id1 = try await wax.put(Data(snippet1.utf8), options: FrameMetaSubset(searchText: snippet1))
        try await text.index(frameId: id1, text: snippet1)

        let snippet2 = "Swift uses ARC and structured concurrency."
        let id2 = try await wax.put(Data(snippet2.utf8), options: FrameMetaSubset(searchText: snippet2))
        try await text.index(frameId: id2, text: snippet2)

        try await text.commit()

        let builder = FastRAGContextBuilder()
        let config = FastRAGConfig(
            maxContextTokens: 40,
            expansionMaxTokens: 15,
            snippetMaxTokens: 8,
            maxSnippets: 10,
            searchTopK: 10,
            searchMode: .textOnly
        )

        let ctxA = try await builder.build(query: "Swift", wax: wax, config: config)
        let ctxB = try await builder.build(query: "Swift", wax: wax, config: config)
        #expect(ctxA == ctxB)

        #expect(ctxA.items.allSatisfy { $0.kind == .expanded || $0.kind == .snippet })
        #expect(ctxA.items.filter { $0.kind == .expanded }.count <= 1)

        let counter = try await TokenCounter()
        var sumTokens = 0
        for item in ctxA.items {
            sumTokens += await counter.count(item.text)
        }
        #expect(ctxA.totalTokens == sumTokens)
        #expect(ctxA.totalTokens <= config.maxContextTokens)

        if let expanded = ctxA.items.first(where: { $0.kind == .expanded }) {
            #expect(await counter.count(expanded.text) <= config.expansionMaxTokens)
            #expect(ctxA.items.filter { $0.kind == .snippet }.allSatisfy { $0.frameId != expanded.frameId })
        }
        for snippet in ctxA.items where snippet.kind == .snippet {
            #expect(await counter.count(snippet.text) <= config.snippetMaxTokens)
        }

        try await wax.close()
    }
}

@Test
func fastRAGSkipsNonUTF8ExpansionCandidates() async throws {
    try await TempFiles.withTempFile { url in
        let wax = try await Wax.create(at: url)
        let text = try await wax.enableTextSearch()

        let invalid = Data([0xFF, 0xFE, 0xFD, 0xFC])
        let invalidId = try await wax.put(invalid, options: FrameMetaSubset(searchText: "Swift Swift Swift"))
        try await text.index(frameId: invalidId, text: "Swift Swift Swift")

        let valid = "Swift is safe and fast."
        let validId = try await wax.put(Data(valid.utf8), options: FrameMetaSubset(searchText: valid))
        try await text.index(frameId: validId, text: valid)

        try await text.commit()

        let builder = FastRAGContextBuilder()
        let config = FastRAGConfig(maxContextTokens: 40, expansionMaxTokens: 20, snippetMaxTokens: 10, maxSnippets: 5, searchTopK: 4, searchMode: .textOnly)
        let ctx = try await builder.build(query: "Swift", wax: wax, config: config)

        let expanded = ctx.items.filter { $0.kind == .expanded }
        #expect(expanded.count == 1)
        #expect(expanded.first?.frameId == validId)

        try await wax.close()
    }
}

@Test
func fastRAGSkipsExpansionWhenBytesExceedCap() async throws {
    try await TempFiles.withTempFile { url in
        let wax = try await Wax.create(at: url)
        let text = try await wax.enableTextSearch()

        let large = String(repeating: "Swift ", count: 2000)
        let largeId = try await wax.put(Data(large.utf8), options: FrameMetaSubset(searchText: large))
        try await text.index(frameId: largeId, text: large)

        try await text.commit()

        let builder = FastRAGContextBuilder()
        var config = FastRAGConfig(
            maxContextTokens: 40,
            expansionMaxTokens: 20,
            snippetMaxTokens: 10,
            maxSnippets: 5,
            searchTopK: 4,
            searchMode: .textOnly
        )
        config.expansionMaxBytes = 64

        let ctx = try await builder.build(query: "Swift", wax: wax, config: config)
        #expect(ctx.items.allSatisfy { $0.kind == .snippet })

        try await wax.close()
    }
}

@Test
func fastRAGExpansionLengthMismatchThrows() throws {
    do {
        try FastRAGContextBuilder.validateExpansionPayloadSize(
            expectedBytes: 128,
            actualBytes: 64,
            maxBytes: 1024
        )
        #expect(Bool(false))
    } catch let error as WaxError {
        if case .io(let message) = error {
            #expect(message.contains("expansion payload length mismatch"))
        } else {
            #expect(Bool(false))
        }
    } catch {
        #expect(Bool(false))
    }
}

@Test
func denseCachedSkipsInvalidSurrogateAndFallsBackToSnippet() async throws {
    try await TempFiles.withTempFile { url in
        let wax = try await Wax.create(at: url)
        let text = try await wax.enableTextSearch()

        let body = "Swift concurrency uses actors and tasks."
        let sourceId = try await wax.put(Data(body.utf8), options: FrameMetaSubset(searchText: body))
        try await text.index(frameId: sourceId, text: body)
        try await text.commit()

        var meta = Metadata()
        meta.entries["source_frame_id"] = String(sourceId)
        meta.entries["surrogate_algo"] = "test_v1"
        meta.entries["surrogate_version"] = "1"
        meta.entries["source_content_hash"] = "deadbeef"

        var subset = FrameMetaSubset()
        subset.kind = "surrogate"
        subset.role = .system
        subset.metadata = meta

        _ = try await wax.put(Data([0xFF, 0xFE, 0xFD, 0xFC]), options: subset)
        try await wax.commit()

        let builder = FastRAGContextBuilder()
        let config = FastRAGConfig(
            mode: .denseCached,
            maxContextTokens: 40,
            expansionMaxTokens: 0,
            snippetMaxTokens: 12,
            maxSnippets: 5,
            maxSurrogates: 2,
            surrogateMaxTokens: 8,
            searchTopK: 5,
            searchMode: .textOnly
        )

        let ctx = try await builder.build(query: "Swift", wax: wax, config: config)
        #expect(ctx.items.contains { $0.kind == .snippet })
        #expect(!ctx.items.contains { $0.kind == .surrogate })

        try await wax.close()
    }
}

@Test
func denseCachedSkipsSurrogateWhenFrameContentThrowsAndStillReturnsSnippets() async throws {
    try await TempFiles.withTempFile { url in
        let wax = try await Wax.create(at: url)
        let text = try await wax.enableTextSearch()

        let body = "Swift concurrency uses actors and tasks."
        let sourceId = try await wax.put(Data(body.utf8), options: FrameMetaSubset(searchText: body))
        try await text.index(frameId: sourceId, text: body)
        try await text.commit()

        var meta = Metadata()
        meta.entries["source_frame_id"] = String(sourceId)
        meta.entries["surrogate_algo"] = "test_v1"
        meta.entries["surrogate_version"] = "1"
        meta.entries["source_content_hash"] = "deadbeef"

        var subset = FrameMetaSubset()
        subset.kind = "surrogate"
        subset.role = .system
        subset.metadata = meta

        let surrogateText = String(repeating: "Swift concurrency is deterministic. ", count: 500)
        let surrogateId = try await wax.put(Data(surrogateText.utf8), options: subset, compression: .lzfse)
        try await wax.commit()

        let surrogateMeta = try await wax.frameMeta(frameId: surrogateId)
        #expect(surrogateMeta.canonicalEncoding != .plain)
        #expect(surrogateMeta.payloadLength > 0)

        let handle = try FileHandle(forUpdating: url)
        let offset = surrogateMeta.payloadOffset
        let corruptCount = Int(min(surrogateMeta.payloadLength, 256))
        try handle.seek(toOffset: offset)
        try handle.write(contentsOf: Data(repeating: 0, count: corruptCount))
        try handle.close()

        let builder = FastRAGContextBuilder()
        let config = FastRAGConfig(
            mode: .denseCached,
            maxContextTokens: 40,
            expansionMaxTokens: 0,
            snippetMaxTokens: 12,
            maxSnippets: 5,
            maxSurrogates: 2,
            surrogateMaxTokens: 8,
            searchTopK: 5,
            searchMode: .textOnly
        )

        let ctx = try await builder.build(query: "Swift", wax: wax, config: config)
        #expect(ctx.items.contains { $0.kind == .snippet })
        #expect(!ctx.items.contains { $0.kind == .surrogate })

        try await wax.close()
    }
}

@Test
func denseCachedEnforcesSurrogateLimitsAndSkipsSourceSnippets() async throws {
    try await TempFiles.withTempFile { url in
        let wax = try await Wax.create(at: url)
        let text = try await wax.enableTextSearch()

        let chunks = [
            "Swift uses actors for isolation.",
            "Swift concurrency uses tasks and structured concurrency.",
            "Swift makes async code readable and safe."
        ]

        var sourceIds: [UInt64] = []
        for chunk in chunks {
            let frameId = try await wax.put(Data(chunk.utf8), options: FrameMetaSubset(searchText: chunk))
            try await text.index(frameId: frameId, text: chunk)
            sourceIds.append(frameId)
        }
        try await text.commit()

        for (idx, sourceId) in sourceIds.enumerated() {
            var meta = Metadata()
            meta.entries["source_frame_id"] = String(sourceId)
            meta.entries["surrogate_algo"] = "test_v1"
            meta.entries["surrogate_version"] = "1"
            meta.entries["source_content_hash"] = String(idx)

            var subset = FrameMetaSubset()
            subset.kind = "surrogate"
            subset.role = .system
            subset.metadata = meta

            let surrogateText = "Surrogate \(idx) " + String(repeating: "Swift ", count: 20)
            _ = try await wax.put(Data(surrogateText.utf8), options: subset)
        }
        try await wax.commit()

        let builder = FastRAGContextBuilder()
        let config = FastRAGConfig(
            mode: .denseCached,
            maxContextTokens: 30,
            expansionMaxTokens: 0,
            snippetMaxTokens: 10,
            maxSnippets: 6,
            maxSurrogates: 1,
            surrogateMaxTokens: 6,
            searchTopK: 10,
            searchMode: .textOnly
        )

        let ctx = try await builder.build(query: "Swift", wax: wax, config: config)
        let surrogates = ctx.items.filter { $0.kind == .surrogate }
        let snippets = ctx.items.filter { $0.kind == .snippet }
        #expect(surrogates.count == 1)

        let counter = try await TokenCounter()
        for surrogate in surrogates {
            #expect(await counter.count(surrogate.text) <= config.surrogateMaxTokens)
            let meta = try await wax.frameMeta(frameId: surrogate.frameId)
            if let sourceRaw = meta.metadata?.entries["source_frame_id"],
               let sourceId = UInt64(sourceRaw) {
                #expect(snippets.allSatisfy { $0.frameId != sourceId })
            } else {
                #expect(Bool(false))
            }
        }

        var sawSnippet = false
        for item in ctx.items {
            if item.kind == .snippet { sawSnippet = true }
            if sawSnippet {
                #expect(item.kind != .surrogate)
            }
        }

        try await wax.close()
    }
}
