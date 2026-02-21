#include "waxcpp/wax_store.hpp"

#include "mv2s_format.hpp"
#include "sha256.hpp"
#include "wal_ring.hpp"
#include "wax_store_test_hooks.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace waxcpp {
namespace {

using core::mv2s::Footer;
using core::mv2s::FooterSlice;
using core::mv2s::HeaderPage;
std::atomic<std::uint32_t> g_test_commit_fail_step{0};

std::runtime_error StoreError(const std::string& message) {
  return std::runtime_error("wax_store: " + message);
}

void MaybeInjectCommitCrash(std::uint32_t step) {
  const auto requested_step = g_test_commit_fail_step.load(std::memory_order_relaxed);
  if (requested_step == 0) {
    return;
  }
  if (requested_step == step) {
    throw StoreError("injected crash-window failure at commit step " + std::to_string(step));
  }
}

std::uint64_t FileSize(const std::filesystem::path& path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    throw StoreError("failed to read file size: " + ec.message());
  }
  return size;
}

std::vector<std::byte> ReadExactly(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  if (length == 0) {
    return {};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw StoreError("failed to open file for read: " + path.string());
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw StoreError("failed to seek for read");
  }
  std::vector<std::byte> out(length);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
  if (in.gcount() != static_cast<std::streamsize>(length)) {
    throw StoreError("short read");
  }
  return out;
}

void WriteAt(std::ofstream& out, std::uint64_t offset, std::span<const std::byte> bytes) {
  out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!out) {
    throw StoreError("failed to seek for write");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw StoreError("failed to write bytes");
  }
}

void WriteBytesAt(const std::filesystem::path& path, std::uint64_t offset, std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return;
  }
  std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!out) {
    std::ofstream create(path, std::ios::binary | std::ios::trunc);
    if (!create) {
      throw StoreError("failed to create file for write: " + path.string());
    }
    create.close();
    out.open(path, std::ios::binary | std::ios::in | std::ios::out);
  }
  if (!out) {
    throw StoreError("failed to open file for write: " + path.string());
  }
  out.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!out) {
    throw StoreError("failed to seek for write");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw StoreError("failed to write bytes");
  }
}

void ResizeFile(const std::filesystem::path& path, std::uint64_t size) {
  std::error_code ec;
  std::filesystem::resize_file(path, size, ec);
  if (ec) {
    throw StoreError("failed to resize file: " + ec.message());
  }
}

