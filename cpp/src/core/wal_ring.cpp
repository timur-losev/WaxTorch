#include "wal_ring.hpp"

#include "mv2s_format.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace waxcpp::core::wal {
namespace {

std::runtime_error WalError(const std::string& message) {
  return std::runtime_error("wal ring error: " + message);
}

template <typename T>
T ReadLE(std::span<const std::byte> bytes, std::size_t offset) {
  static_assert(std::is_integral_v<T>, "ReadLE requires integral type");
  if (offset + sizeof(T) > bytes.size()) {
    throw WalError("read out of range");
  }
  using UnsignedT = std::make_unsigned_t<T>;
  UnsignedT out = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    out |= static_cast<UnsignedT>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (8U * i);
  }
  return static_cast<T>(out);
}

std::vector<std::byte> ReadExactly(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  if (length == 0) {
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw WalError("failed to open file for read: " + path.string());
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw WalError("failed to seek for read");
  }
  std::vector<std::byte> out(length);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
  if (in.gcount() != static_cast<std::streamsize>(length)) {
    throw WalError("short read");
  }
  return out;
}

std::array<std::byte, 32> EmptyPayloadChecksum() {
  static const std::array<std::byte, 32> checksum = [] {
    const std::vector<std::byte> empty{};
    return Sha256Digest(empty);
  }();
  return checksum;
}

class PayloadCursor {
 public:
  explicit PayloadCursor(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::size_t remaining() const {
    return bytes_.size() - cursor_;
  }

  [[nodiscard]] std::size_t position() const {
    return cursor_;
  }

  std::uint8_t ReadU8() {
    EnsureAvailable(1, "UInt8");
    return std::to_integer<std::uint8_t>(bytes_[cursor_++]);
  }

  std::uint32_t ReadU32() {
    return ReadIntegral<std::uint32_t>("UInt32");
  }

  std::uint64_t ReadU64() {
    return ReadIntegral<std::uint64_t>("UInt64");
  }

  std::int64_t ReadI64() {
    const auto raw = ReadU64();
    std::int64_t out = 0;
    static_assert(sizeof(out) == sizeof(raw));
    std::memcpy(&out, &raw, sizeof(out));
    return out;
  }

  void Skip(std::size_t count, const char* context) {
    EnsureAvailable(count, context);
    cursor_ += count;
  }

  void SkipBytesLen32(std::size_t max_bytes, const char* context) {
    const auto length = static_cast<std::size_t>(ReadU32());
    if (length > max_bytes) {
      throw WalError(std::string(context) + " exceeds limit");
    }
    Skip(length, context);
  }

  void SkipString(const char* context) {
    SkipBytesLen32(core::mv2s::kMaxStringBytes, context);
  }

  void SkipOptionalString(const char* field) {
    ReadOptional([&]() { SkipString(field); }, field);
  }

  void SkipOptionalU8(const char* field) {
    ReadOptional([&]() { (void)ReadU8(); }, field);
  }

  void SkipOptionalU32(const char* field) {
    ReadOptional([&]() { (void)ReadU32(); }, field);
  }

  void SkipOptionalU64(const char* field) {
    ReadOptional([&]() { (void)ReadU64(); }, field);
  }

  void SkipOptionalBytes(std::size_t max_bytes, const char* field) {
    ReadOptional([&]() { SkipBytesLen32(max_bytes, field); }, field);
  }

  template <typename Fn>
  void ReadOptional(Fn&& reader, const char* field) {
    const auto tag = ReadU8();
    switch (tag) {
      case 0:
        return;
      case 1:
        reader();
        return;
      default:
        throw WalError(std::string("invalid optional tag for ") + field);
    }
  }

  void Finalize() const {
    if (cursor_ != bytes_.size()) {
      throw WalError("excess bytes while decoding WAL entry");
    }
  }

 private:
  template <typename T>
  T ReadIntegral(const char* context) {
    static_assert(std::is_integral_v<T>, "ReadIntegral requires integral type");
    EnsureAvailable(sizeof(T), context);
    using UnsignedT = std::make_unsigned_t<T>;
    UnsignedT out = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      out |= static_cast<UnsignedT>(std::to_integer<std::uint8_t>(bytes_[cursor_ + i])) << (8U * i);
    }
    cursor_ += sizeof(T);
    return static_cast<T>(out);
  }

