import Foundation

public struct FooterSlice: Equatable, Sendable {
    public let footerOffset: UInt64
    public let tocOffset: UInt64
    public let footer: MV2SFooter
    public let tocBytes: Data
}

/// Scans for the most recent valid MV2S footer.
public enum FooterScanner {
    public struct Limits: Sendable {
        public var maxTocBytes: UInt64 = Constants.maxTocBytes
        public var maxFooterScanBytes: UInt64 = Constants.maxFooterScanBytes

        public init() {}
    }

    /// Bounded scan over an in-memory buffer. Intended for tests and small buffers.
    public static func findLastValidFooter(in bytes: Data, limits: Limits = .init()) -> FooterSlice? {
        let footerSize = MV2SFooter.size
        guard bytes.count >= footerSize else { return nil }

        let scanWindow = Int(min(UInt64(bytes.count), limits.maxFooterScanBytes))
        let scanStart = bytes.count - scanWindow

        var best: FooterSlice?

        for pos in stride(from: bytes.count - footerSize, through: scanStart, by: -1) {
            guard bytes[pos] == Constants.footerMagic[0] else { continue }
            let magicEnd = pos + Constants.footerMagic.count
            guard magicEnd <= bytes.count else { continue }
            guard bytes[pos..<magicEnd] == Constants.footerMagic else { continue }

            let footerEnd = pos + footerSize
            guard footerEnd <= bytes.count else { continue }

            guard let footer = try? MV2SFooter.decode(from: bytes.subdata(in: pos..<footerEnd)) else {
                continue
            }

            guard footer.tocLen >= 32 else { continue }
            guard footer.tocLen <= limits.maxTocBytes else { continue }
            guard footer.tocLen <= UInt64(pos) else { continue }

            let tocOffset = pos - Int(footer.tocLen)
            let tocBytes = bytes.subdata(in: tocOffset..<pos)
            guard footer.hashMatches(tocBytes: tocBytes) else { continue }

            let candidate = FooterSlice(
                footerOffset: UInt64(pos),
                tocOffset: UInt64(tocOffset),
                footer: footer,
                tocBytes: tocBytes
            )

            if let current = best {
                if footer.generation > current.footer.generation {
                    best = candidate
                } else if footer.generation == current.footer.generation && candidate.footerOffset > current.footerOffset {
                    best = candidate
                }
            } else {
                best = candidate
            }
        }

        return best
    }

    /// Bounded scan over a file. Only the final `limits.maxFooterScanBytes` are searched.
    public static func findLastValidFooter(in fileURL: URL, limits: Limits = .init()) throws -> FooterSlice? {
        let handle = try FileHandle(forReadingFrom: fileURL)
        defer { try? handle.close() }

        let fileSize = try handle.seekToEnd()
        let scanStart = fileSize > limits.maxFooterScanBytes ? (fileSize - limits.maxFooterScanBytes) : 0
        let scanLength = Int(fileSize - scanStart)

        try handle.seek(toOffset: scanStart)
        guard let window = try handle.read(upToCount: scanLength) else {
            return nil
        }

        return try scanWindow(
            window: window,
            windowBaseOffset: scanStart,
            fileSize: fileSize,
            handle: handle,
            limits: limits
        )
    }

    private static func scanWindow(
        window: Data,
        windowBaseOffset: UInt64,
        fileSize: UInt64,
        handle: FileHandle,
        limits: Limits
    ) throws -> FooterSlice? {
        let footerSize = MV2SFooter.size
        guard window.count >= footerSize else { return nil }

        var best: FooterSlice?

        for localPos in stride(from: window.count - footerSize, through: 0, by: -1) {
            guard window[localPos] == Constants.footerMagic[0] else { continue }
            let magicEnd = localPos + Constants.footerMagic.count
            guard magicEnd <= window.count else { continue }
            guard window[localPos..<magicEnd] == Constants.footerMagic else { continue }

            let footerEnd = localPos + footerSize
            guard footerEnd <= window.count else { continue }

            guard let footer = try? MV2SFooter.decode(from: window.subdata(in: localPos..<footerEnd)) else {
                continue
            }

            guard footer.tocLen >= 32 else { continue }
            guard footer.tocLen <= limits.maxTocBytes else { continue }

            let footerOffset = windowBaseOffset + UInt64(localPos)
            guard footerOffset >= footer.tocLen else { continue }
            let tocOffset = footerOffset - footer.tocLen
            guard tocOffset + footer.tocLen == footerOffset else { continue }
            guard footerOffset <= fileSize else { continue }

            guard footer.tocLen <= UInt64(Int.max) else { continue }
            let tocLenInt = Int(footer.tocLen)

            try handle.seek(toOffset: tocOffset)
            guard let tocBytes = try handle.read(upToCount: tocLenInt), tocBytes.count == tocLenInt else {
                continue
            }
            guard footer.hashMatches(tocBytes: tocBytes) else { continue }

            let candidate = FooterSlice(
                footerOffset: footerOffset,
                tocOffset: tocOffset,
                footer: footer,
                tocBytes: tocBytes
            )

            if let current = best {
                if footer.generation > current.footer.generation {
                    best = candidate
                } else if footer.generation == current.footer.generation && candidate.footerOffset > current.footerOffset {
                    best = candidate
                }
            } else {
                best = candidate
            }
        }

        return best
    }
}

