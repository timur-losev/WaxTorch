import Compression
import Foundation

/// Payload compression using Apple's compression library (`compression.h`).
public enum PayloadCompressor {
    /// Compress data using the specified algorithm.
    ///
    /// v1 determinism notes:
    /// - Cap maximum output to avoid unbounded expansion for small inputs.
    public static func compress(_ data: Data, algorithm: CompressionKind) throws -> Data {
        guard algorithm != .none else { return data }
        if data.isEmpty { return Data() }

        let maxCapacity = Self.maxCompressedCapacity(forInputSize: data.count)
        var dstCapacity = max(64, min(maxCapacity, max(1, data.count)))

        while dstCapacity <= maxCapacity {
            var dst = [UInt8](repeating: 0, count: dstCapacity)
            let written = data.withUnsafeBytes { raw -> Int in
                guard let src = raw.bindMemory(to: UInt8.self).baseAddress else { return 0 }
                return dst.withUnsafeMutableBytes { dstRaw -> Int in
                    guard let out = dstRaw.bindMemory(to: UInt8.self).baseAddress else { return 0 }
                    return compression_encode_buffer(
                        out,
                        dstRaw.count,
                        src,
                        raw.count,
                        nil,
                        algorithm.algorithm
                    )
                }
            }

            if written > 0 {
                dst.removeSubrange(written..<dst.count)
                return Data(dst)
            }

            let doubled = dstCapacity &* 2
            if doubled <= dstCapacity { break }
            dstCapacity = doubled
        }

        throw WaxError.io("compression failed: output did not fit within cap \(maxCapacity) bytes")
    }

    /// Decompress data using the specified algorithm.
    ///
    /// `uncompressedLength` is required for determinism.
    public static func decompress(
        _ data: Data,
        algorithm: CompressionKind,
        uncompressedLength: Int
    ) throws -> Data {
        guard algorithm != .none else { return data }

        guard uncompressedLength >= 0 else {
            throw WaxError.io("uncompressedLength must be >= 0")
        }
        if uncompressedLength == 0 { return Data() }

        var dst = [UInt8](repeating: 0, count: uncompressedLength)
        let written = data.withUnsafeBytes { raw -> Int in
            guard let src = raw.bindMemory(to: UInt8.self).baseAddress else { return 0 }
            return dst.withUnsafeMutableBytes { dstRaw -> Int in
                guard let out = dstRaw.bindMemory(to: UInt8.self).baseAddress else { return 0 }
                return compression_decode_buffer(
                    out,
                    dstRaw.count,
                    src,
                    raw.count,
                    nil,
                    algorithm.algorithm
                )
            }
        }

        guard written == uncompressedLength else {
            throw WaxError.io("decompress truncated or failed: wrote \(written), expected \(uncompressedLength)")
        }
        return Data(dst)
    }

    private static func maxCompressedCapacity(forInputSize inputSize: Int) -> Int {
        let plus128 = inputSize > Int.max - 128 ? Int.max : inputSize + 128
        let quadruple: Int
        if inputSize > Int.max / 4 {
            quadruple = Int.max
        } else {
            quadruple = inputSize * 4
        }
        return max(plus128, quadruple)
    }
}