  void EnsureAvailable(std::size_t count, const char* context) {
    if (count > bytes_.size() || cursor_ > bytes_.size() - count) {
      throw WalError(std::string("truncated buffer while reading ") + context);
    }
  }

  std::span<const std::byte> bytes_;
  std::size_t cursor_ = 0;
};

void SkipStringArray(PayloadCursor& cursor, const char* field) {
  const auto count = static_cast<std::size_t>(cursor.ReadU32());
  if (count > core::mv2s::kMaxArrayCount) {
    throw WalError(std::string(field) + " count exceeds limit");
  }
  for (std::size_t i = 0; i < count; ++i) {
    cursor.SkipString(field);
  }
}

void SkipTagPairs(PayloadCursor& cursor) {
  const auto count = static_cast<std::size_t>(cursor.ReadU32());
  if (count > core::mv2s::kMaxArrayCount) {
    throw WalError("tags count exceeds limit");
  }
  for (std::size_t i = 0; i < count; ++i) {
    cursor.SkipString("tags.key");
    cursor.SkipString("tags.value");
  }
}

void SkipMetadata(PayloadCursor& cursor) {
  const auto count = static_cast<std::size_t>(cursor.ReadU32());
  if (count > core::mv2s::kMaxArrayCount) {
    throw WalError("metadata count exceeds limit");
  }
  for (std::size_t i = 0; i < count; ++i) {
    cursor.SkipString("metadata.key");
    cursor.SkipString("metadata.value");
  }
}

void SkipFrameMetaSubset(PayloadCursor& cursor) {
  cursor.SkipOptionalString("subset.uri");
  cursor.SkipOptionalString("subset.title");
  cursor.SkipOptionalString("subset.kind");
  cursor.SkipOptionalString("subset.track");
  SkipTagPairs(cursor);
  SkipStringArray(cursor, "subset.labels");
  SkipStringArray(cursor, "subset.content_dates");
  cursor.SkipOptionalU8("subset.role");
  cursor.SkipOptionalU64("subset.parent_id");
  cursor.SkipOptionalU32("subset.chunk_index");
  cursor.SkipOptionalU32("subset.chunk_count");
  cursor.SkipOptionalBytes(core::mv2s::kMaxBlobBytes, "subset.chunk_manifest");
  cursor.SkipOptionalU8("subset.status");
  cursor.SkipOptionalU64("subset.supersedes");
  cursor.SkipOptionalU64("subset.superseded_by");
  cursor.SkipOptionalString("subset.search_text");

  const auto metadata_tag = cursor.ReadU8();
  switch (metadata_tag) {
    case 0:
      break;
    case 1:
      SkipMetadata(cursor);
      break;
    default:
      throw WalError("invalid optional tag for subset.metadata");
  }
}

