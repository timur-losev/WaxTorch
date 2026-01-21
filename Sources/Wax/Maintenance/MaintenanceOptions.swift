import Foundation

public struct MaintenanceOptions: Sendable, Equatable {
    public var maxFrames: Int?
    public var maxWallTimeMs: Int?
    public var surrogateMaxTokens: Int
    public var overwriteExisting: Bool

    public init(
        maxFrames: Int? = nil,
        maxWallTimeMs: Int? = nil,
        surrogateMaxTokens: Int = 60,
        overwriteExisting: Bool = false
    ) {
        self.maxFrames = maxFrames
        self.maxWallTimeMs = maxWallTimeMs
        self.surrogateMaxTokens = surrogateMaxTokens
        self.overwriteExisting = overwriteExisting
    }
}