void AppendU8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void AppendLE32(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendLE64(std::vector<std::byte>& out, std::uint64_t value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendFixed(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

std::vector<std::byte> BuildWalPutFramePayload(std::uint64_t frame_id,
                                               std::uint64_t payload_offset,
                                               std::uint64_t payload_length,
                                               std::uint8_t canonical_encoding,
                                               std::uint64_t canonical_length,
                                               std::span<const std::byte, 32> canonical_checksum,
                                               std::span<const std::byte, 32> stored_checksum) {
  std::vector<std::byte> payload{};
  payload.reserve(256);
  AppendU8(payload, 0x01);  // putFrame
  AppendLE64(payload, frame_id);
  AppendLE64(payload, 0);   // timestampMs

  // FrameMetaSubset optional fields (none set yet in C++ port).
  AppendU8(payload, 0);   // uri?
  AppendU8(payload, 0);   // title?
  AppendU8(payload, 0);   // kind?
  AppendU8(payload, 0);   // track?
  AppendLE32(payload, 0); // tags.count
  AppendLE32(payload, 0); // labels.count
  AppendLE32(payload, 0); // contentDates.count
  AppendU8(payload, 0);   // role?
  AppendU8(payload, 0);   // parentId?
  AppendU8(payload, 0);   // chunkIndex?
  AppendU8(payload, 0);   // chunkCount?
  AppendU8(payload, 0);   // chunkManifest?
  AppendU8(payload, 0);   // status?
  AppendU8(payload, 0);   // supersedes?
  AppendU8(payload, 0);   // supersededBy?
  AppendU8(payload, 0);   // searchText?
  AppendU8(payload, 0);   // metadata?

  AppendLE64(payload, payload_offset);
  AppendLE64(payload, payload_length);
  AppendU8(payload, canonical_encoding);
  AppendLE64(payload, canonical_length);
  AppendFixed(payload, canonical_checksum);
  AppendFixed(payload, stored_checksum);
  return payload;
}

std::vector<std::byte> BuildWalDeletePayload(std::uint64_t frame_id) {
  std::vector<std::byte> payload{};
  payload.reserve(1 + 8);
  AppendU8(payload, 0x02);  // deleteFrame
  AppendLE64(payload, frame_id);
  return payload;
}

std::vector<std::byte> BuildWalSupersedePayload(std::uint64_t superseded_id, std::uint64_t superseding_id) {
  std::vector<std::byte> payload{};
  payload.reserve(1 + 8 + 8);
  AppendU8(payload, 0x03);  // supersedeFrame
  AppendLE64(payload, superseded_id);
  AppendLE64(payload, superseding_id);
  return payload;
}

std::optional<HeaderPage> TryDecodeHeader(const std::filesystem::path& path, std::uint64_t offset) {
  try {
    const auto bytes = ReadExactly(path, offset, static_cast<std::size_t>(core::mv2s::kHeaderPageSize));
    return core::mv2s::DecodeHeaderPage(bytes);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<FooterSlice> TryReadFooterAt(const std::filesystem::path& path,
                                           std::uint64_t file_size,
                                           std::uint64_t footer_offset) {
  try {
    if (footer_offset + core::mv2s::kFooterSize > file_size) {
      return std::nullopt;
    }
    const auto footer_bytes = ReadExactly(path, footer_offset, static_cast<std::size_t>(core::mv2s::kFooterSize));
    const Footer footer = core::mv2s::DecodeFooter(footer_bytes);
    if (footer.toc_len < 32 || footer.toc_len > core::mv2s::kMaxTocBytes || footer.toc_len > footer_offset) {
      return std::nullopt;
    }
    if (footer.toc_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      return std::nullopt;
    }
    const auto toc_offset = footer_offset - footer.toc_len;
    const auto toc_bytes = ReadExactly(path, toc_offset, static_cast<std::size_t>(footer.toc_len));
    if (!core::mv2s::TocHashMatches(toc_bytes, footer.toc_hash)) {
      return std::nullopt;
    }
    return FooterSlice{
        .footer_offset = footer_offset,
        .toc_offset = toc_offset,
        .footer = footer,
        .toc_bytes = std::move(toc_bytes),
    };
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<FooterSlice> ScanForLatestFooter(const std::filesystem::path& path, std::uint64_t file_size) {
  if (file_size < core::mv2s::kFooterSize) {
    return std::nullopt;
  }
  const auto scan_start = file_size > core::mv2s::kMaxFooterScanBytes ? file_size - core::mv2s::kMaxFooterScanBytes : 0;
  const auto scan_len = file_size - scan_start;
  if (scan_len > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw StoreError("scan window too large for memory");
  }

  const auto window = ReadExactly(path, scan_start, static_cast<std::size_t>(scan_len));
  std::optional<FooterSlice> best;

  if (window.size() < core::mv2s::kFooterSize) {
    return std::nullopt;
  }

  const auto last = window.size() - static_cast<std::size_t>(core::mv2s::kFooterSize);
  for (std::size_t pos = last + 1; pos-- > 0;) {
    if (window[pos] != core::mv2s::kFooterMagic[0]) {
      continue;
    }
    const std::span<const std::byte> possible_magic(window.data() + static_cast<std::ptrdiff_t>(pos),
                                                     core::mv2s::kFooterMagic.size());
    if (!std::equal(core::mv2s::kFooterMagic.begin(), core::mv2s::kFooterMagic.end(), possible_magic.begin())) {
      continue;
    }

    const auto footer_offset = scan_start + pos;
    const auto candidate = TryReadFooterAt(path, file_size, footer_offset);
    if (!candidate.has_value()) {
      continue;
    }
    if (!best.has_value()) {
      best = candidate;
      continue;
    }
    if (candidate->footer.generation > best->footer.generation ||
        (candidate->footer.generation == best->footer.generation &&
         candidate->footer_offset > best->footer_offset)) {
      best = candidate;
    }
  }
  return best;
}

std::optional<FooterSlice> SelectPreferredFooter(std::optional<FooterSlice> from_header,
                                                 std::optional<FooterSlice> from_scan) {
  if (!from_header.has_value()) {
    return from_scan;
  }
  if (!from_scan.has_value()) {
    return from_header;
  }
  const auto& header = *from_header;
  const auto& scan = *from_scan;
  if (scan.footer.generation > header.footer.generation) {
    return from_scan;
  }
  if (scan.footer.generation == header.footer.generation &&
      scan.footer_offset > header.footer_offset) {
    return from_scan;
  }
  return from_header;
}

std::array<std::byte, 32> ComputePayloadHash(const std::filesystem::path& path,
                                             std::uint64_t offset,
                                             std::uint64_t length) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw StoreError("failed to open payload for hash");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw StoreError("failed to seek payload for hash");
  }

  constexpr std::size_t kBufferSize = 1U << 20U;
  std::vector<std::byte> buffer(kBufferSize);
  core::Sha256 hasher;

  std::uint64_t remaining = length;
  while (remaining > 0) {
    const auto chunk_size = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, kBufferSize));
    in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(chunk_size));
    if (in.gcount() != static_cast<std::streamsize>(chunk_size)) {
      throw StoreError("short read while hashing payload");
    }
    hasher.Update(std::span<const std::byte>(buffer.data(), chunk_size));
    remaining -= chunk_size;
  }

  return hasher.Finalize();
}

void DeepVerifyFrames(const std::filesystem::path& path, const std::vector<core::mv2s::FrameSummary>& frames) {
  for (const auto& frame : frames) {
    if (frame.payload_length == 0) {
      continue;
    }
    if (!frame.stored_checksum.has_value()) {
      throw StoreError("frame missing stored checksum");
    }
    const auto computed = ComputePayloadHash(path, frame.payload_offset, frame.payload_length);
    if (!std::equal(computed.begin(), computed.end(), frame.stored_checksum->begin())) {
      throw StoreError("frame stored checksum mismatch");
    }
    // Canonical checksum equality is guaranteed for plain payloads.
    // Compressed canonical checksum verification requires decompression support (tracked post-M2).
    if (frame.canonical_encoding == 0 &&
        !std::equal(computed.begin(), computed.end(), frame.payload_checksum.begin())) {
      throw StoreError("frame canonical checksum mismatch");
    }
  }
}

void DeepVerifySegments(const std::filesystem::path& path, const std::vector<core::mv2s::SegmentSummary>& segments) {
  for (const auto& segment : segments) {
    if (segment.bytes_length == 0) {
      continue;
    }
    const auto computed = ComputePayloadHash(path, segment.bytes_offset, segment.bytes_length);
    if (!std::equal(computed.begin(), computed.end(), segment.checksum.begin())) {
      throw StoreError("segment checksum mismatch");
    }
  }
}

void ValidateDataRanges(const std::vector<core::mv2s::FrameSummary>& frames,
                        const std::vector<core::mv2s::SegmentSummary>& segments,
                        std::uint64_t data_start,
                        std::uint64_t data_end) {
  struct Range {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool is_frame = true;
  };
  std::vector<Range> ranges{};
  ranges.reserve(frames.size() + segments.size());

  for (const auto& frame : frames) {
    if (frame.payload_length == 0) {
      continue;
    }
    if (frame.payload_offset < data_start) {
      throw StoreError("frame payload below data region");
    }
    if (frame.payload_offset > std::numeric_limits<std::uint64_t>::max() - frame.payload_length) {
      throw StoreError("frame payload range overflow");
    }
    const auto end = frame.payload_offset + frame.payload_length;
    if (end > data_end) {
      throw StoreError("frame payload exceeds committed data end");
    }
    ranges.push_back({.start = frame.payload_offset, .end = end, .is_frame = true});
  }

  for (const auto& segment : segments) {
    if (segment.bytes_length == 0) {
      continue;
    }
    if (segment.bytes_offset < data_start) {
      throw StoreError("segment below data region");
    }
    if (segment.bytes_offset > std::numeric_limits<std::uint64_t>::max() - segment.bytes_length) {
      throw StoreError("segment range overflow");
    }
    const auto end = segment.bytes_offset + segment.bytes_length;
    if (end > data_end) {
      throw StoreError("segment exceeds committed data end");
    }
    ranges.push_back({.start = segment.bytes_offset, .end = end, .is_frame = false});
  }

  std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.start < rhs.start;
  });
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i - 1].end > ranges[i].start) {
      if (ranges[i - 1].is_frame && ranges[i].is_frame) {
        throw StoreError("overlapping frame payload ranges");
      }
      if (!ranges[i - 1].is_frame && !ranges[i].is_frame) {
        throw StoreError("overlapping segment ranges");
      }
      throw StoreError("overlap between frame payload and segment range");
    }
  }
}