WalPendingMutationInfo DecodeWalMutationPayload(std::uint64_t sequence, std::span<const std::byte> payload) {
  PayloadCursor cursor(payload);
  const auto opcode = cursor.ReadU8();

  WalPendingMutationInfo mutation{};
  mutation.sequence = sequence;
  switch (opcode) {
    case 0x01: {  // putFrame
      mutation.kind = WalMutationKind::kPutFrame;
      WalPutFrameInfo put{};
      put.frame_id = cursor.ReadU64();
      (void)cursor.ReadI64();  // timestampMs
      SkipFrameMetaSubset(cursor);
      put.payload_offset = cursor.ReadU64();
      put.payload_length = cursor.ReadU64();
      const auto canonical_encoding = cursor.ReadU8();
      if (canonical_encoding > 3) {
        throw WalError("invalid canonical encoding in WAL putFrame");
      }
      (void)cursor.ReadU64();              // canonicalLength
      cursor.Skip(32, "canonicalChecksum");
      cursor.Skip(32, "storedChecksum");
      mutation.put_frame = put;
      break;
    }
    case 0x02: {  // deleteFrame
      mutation.kind = WalMutationKind::kDeleteFrame;
      WalDeleteFrameInfo del{};
      del.frame_id = cursor.ReadU64();
      mutation.delete_frame = del;
      break;
    }
    case 0x03: {  // supersedeFrame
      mutation.kind = WalMutationKind::kSupersedeFrame;
      WalSupersedeFrameInfo supersede{};
      supersede.superseded_id = cursor.ReadU64();
      supersede.superseding_id = cursor.ReadU64();
      mutation.supersede_frame = supersede;
      break;
    }
    case 0x04: {  // putEmbedding
      mutation.kind = WalMutationKind::kPutEmbedding;
      WalPutEmbeddingInfo put_embedding{};
      put_embedding.frame_id = cursor.ReadU64();
      const auto dimension = static_cast<std::size_t>(cursor.ReadU32());
      if (dimension > core::mv2s::kMaxArrayCount) {
        throw WalError("embedding dimension exceeds limit");
      }
      if (dimension > std::numeric_limits<std::size_t>::max() / 4) {
        throw WalError("embedding dimension overflows byte length");
      }
      put_embedding.dimension = static_cast<std::uint32_t>(dimension);
      cursor.Skip(dimension * 4, "embedding.vector");
      mutation.put_embedding = put_embedding;
      break;
    }
    default:
      throw WalError("unknown WAL opcode");
  }

  cursor.Finalize();
  return mutation;
}

}  // namespace

bool WalRecordHeader::IsSentinel() const {
  return sequence == 0 && length == 0 && flags == 0 &&
         std::all_of(checksum.begin(), checksum.end(), [](std::byte b) { return b == std::byte{0}; });
}

bool WalRecordHeader::IsPadding() const {
  return (flags & kFlagIsPadding) != 0;
}

WalRecordHeader DecodeWalRecordHeader(std::span<const std::byte> bytes) {
  if (bytes.size() != kRecordHeaderSize) {
    throw WalError("record header size mismatch");
  }
  WalRecordHeader header{};
  header.sequence = ReadLE<std::uint64_t>(bytes, 0);
  header.length = ReadLE<std::uint32_t>(bytes, 8);
  header.flags = ReadLE<std::uint32_t>(bytes, 12);
  std::copy_n(bytes.begin() + 16, header.checksum.size(), header.checksum.begin());
  return header;
}

bool IsTerminalMarker(const std::filesystem::path& path,
                      std::uint64_t wal_offset,
                      std::uint64_t wal_size,
                      std::uint64_t cursor) {
  if (wal_size == 0) {
    return true;
  }
  const auto normalized = cursor % wal_size;
  const auto remaining = wal_size - normalized;
  if (remaining < kRecordHeaderSize) {
    return false;
  }
  try {
    const auto header_bytes = ReadExactly(path, wal_offset + normalized, static_cast<std::size_t>(kRecordHeaderSize));
    const auto header = DecodeWalRecordHeader(header_bytes);
    return header.IsSentinel() || header.sequence == 0;
  } catch (...) {
    return false;
  }
}

WalScanState ScanWalState(const std::filesystem::path& path,
                          std::uint64_t wal_offset,
                          std::uint64_t wal_size,
                          std::uint64_t checkpoint_pos) {
  return ScanPendingMutationsWithState(path,
                                       wal_offset,
                                       wal_size,
                                       checkpoint_pos,
                                       std::numeric_limits<std::uint64_t>::max())
      .state;
}

