import Compression

/// Supported compression algorithms for frame payload bytes (v1).
public enum CompressionKind: Sendable, Equatable {
    case none
    case lzfse
    case lz4
    case deflate

    var algorithm: compression_algorithm {
        switch self {
        case .none:
            // Not used by the compressor; callers short-circuit.
            return COMPRESSION_LZFSE
        case .lzfse:
            return COMPRESSION_LZFSE
        case .lz4:
            return COMPRESSION_LZ4
        case .deflate:
            return COMPRESSION_ZLIB
        }
    }
}

extension CompressionKind {
    init(canonicalEncoding: CanonicalEncoding) {
        switch canonicalEncoding {
        case .plain:
            self = .none
        case .lzfse:
            self = .lzfse
        case .lz4:
            self = .lz4
        case .deflate:
            self = .deflate
        }
    }

    var canonicalEncoding: CanonicalEncoding {
        switch self {
        case .none:
            return .plain
        case .lzfse:
            return .lzfse
        case .lz4:
            return .lz4
        case .deflate:
            return .deflate
        }
    }
}

