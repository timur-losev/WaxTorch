#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace waxcpp::core::wal {

inline constexpr std::uint64_t kRecordHeaderSize = 48;
inline constexpr std::uint32_t kFlagIsPadding = 1U << 0U;

struct WalRecordHeader {
  std::uint64_t sequence = 0;
  std::uint32_t length = 0;
  std::uint32_t flags = 0;
  std::array<std::byte, 32> checksum{};

  [[nodiscard]] bool IsSentinel() const;
  [[nodiscard]] bool IsPadding() const;
};

struct WalScanState {
  std::uint64_t last_sequence = 0;
  std::uint64_t write_pos = 0;
  std::uint64_t pending_bytes = 0;
};

enum class WalMutationKind : std::uint8_t {
  kPutFrame = 1,
  kDeleteFrame = 2,
  kSupersedeFrame = 3,
  kPutEmbedding = 4,
};

struct WalPutFrameInfo {
  std::uint64_t frame_id = 0;
  std::uint64_t payload_offset = 0;
  std::uint64_t payload_length = 0;
};

struct WalDeleteFrameInfo {
  std::uint64_t frame_id = 0;
};

struct WalSupersedeFrameInfo {
  std::uint64_t superseded_id = 0;
  std::uint64_t superseding_id = 0;
};

struct WalPutEmbeddingInfo {
  std::uint64_t frame_id = 0;
  std::uint32_t dimension = 0;
};

struct WalPendingMutationInfo {
  std::uint64_t sequence = 0;
  WalMutationKind kind = WalMutationKind::kDeleteFrame;
  std::optional<WalPutFrameInfo> put_frame;
  std::optional<WalDeleteFrameInfo> delete_frame;
  std::optional<WalSupersedeFrameInfo> supersede_frame;
  std::optional<WalPutEmbeddingInfo> put_embedding;
};

struct WalPendingScanResult {
  std::vector<WalPendingMutationInfo> pending_mutations;
  WalScanState state{};
};

[[nodiscard]] WalRecordHeader DecodeWalRecordHeader(std::span<const std::byte> bytes);
[[nodiscard]] bool IsTerminalMarker(const std::filesystem::path& path,
                                    std::uint64_t wal_offset,
                                    std::uint64_t wal_size,
                                    std::uint64_t cursor);
[[nodiscard]] WalScanState ScanWalState(const std::filesystem::path& path,
                                        std::uint64_t wal_offset,
                                        std::uint64_t wal_size,
                                        std::uint64_t checkpoint_pos);
[[nodiscard]] WalPendingScanResult ScanPendingMutationsWithState(const std::filesystem::path& path,
                                                                 std::uint64_t wal_offset,
                                                                 std::uint64_t wal_size,
                                                                 std::uint64_t checkpoint_pos,
                                                                 std::uint64_t committed_seq);

}  // namespace waxcpp::core::wal
