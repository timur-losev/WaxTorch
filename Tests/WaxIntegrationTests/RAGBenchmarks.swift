import XCTest
import Foundation
@testable import Wax

final class RAGBenchmarks: XCTestCase {
    var wax: Wax!
    var tempURL: URL!
    var memoryURL: URL!

    override func setUp() async throws {
        // Create a unique temp directory for each test run
        tempURL = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: tempURL, withIntermediateDirectories: true)
        
        memoryURL = tempURL.appendingPathComponent("bench.mv2s")
        wax = try await Wax.create(at: memoryURL)
        let text = try await wax.enableTextSearch()
        
        // Seed with some data to make the benchmark meaningful
        let sampleText = "Swift is a powerful and intuitive programming language for iOS, iPadOS, macOS, tvOS, and watchOS."
        for i in 0..<100 {
            let content = "\(sampleText) Batch \(i)"
            let id = try await wax.put(Data(content.utf8), options: FrameMetaSubset(searchText: content))
            try await text.index(frameId: id, text: content)
        }
        try await text.commit()
    }

    override func tearDown() async throws {
        try? await wax?.close()
        try? FileManager.default.removeItem(at: tempURL)
    }

    func testRAGBuildPerformance() async throws {
        let builder = FastRAGContextBuilder()
        let config = FastRAGConfig(maxContextTokens: 1000)
        let localWax = self.wax! // Capture as local to avoid capturing 'self' in Task
        
        measure {
            let exp = expectation(description: "RAG Build")
            Task {
                _ = try? await builder.build(query: "Swift", wax: localWax, config: config)
                exp.fulfill()
            }
            wait(for: [exp], timeout: 10.0)
        }
    }
    
    func testTokenCountingPerformance() async throws {
        let counter = try await TokenCounter.shared()
        let longText = String(repeating: "Swift concurrency is cool. ", count: 1000)
        
        measure {
            let exp = expectation(description: "Token Count")
            Task {
                _ = await counter.count(longText)
                exp.fulfill()
            }
            wait(for: [exp], timeout: 10.0)
        }
    }
}
