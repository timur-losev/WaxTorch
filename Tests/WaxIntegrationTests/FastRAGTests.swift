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