WalPendingScanResult ScanPendingMutationsWithState(const std::filesystem::path& path,
                                                   std::uint64_t wal_offset,
                                                   std::uint64_t wal_size,
                                                   std::uint64_t checkpoint_pos,
                                                   std::uint64_t committed_seq) {
  if (wal_size == 0) {
    return WalPendingScanResult{};
  }
  if (wal_size < kRecordHeaderSize) {
    throw WalError("wal_size smaller than record header");
  }

  const auto start = checkpoint_pos % wal_size;
  auto cursor = start;
  std::uint64_t last_sequence = 0;
  std::uint64_t pending_bytes = 0;
  bool wrapped = false;
  bool stop_decoding_pending = false;
  std::vector<WalPendingMutationInfo> pending_mutations;

  while (true) {
    const auto remaining = wal_size - cursor;
    if (remaining < kRecordHeaderSize) {
      if (wrapped) {
        break;
      }
      pending_bytes += remaining;
      cursor = 0;
      wrapped = true;
      if (cursor == start) {
        break;
      }
      continue;
    }

    const auto header_bytes = ReadExactly(path, wal_offset + cursor, static_cast<std::size_t>(kRecordHeaderSize));
    WalRecordHeader header{};
    try {
      header = DecodeWalRecordHeader(header_bytes);
    } catch (...) {
      break;
    }

    if (header.IsSentinel() || header.sequence == 0) {
      break;
    }
    if (last_sequence != 0 && header.sequence <= last_sequence) {
      break;
    }

    if (header.IsPadding()) {
      const auto expected = EmptyPayloadChecksum();
      if (!std::equal(expected.begin(), expected.end(), header.checksum.begin())) {
        break;
      }
      const auto skip_bytes = static_cast<std::uint64_t>(header.length);
      if (cursor > std::numeric_limits<std::uint64_t>::max() - (kRecordHeaderSize + skip_bytes)) {
        break;
      }
      const auto advance = kRecordHeaderSize + skip_bytes;
      if (cursor + advance > wal_size) {
        break;
      }
      cursor = (cursor + advance) % wal_size;
      pending_bytes += advance;
      last_sequence = header.sequence;
      if (cursor == 0) {
        wrapped = true;
      }
      if (cursor == start) {
        break;
      }
      continue;
    }

    const auto payload_len = static_cast<std::uint64_t>(header.length);
    if (payload_len == 0) {
      break;
    }

    const auto max_payload = wal_size >= kRecordHeaderSize ? wal_size - kRecordHeaderSize : 0;
    if (payload_len > max_payload) {
      break;
    }
    if (payload_len > remaining - kRecordHeaderSize) {
      break;
    }
    if (payload_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      break;
    }

    const auto payload = ReadExactly(path, wal_offset + cursor + kRecordHeaderSize, static_cast<std::size_t>(payload_len));
    const auto computed = Sha256Digest(payload);
    if (!std::equal(computed.begin(), computed.end(), header.checksum.begin())) {
      break;
    }

    if (!stop_decoding_pending && header.sequence > committed_seq) {
      try {
        pending_mutations.push_back(DecodeWalMutationPayload(header.sequence, payload));
      } catch (...) {
        // Preserve Swift open-path behavior: continue state scan even if entry decode fails.
        stop_decoding_pending = true;
      }
    }

    const auto advance = kRecordHeaderSize + payload_len;
    cursor += advance;
    if (cursor == wal_size) {
      cursor = 0;
      wrapped = true;
    }
    pending_bytes += advance;
    last_sequence = header.sequence;
    if (cursor == start) {
      break;
    }
  }

  WalPendingScanResult result{};
  result.pending_mutations = std::move(pending_mutations);
  result.state.last_sequence = last_sequence;
  result.state.write_pos = cursor;
  result.state.pending_bytes = pending_bytes;
  return result;
}

}  // namespace waxcpp::core::wal
