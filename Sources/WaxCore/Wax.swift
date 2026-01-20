import Foundation
import Logging

/// Primary handle for interacting with a `.mv2s` memory file.
/// 
/// Holds the file descriptor, lock, header, TOC, and in-memory index state.
/// All mutable state is isolated within this actor for thread safety.
public actor Wax {
    private static let logger = Logger(label: "com.wax.core")

    // MARK: - Lifecycle

    public init() {
        // Stub - will be implemented in Phase 4
    }

    // MARK: - Public API (stubs)

    /// Create a new, empty `.mv2s` file
    public static func create(at url: URL) async throws -> Wax {
        fatalError("Not implemented - Phase 4")
    }

    /// Open an existing `.mv2s` file
    public static func open(at url: URL) async throws -> Wax {
        fatalError("Not implemented - Phase 4")
    }
}
