import Foundation
import WaxCore

private struct GeneratorConfig: Sendable {
    let outputDirectory: URL

    static func parse(arguments: [String]) throws -> GeneratorConfig {
        var outputPath: String?

        var index = 0
        while index < arguments.count {
            let arg = arguments[index]
            switch arg {
            case "--output":
                let next = index + 1
                guard next < arguments.count else {
                    throw NSError(
                        domain: "WaxParityFixtureGenerator",
                        code: 2,
                        userInfo: [NSLocalizedDescriptionKey: "missing value for --output"]
                    )
                }
                outputPath = arguments[next]
                index += 2
            default:
                throw NSError(
                    domain: "WaxParityFixtureGenerator",
                    code: 2,
                    userInfo: [NSLocalizedDescriptionKey: "unknown argument: \(arg)"]
                )
            }
        }

        let fm = FileManager.default
        let outputURL: URL
        if let outputPath {
            outputURL = URL(fileURLWithPath: outputPath, isDirectory: true)
        } else {
            outputURL = URL(fileURLWithPath: fm.currentDirectoryPath, isDirectory: true)
                .appendingPathComponent("fixtures", isDirectory: true)
                .appendingPathComponent("parity", isDirectory: true)
                .appendingPathComponent("swift", isDirectory: true)
        }

        return GeneratorConfig(outputDirectory: outputURL)
    }

    static func printUsage() {
        print("Usage: swift run WaxParityFixtureGenerator [--output <path>]")
    }
}

private struct FixtureGenerator {
    let outputDirectory: URL
    let fileManager = FileManager.default
    private let fixtureWalSize: UInt64 = 64 * 1024

    func run() async throws {
        try fileManager.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
        print("[swift-fixture-gen] output_dir=\(outputDirectory.path)")

        let validEmpty = outputDirectory.appendingPathComponent("swift_valid_empty.mv2s")
        try await generateValidEmpty(at: validEmpty)

        let validPayload = outputDirectory.appendingPathComponent("swift_valid_payload.mv2s")
        try await generateValidPayload(at: validPayload)

        let validCompressed = outputDirectory.appendingPathComponent("swift_valid_compressed_lz4.mv2s")
        try await generateValidCompressed(at: validCompressed)

        let verifyFailPayload = outputDirectory.appendingPathComponent("swift_verify_fail_payload_checksum.mv2s")
        try generateVerifyFailPayload(from: validPayload, to: verifyFailPayload)

        let openFailFooterMagic = outputDirectory.appendingPathComponent("swift_open_fail_bad_footer_magic.mv2s")
        try generateOpenFailFooterMagic(from: validEmpty, to: openFailFooterMagic)
    }

    private func generateValidEmpty(at fixtureURL: URL) async throws {
        try resetFixtureFiles(at: fixtureURL)
        let wax = try await Wax.create(at: fixtureURL, walSize: fixtureWalSize)
        try await wax.close()

        let stats = try await verifyAndCollectStats(at: fixtureURL)
        try writeExpected(
            for: fixtureURL,
            lines: [
                "mode=pass",
                "verify_deep=true",
                "frame_count=\(stats.frameCount)",
                "generation=\(stats.generation)",
            ]
        )
        print("[swift-fixture-gen] generated \(fixtureURL.lastPathComponent) mode=pass")
    }

    private func generateValidPayload(at fixtureURL: URL) async throws {
        try resetFixtureFiles(at: fixtureURL)
        let wax = try await Wax.create(at: fixtureURL, walSize: fixtureWalSize)
        _ = try await wax.put(Data("swift parity payload fixture".utf8))
        try await wax.commit()
        try await wax.close()

        let stats = try await verifyAndCollectStats(at: fixtureURL)
        try writeExpected(
            for: fixtureURL,
            lines: [
                "mode=pass",
                "verify_deep=true",
                "frame_count=\(stats.frameCount)",
                "generation=\(stats.generation)",
            ]
        )
        print("[swift-fixture-gen] generated \(fixtureURL.lastPathComponent) mode=pass")
    }