bool WouldCreateSupersedeCycle(const std::vector<core::mv2s::FrameSummary>& frames,
                               std::uint64_t superseded_id,
                               std::uint64_t superseding_id) {
  std::uint64_t cursor = superseded_id;
  for (std::size_t hops = 0; hops < frames.size(); ++hops) {
    const auto& frame = frames[static_cast<std::size_t>(cursor)];
    if (!frame.supersedes.has_value()) {
      return false;
    }
    cursor = *frame.supersedes;
    if (cursor == superseding_id) {
      return true;
    }
    if (cursor >= frames.size()) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace core::testing {

void SetCommitFailStep(std::uint32_t step) {
  g_test_commit_fail_step.store(step, std::memory_order_relaxed);
}

void ClearCommitFailStep() {
  g_test_commit_fail_step.store(0, std::memory_order_relaxed);
}

}  // namespace core::testing

WaxStore WaxStore::Create(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
      throw StoreError("failed to create parent directories: " + ec.message());
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw StoreError("failed to create file: " + path.string());
  }

  auto toc_bytes = core::mv2s::EncodeEmptyTocV1();
  std::array<std::byte, 32> toc_checksum{};
  std::copy(toc_bytes.end() - 32, toc_bytes.end(), toc_checksum.begin());

  const std::uint64_t toc_offset = core::mv2s::kWalOffset + core::mv2s::kDefaultWalSize;
  const std::uint64_t footer_offset = toc_offset + toc_bytes.size();

  Footer footer;
  footer.toc_len = toc_bytes.size();
  footer.toc_hash = toc_checksum;
  footer.generation = 0;
  footer.wal_committed_seq = 0;
  const auto footer_bytes = core::mv2s::EncodeFooter(footer);

  HeaderPage page_a;
  page_a.header_page_generation = 1;
  page_a.file_generation = 0;
  page_a.footer_offset = footer_offset;
  page_a.wal_offset = core::mv2s::kWalOffset;
  page_a.wal_size = core::mv2s::kDefaultWalSize;
  page_a.wal_write_pos = 0;
  page_a.wal_checkpoint_pos = 0;
  page_a.wal_committed_seq = 0;
  page_a.toc_checksum = toc_checksum;
  const auto page_a_bytes = core::mv2s::EncodeHeaderPage(page_a);

  HeaderPage page_b = page_a;
  page_b.header_page_generation = 0;
  const auto page_b_bytes = core::mv2s::EncodeHeaderPage(page_b);

  WriteAt(out, 0, page_a_bytes);
  WriteAt(out, core::mv2s::kHeaderPageSize, page_b_bytes);
  WriteAt(out, toc_offset, toc_bytes);
  WriteAt(out, footer_offset, footer_bytes);
  out.flush();
  if (!out) {
    throw StoreError("flush failed");
  }

  return Open(path, true);
}

WaxStore WaxStore::Open(const std::filesystem::path& path) {
  return Open(path, true);
}

WaxStore WaxStore::Open(const std::filesystem::path& path, bool repair) {
  WaxStore store(path);
  store.LoadState(false, repair);
  return store;
}

void WaxStore::Verify(bool deep) {
  LoadState(deep, false);
}

std::uint64_t WaxStore::Put(const std::vector<std::byte>& content, const Metadata& /*metadata*/) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }

  const auto frame_id = next_frame_id_;
  const auto payload_offset = FileSize(path_);
  const auto payload_length = static_cast<std::uint64_t>(content.size());
  const auto stored_checksum = core::Sha256Digest(content);
  const auto canonical_checksum = stored_checksum;
  const std::uint8_t canonical_encoding = 0;  // plain
  const auto canonical_length = payload_length;

  if (!content.empty()) {
    WriteBytesAt(path_, payload_offset, content);
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  const auto wal_payload = BuildWalPutFramePayload(frame_id,
                                                   payload_offset,
                                                   payload_length,
                                                   canonical_encoding,
                                                   canonical_length,
                                                   canonical_checksum,
                                                   stored_checksum);
  (void)writer.Append(wal_payload);

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();

  stats_.pending_frames += 1;
  next_frame_id_ = frame_id + 1;
  dirty_ = true;
  has_local_mutations_ = true;
  return frame_id;
}

