import Foundation

public protocol EmbeddingProvider: Sendable {
    var dimensions: Int { get }
    var normalize: Bool { get }
    var identity: EmbeddingIdentity? { get }
    func embed(_ text: String) async throws -> [Float]
}

public struct EmbeddingIdentity: Sendable, Equatable {
    public var provider: String?
    public var model: String?
    public var dimensions: Int?
    public var normalized: Bool?

    public init(
        provider: String? = nil,
        model: String? = nil,
        dimensions: Int? = nil,
        normalized: Bool? = nil
    ) {
        self.provider = provider
        self.model = model
        self.dimensions = dimensions
        self.normalized = normalized
    }
}