    private func generateValidCompressed(at fixtureURL: URL) async throws {
        try resetFixtureFiles(at: fixtureURL)
        let wax = try await Wax.create(at: fixtureURL, walSize: fixtureWalSize)

        // Repetitive payload makes compression deterministic enough for fixture generation.
        let largePayload = String(repeating: "swift-compressed-parity-fixture-", count: 512)
        _ = try await wax.put(Data(largePayload.utf8), compression: .lz4)
        try await wax.commit()
        try await wax.close()

        guard try firstFrameCanonicalEncoding(at: fixtureURL) != .plain else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 3,
                userInfo: [NSLocalizedDescriptionKey: "expected compressed frame but got plain encoding"]
            )
        }

        let stats = try await verifyAndCollectStats(at: fixtureURL)
        try writeExpected(
            for: fixtureURL,
            lines: [
                "mode=pass",
                "verify_deep=true",
                "frame_count=\(stats.frameCount)",
                "generation=\(stats.generation)",
            ]
        )
        print("[swift-fixture-gen] generated \(fixtureURL.lastPathComponent) mode=pass")
    }

    private func generateVerifyFailPayload(from sourceURL: URL, to fixtureURL: URL) throws {
        try resetFixtureFiles(at: fixtureURL)
        try fileManager.copyItem(at: sourceURL, to: fixtureURL)
        try mutateFirstFramePayloadByte(at: fixtureURL)
        try writeExpected(
            for: fixtureURL,
            lines: [
                "mode=verify_fail",
                "verify_deep=true",
                "error_contains=stored checksum mismatch",
            ]
        )
        print("[swift-fixture-gen] generated \(fixtureURL.lastPathComponent) mode=verify_fail")
    }

    private func generateOpenFailFooterMagic(from sourceURL: URL, to fixtureURL: URL) throws {
        try resetFixtureFiles(at: fixtureURL)
        try fileManager.copyItem(at: sourceURL, to: fixtureURL)
        try mutateFooterMagicByte(at: fixtureURL)
        try writeExpected(
            for: fixtureURL,
            lines: [
                "mode=open_fail",
                "error_contains=no valid footer",
            ]
        )
        print("[swift-fixture-gen] generated \(fixtureURL.lastPathComponent) mode=open_fail")
    }

    private func verifyAndCollectStats(at fixtureURL: URL) async throws -> WaxStats {
        let wax = try await Wax.open(at: fixtureURL)
        do {
            try await wax.verify(deep: true)
            let stats = await wax.stats()
            try await wax.close()
            return stats
        } catch {
            try? await wax.close()
            throw error
        }
    }

    private func firstFrameCanonicalEncoding(at fixtureURL: URL) throws -> CanonicalEncoding {
        guard let footer = try FooterScanner.findLastValidFooter(in: fixtureURL) else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 4,
                userInfo: [NSLocalizedDescriptionKey: "no valid footer while reading fixture TOC"]
            )
        }
        let toc = try MV2STOC.decode(from: footer.tocBytes)
        guard let frame = toc.frames.first else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 5,
                userInfo: [NSLocalizedDescriptionKey: "expected at least one frame for compressed fixture"]
            )
        }
        return frame.canonicalEncoding
    }

    private func mutateFirstFramePayloadByte(at fixtureURL: URL) throws {
        guard let footer = try FooterScanner.findLastValidFooter(in: fixtureURL) else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 6,
                userInfo: [NSLocalizedDescriptionKey: "no valid footer while preparing verify_fail fixture"]
            )
        }
        let toc = try MV2STOC.decode(from: footer.tocBytes)
        guard let frame = toc.frames.first(where: { $0.payloadLength > 0 }) else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 7,
                userInfo: [NSLocalizedDescriptionKey: "no non-empty frame payload to corrupt"]
            )
        }

        let file = try FDFile.open(at: fixtureURL)
        defer { try? file.close() }
        var byte = try file.readExactly(length: 1, at: frame.payloadOffset)
        guard let first = byte.first else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 8,
                userInfo: [NSLocalizedDescriptionKey: "failed to read payload byte"]
            )
        }
        byte[byte.startIndex] = first ^ 0x01
        try file.writeAll(byte, at: frame.payloadOffset)
        try file.fsync()
    }

    private func mutateFooterMagicByte(at fixtureURL: URL) throws {
        guard let footer = try FooterScanner.findLastValidFooter(in: fixtureURL) else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 9,
                userInfo: [NSLocalizedDescriptionKey: "no valid footer while preparing open_fail fixture"]
            )
        }

        let file = try FDFile.open(at: fixtureURL)
        defer { try? file.close() }
        var byte = try file.readExactly(length: 1, at: footer.footerOffset)
        guard let first = byte.first else {
            throw NSError(
                domain: "WaxParityFixtureGenerator",
                code: 10,
                userInfo: [NSLocalizedDescriptionKey: "failed to read footer byte"]
            )
        }
        byte[byte.startIndex] = first ^ 0x01
        try file.writeAll(byte, at: footer.footerOffset)
        try file.fsync()
    }

    private func writeExpected(for fixtureURL: URL, lines: [String]) throws {
        let expectedURL = fixtureURL.appendingPathExtension("expected")
        let text = lines.joined(separator: "\n") + "\n"
        try text.write(to: expectedURL, atomically: true, encoding: .utf8)
    }

    private func resetFixtureFiles(at fixtureURL: URL) throws {
        try removeIfExists(fixtureURL)
        try removeIfExists(fixtureURL.appendingPathExtension("expected"))
    }

    private func removeIfExists(_ url: URL) throws {
        if fileManager.fileExists(atPath: url.path) {
            try fileManager.removeItem(at: url)
        }
    }
}

@main
enum WaxParityFixtureGeneratorMain {
    static func main() async throws {
        let args = Array(CommandLine.arguments.dropFirst())
        if args.contains("--help") || args.contains("-h") {
            GeneratorConfig.printUsage()
            return
        }

        let config = try GeneratorConfig.parse(arguments: args)
        try await FixtureGenerator(outputDirectory: config.outputDirectory).run()
        print("[swift-fixture-gen] done")
    }
}