std::vector<std::uint64_t> WaxStore::PutBatch(const std::vector<std::vector<std::byte>>& contents,
                                              const std::vector<Metadata>& metadatas) {
  if (!metadatas.empty() && metadatas.size() != contents.size()) {
    throw StoreError("PutBatch metadatas size must be zero or match contents size");
  }
  std::vector<std::uint64_t> ids{};
  ids.reserve(contents.size());
  for (std::size_t i = 0; i < contents.size(); ++i) {
    const Metadata* metadata = metadatas.empty() ? nullptr : &metadatas[i];
    ids.push_back(Put(contents[i], metadata ? *metadata : Metadata{}));
  }
  return ids;
}

void WaxStore::Delete(std::uint64_t frame_id) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (frame_id >= next_frame_id_) {
    throw StoreError("delete frame_id out of range");
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  const auto wal_payload = BuildWalDeletePayload(frame_id);
  (void)writer.Append(wal_payload);

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  dirty_ = true;
  has_local_mutations_ = true;
}

void WaxStore::Supersede(std::uint64_t superseded_id, std::uint64_t superseding_id) {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (superseded_id == superseding_id) {
    throw StoreError("supersede self-reference is not allowed");
  }
  if (superseded_id >= next_frame_id_ || superseding_id >= next_frame_id_) {
    throw StoreError("supersede frame_id out of range");
  }

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  wal_write_pos_,
                                  wal_checkpoint_pos_,
                                  wal_pending_bytes_,
                                  wal_last_sequence_,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  const auto wal_payload = BuildWalSupersedePayload(superseded_id, superseding_id);
  (void)writer.Append(wal_payload);

  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  dirty_ = true;
  has_local_mutations_ = true;
}

