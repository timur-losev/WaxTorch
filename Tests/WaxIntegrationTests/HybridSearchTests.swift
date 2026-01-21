import Testing
import Wax

@Test func rrfWithDisjointResults() {
    let textResults: [(UInt64, Float)] = [(0, 0.9), (1, 0.8), (2, 0.7)]
    let vectorResults: [(UInt64, Float)] = [(3, 0.95), (4, 0.85), (5, 0.75)]

    let merged = HybridSearch.rrfFusion(
        textResults: textResults,
        vectorResults: vectorResults,
        k: 60,
        alpha: 0.5
    )

    #expect(merged.count == 6)
    let frameIds = Set(merged.map { $0.0 })
    #expect(frameIds == Set([0, 1, 2, 3, 4, 5]))
}

@Test func rrfWithOverlappingResults() {
    let textResults: [(UInt64, Float)] = [(0, 0.9), (1, 0.8)]
    let vectorResults: [(UInt64, Float)] = [(1, 0.95), (2, 0.85)]

    let merged = HybridSearch.rrfFusion(
        textResults: textResults,
        vectorResults: vectorResults,
        k: 60,
        alpha: 0.5
    )

    #expect(merged.count == 3)
    #expect(merged[0].0 == 1)
}

@Test func rrfAlphaWeighting() {
    let textResults: [(UInt64, Float)] = [(0, 0.9)]
    let vectorResults: [(UInt64, Float)] = [(1, 0.95)]

    let textOnly = HybridSearch.rrfFusion(
        textResults: textResults,
        vectorResults: vectorResults,
        k: 60,
        alpha: 1.0
    )
    #expect(textOnly[0].0 == 0)

    let vectorOnly = HybridSearch.rrfFusion(
        textResults: textResults,
        vectorResults: vectorResults,
        k: 60,
        alpha: 0.0
    )
    #expect(vectorOnly[0].0 == 1)
}

@Test func rrfWithEmptyTextResults() {
    let textResults: [(UInt64, Float)] = []
    let vectorResults: [(UInt64, Float)] = [(0, 0.9), (1, 0.8)]

    let merged = HybridSearch.rrfFusion(
        textResults: textResults,
        vectorResults: vectorResults,
        k: 60,
        alpha: 0.5
    )

    #expect(merged.count == 2)
}

@Test func rrfWithEmptyVectorResults() {
    let textResults: [(UInt64, Float)] = [(0, 0.9), (1, 0.8)]
    let vectorResults: [(UInt64, Float)] = []

    let merged = HybridSearch.rrfFusion(
        textResults: textResults,
        vectorResults: vectorResults,
        k: 60,
        alpha: 0.5
    )

    #expect(merged.count == 2)
}

