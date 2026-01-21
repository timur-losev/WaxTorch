import Foundation

public struct MaintenanceReport: Sendable, Equatable {
    public var scannedFrames: Int
    public var eligibleFrames: Int
    public var generatedSurrogates: Int
    public var supersededSurrogates: Int
    public var skippedUpToDate: Int
    public var didTimeout: Bool

    public init(
        scannedFrames: Int = 0,
        eligibleFrames: Int = 0,
        generatedSurrogates: Int = 0,
        supersededSurrogates: Int = 0,
        skippedUpToDate: Int = 0,
        didTimeout: Bool = false
    ) {
        self.scannedFrames = scannedFrames
        self.eligibleFrames = eligibleFrames
        self.generatedSurrogates = generatedSurrogates
        self.supersededSurrogates = supersededSurrogates
        self.skippedUpToDate = skippedUpToDate
        self.didTimeout = didTimeout
    }
}