void WaxStore::Commit() {
  if (!is_open_) {
    throw StoreError("store is closed");
  }
  if (!dirty_) {
    return;
  }

  const auto file_size = FileSize(path_);
  const auto footer_slice = TryReadFooterAt(path_, file_size, footer_offset_);
  if (!footer_slice.has_value()) {
    throw StoreError("current footer is missing or invalid");
  }
  auto toc_summary = core::mv2s::DecodeToc(footer_slice->toc_bytes);
  auto frames = toc_summary.frames;

  auto pending_scan = core::wal::ScanPendingMutationsWithState(path_,
                                                                wal_offset_,
                                                                wal_size_,
                                                                wal_checkpoint_pos_,
                                                                wal_committed_seq_);
  for (const auto& mutation : pending_scan.pending_mutations) {
    switch (mutation.kind) {
      case core::wal::WalMutationKind::kPutFrame: {
        if (!mutation.put_frame.has_value()) {
          throw StoreError("wal putFrame mutation missing payload");
        }
        const auto& put = *mutation.put_frame;
        if (put.frame_id != frames.size()) {
          throw StoreError("wal putFrame frame_id is not dense");
        }
        core::mv2s::FrameSummary frame{};
        frame.id = put.frame_id;
        frame.payload_offset = put.payload_offset;
        frame.payload_length = put.payload_length;
        frame.payload_checksum = put.canonical_checksum;
        frame.canonical_encoding = put.canonical_encoding;
        if (put.canonical_encoding != 0) {
          frame.canonical_length = put.canonical_length;
        }
        if (put.payload_length > 0) {
          frame.stored_checksum = put.stored_checksum;
        }
        frame.status = 0;
        frames.push_back(frame);
        break;
      }
      case core::wal::WalMutationKind::kDeleteFrame: {
        if (!mutation.delete_frame.has_value()) {
          throw StoreError("wal delete mutation missing payload");
        }
        const auto frame_id = mutation.delete_frame->frame_id;
        if (frame_id >= frames.size()) {
          throw StoreError("wal delete references unknown frame_id");
        }
        frames[static_cast<std::size_t>(frame_id)].status = 1;
        break;
      }
      case core::wal::WalMutationKind::kSupersedeFrame: {
        if (!mutation.supersede_frame.has_value()) {
          throw StoreError("wal supersede mutation missing payload");
        }
        const auto superseded_id = mutation.supersede_frame->superseded_id;
        const auto superseding_id = mutation.supersede_frame->superseding_id;
        if (superseded_id >= frames.size() || superseding_id >= frames.size()) {
          throw StoreError("wal supersede references unknown frame_id");
        }
        if (superseded_id == superseding_id) {
          throw StoreError("wal supersede self-reference");
        }
        auto& superseded = frames[static_cast<std::size_t>(superseded_id)];
        auto& superseding = frames[static_cast<std::size_t>(superseding_id)];
        if (superseded.superseded_by.has_value() && *superseded.superseded_by != superseding_id) {
          throw StoreError("wal supersede conflict: superseded frame already has different superseding frame");
        }
        if (superseding.supersedes.has_value() && *superseding.supersedes != superseded_id) {
          throw StoreError("wal supersede conflict: superseding frame already supersedes different frame");
        }
        if (WouldCreateSupersedeCycle(frames, superseded_id, superseding_id)) {
          throw StoreError("wal supersede cycle detected");
        }
        superseded.superseded_by = superseding_id;
        superseding.supersedes = superseded_id;
        break;
      }
      case core::wal::WalMutationKind::kPutEmbedding:
        // M3/M4 scope: embedding WAL mutation is accepted in scan state, apply path is deferred.
        break;
    }
  }

  const auto toc_bytes = core::mv2s::EncodeTocV1(frames);
  std::uint64_t data_end = wal_offset_ + wal_size_;
  for (const auto& frame : frames) {
    if (frame.payload_length == 0) {
      continue;
    }
    if (frame.payload_offset > std::numeric_limits<std::uint64_t>::max() - frame.payload_length) {
      throw StoreError("frame payload range overflow during commit");
    }
    const auto frame_end = frame.payload_offset + frame.payload_length;
    if (frame_end > data_end) {
      data_end = frame_end;
    }
  }

  const auto toc_offset = data_end;
  const auto footer_offset = toc_offset + toc_bytes.size();
  Footer footer{};
  footer.toc_len = toc_bytes.size();
  std::copy(toc_bytes.end() - 32, toc_bytes.end(), footer.toc_hash.begin());
  footer.generation = file_generation_ + 1;
  footer.wal_committed_seq = pending_scan.state.last_sequence;
  const auto footer_bytes = core::mv2s::EncodeFooter(footer);

  WriteBytesAt(path_, toc_offset, toc_bytes);
  MaybeInjectCommitCrash(1);
  WriteBytesAt(path_, footer_offset, footer_bytes);
  ResizeFile(path_, footer_offset + core::mv2s::kFooterSize);
  MaybeInjectCommitCrash(2);

  core::wal::WalRingWriter writer(path_,
                                  wal_offset_,
                                  wal_size_,
                                  pending_scan.state.write_pos,
                                  wal_checkpoint_pos_,
                                  pending_scan.state.pending_bytes,
                                  pending_scan.state.last_sequence,
                                  wal_wrap_count_,
                                  wal_checkpoint_count_,
                                  wal_sentinel_write_count_,
                                  wal_write_call_count_);
  writer.RecordCheckpoint();

  HeaderPage page_a{};
  page_a.header_page_generation = header_page_generation_ + 1;
  page_a.file_generation = footer.generation;
  page_a.footer_offset = footer_offset;
  page_a.wal_offset = wal_offset_;
  page_a.wal_size = wal_size_;
  page_a.wal_write_pos = writer.write_pos();
  page_a.wal_checkpoint_pos = writer.checkpoint_pos();
  page_a.wal_committed_seq = footer.wal_committed_seq;
  page_a.toc_checksum = footer.toc_hash;
  page_a.replay_snapshot = core::mv2s::ReplaySnapshot{
      .file_generation = footer.generation,
      .wal_committed_seq = footer.wal_committed_seq,
      .footer_offset = footer_offset,
      .wal_write_pos = writer.write_pos(),
      .wal_checkpoint_pos = writer.checkpoint_pos(),
      .wal_pending_bytes = writer.pending_bytes(),
      .wal_last_sequence = writer.last_sequence(),
  };

  auto page_b = page_a;
  page_b.header_page_generation = header_page_generation_;
  const auto page_a_bytes = core::mv2s::EncodeHeaderPage(page_a);
  const auto page_b_bytes = core::mv2s::EncodeHeaderPage(page_b);
  WriteBytesAt(path_, 0, page_a_bytes);
  MaybeInjectCommitCrash(3);
  WriteBytesAt(path_, core::mv2s::kHeaderPageSize, page_b_bytes);
  MaybeInjectCommitCrash(4);

  file_generation_ = footer.generation;
  header_page_generation_ = page_a.header_page_generation;
  wal_committed_seq_ = footer.wal_committed_seq;
  wal_write_pos_ = writer.write_pos();
  wal_checkpoint_pos_ = writer.checkpoint_pos();
  wal_pending_bytes_ = writer.pending_bytes();
  wal_last_sequence_ = writer.last_sequence();
  wal_wrap_count_ = writer.wrap_count();
  wal_checkpoint_count_ = writer.checkpoint_count();
  wal_sentinel_write_count_ = writer.sentinel_write_count();
  wal_write_call_count_ = writer.write_call_count();
  footer_offset_ = footer_offset;
  dirty_ = false;
  has_local_mutations_ = false;

  stats_.generation = file_generation_;
  stats_.frame_count = frames.size();
  stats_.pending_frames = 0;
  next_frame_id_ = frames.size();
}

void WaxStore::Close() {
  if (is_open_ && dirty_ && has_local_mutations_) {
    Commit();
  }
  is_open_ = false;
}

WaxStats WaxStore::Stats() const {
  return stats_;
}

WaxWALStats WaxStore::WalStats() const {
  WaxWALStats stats{};
  stats.wal_size = wal_size_;
  stats.write_pos = wal_write_pos_;
  stats.checkpoint_pos = wal_checkpoint_pos_;
  stats.pending_bytes = wal_pending_bytes_;
  stats.committed_seq = wal_committed_seq_;
  stats.last_seq = wal_last_sequence_;
  stats.wrap_count = wal_wrap_count_;
  stats.checkpoint_count = wal_checkpoint_count_;
  stats.sentinel_write_count = wal_sentinel_write_count_;
  stats.write_call_count = wal_write_call_count_;
  stats.replay_snapshot_hit_count = wal_replay_snapshot_hit_count_;
  return stats;
}

WaxStore::WaxStore(std::filesystem::path path) : path_(std::move(path)) {}

void WaxStore::LoadState(bool deep_verify, bool repair_trailing_bytes) {
  if (!std::filesystem::exists(path_)) {
    throw StoreError("store file does not exist: " + path_.string());
  }
  auto file_size = FileSize(path_);
  if (file_size < core::mv2s::kHeaderRegionSize + core::mv2s::kFooterSize) {
    throw StoreError("file is too small to be a valid mv2s store");
  }

  const auto page_a = TryDecodeHeader(path_, 0);
  const auto page_b = TryDecodeHeader(path_, core::mv2s::kHeaderPageSize);
  if (!page_a.has_value() && !page_b.has_value()) {
    throw StoreError("no valid header pages");
  }

  HeaderPage selected{};
  if (page_a.has_value() && page_b.has_value()) {
    selected = page_a->header_page_generation >= page_b->header_page_generation ? *page_a : *page_b;
  } else if (page_a.has_value()) {
    selected = *page_a;
  } else {
    selected = *page_b;
  }

  const auto footer_from_header = TryReadFooterAt(path_, file_size, selected.footer_offset);
  std::optional<FooterSlice> footer_from_snapshot;
  if (selected.replay_snapshot.has_value()) {
    footer_from_snapshot = TryReadFooterAt(path_, file_size, selected.replay_snapshot->footer_offset);
  }
  const auto footer_from_scan = ScanForLatestFooter(path_, file_size);
  auto footer_slice = SelectPreferredFooter(footer_from_header, footer_from_snapshot);
  footer_slice = SelectPreferredFooter(footer_slice, footer_from_scan);
  if (!footer_slice.has_value()) {
    throw StoreError("no valid footer slice found");
  }

  const auto toc_summary = core::mv2s::DecodeToc(footer_slice->toc_bytes);
  const auto data_start = selected.wal_offset + selected.wal_size;
  const auto data_end = footer_slice->footer_offset;
  ValidateDataRanges(toc_summary.frames, toc_summary.segments, data_start, data_end);
  if (deep_verify) {
    DeepVerifyFrames(path_, toc_summary.frames);
    DeepVerifySegments(path_, toc_summary.segments);
  }

  const auto committed_seq = footer_slice->footer.wal_committed_seq;
  const auto selected_header_was_stale = selected.file_generation != footer_slice->footer.generation;
  bool used_replay_snapshot = false;
  std::vector<core::wal::WalPendingMutationInfo> pending_mutations{};
  core::wal::WalScanState wal_scan_state{};

  try {
    const auto replay_snapshot_matches_footer = selected.replay_snapshot.has_value() &&
                                                selected.replay_snapshot->file_generation == footer_slice->footer.generation &&
                                                selected.replay_snapshot->wal_committed_seq == committed_seq &&
                                                selected.replay_snapshot->footer_offset == footer_slice->footer_offset;

    if (replay_snapshot_matches_footer &&
        selected.replay_snapshot->wal_checkpoint_pos == selected.replay_snapshot->wal_write_pos &&
        core::wal::IsTerminalMarker(path_,
                                    selected.wal_offset,
                                    selected.wal_size,
                                    selected.replay_snapshot->wal_write_pos)) {
      used_replay_snapshot = true;
      wal_scan_state.last_sequence = std::max(committed_seq, selected.replay_snapshot->wal_last_sequence);
      wal_scan_state.write_pos = selected.replay_snapshot->wal_write_pos % selected.wal_size;
      wal_scan_state.pending_bytes = 0;
    } else if (!selected_header_was_stale &&
               selected.wal_checkpoint_pos == selected.wal_write_pos &&
               core::wal::IsTerminalMarker(path_,
                                           selected.wal_offset,
                                           selected.wal_size,
                                           selected.wal_write_pos)) {
      used_replay_snapshot = true;
      wal_scan_state.last_sequence = committed_seq;
      wal_scan_state.write_pos = selected.wal_write_pos % selected.wal_size;
      wal_scan_state.pending_bytes = 0;
    } else {
      auto pending_scan = core::wal::ScanPendingMutationsWithState(path_,
                                                                   selected.wal_offset,
                                                                   selected.wal_size,
                                                                   selected.wal_checkpoint_pos,
                                                                   committed_seq);
      wal_scan_state = pending_scan.state;
      pending_mutations = std::move(pending_scan.pending_mutations);
    }
  } catch (const std::exception& ex) {
    throw StoreError(std::string("wal scan failed: ") + ex.what());
  }

  const auto last_sequence = std::max(committed_seq, wal_scan_state.last_sequence);
  std::uint64_t effective_checkpoint_pos = 0;
  std::uint64_t effective_pending_bytes = 0;
  if (wal_scan_state.last_sequence <= committed_seq) {
    effective_checkpoint_pos = wal_scan_state.write_pos;
    effective_pending_bytes = 0;
  } else {
    effective_checkpoint_pos = selected.wal_checkpoint_pos % selected.wal_size;
    effective_pending_bytes = wal_scan_state.pending_bytes;
  }

  std::uint64_t required_end = footer_slice->footer_offset + core::mv2s::kFooterSize;
  std::uint64_t pending_put_frames = 0;
  std::uint64_t pending_max_frame_id_plus_one = toc_summary.frames.size();
  for (const auto& mutation : pending_mutations) {
    if (!mutation.put_frame.has_value()) {
      continue;
    }
    ++pending_put_frames;
    const auto& put = *mutation.put_frame;
    if (put.frame_id == std::numeric_limits<std::uint64_t>::max()) {
      throw StoreError("pending WAL putFrame frame_id overflow");
    }
    const auto put_next = put.frame_id + 1;
    if (put_next > pending_max_frame_id_plus_one) {
      pending_max_frame_id_plus_one = put_next;
    }
    if (put.payload_offset > std::numeric_limits<std::uint64_t>::max() - put.payload_length) {
      throw StoreError("pending WAL putFrame payload range overflow");
    }
    const auto end = put.payload_offset + put.payload_length;
    if (end > required_end) {
      required_end = end;
    }
  }
  if (required_end > file_size) {
    throw StoreError("pending WAL references bytes beyond file size");
  }
  if (repair_trailing_bytes && file_size > required_end) {
    std::error_code ec;
    std::filesystem::resize_file(path_, required_end, ec);
    if (ec) {
      throw StoreError("failed to truncate trailing bytes: " + ec.message());
    }
    file_size = required_end;
  }

  file_generation_ = footer_slice->footer.generation;
  header_page_generation_ = selected.header_page_generation;
  wal_offset_ = selected.wal_offset;
  wal_size_ = selected.wal_size;
  wal_committed_seq_ = committed_seq;
  wal_write_pos_ = wal_scan_state.write_pos;
  wal_checkpoint_pos_ = effective_checkpoint_pos;
  wal_pending_bytes_ = effective_pending_bytes;
  wal_last_sequence_ = last_sequence;
  wal_wrap_count_ = 0;
  wal_checkpoint_count_ = 0;
  wal_sentinel_write_count_ = 0;
  wal_write_call_count_ = 0;
  wal_replay_snapshot_hit_count_ = used_replay_snapshot ? 1U : 0U;
  footer_offset_ = footer_slice->footer_offset;
  next_frame_id_ = pending_max_frame_id_plus_one;
  dirty_ = wal_scan_state.last_sequence > committed_seq;
  has_local_mutations_ = false;
  is_open_ = true;
  (void)file_size;
  (void)used_replay_snapshot;

  stats_.generation = file_generation_;
  stats_.frame_count = toc_summary.frame_count;
  stats_.pending_frames = pending_put_frames;
}

}  // namespace waxcpp
