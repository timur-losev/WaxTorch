#include "waxcpp/wax_store.hpp"

#include "mv2s_format.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>
#include <stdexcept>

namespace waxcpp {
namespace {

using core::mv2s::Footer;
using core::mv2s::FooterSlice;
using core::mv2s::HeaderPage;

std::runtime_error StoreError(const std::string& message) {
  return std::runtime_error("wax_store: " + message);
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

}  // namespace

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

  return Open(path);
}

WaxStore WaxStore::Open(const std::filesystem::path& path) {
  WaxStore store(path);
  store.LoadState(false);
  return store;
}

void WaxStore::Verify(bool deep) {
  LoadState(deep);
}

std::uint64_t WaxStore::Put(const std::vector<std::byte>& /*content*/, const Metadata& /*metadata*/) {
  throw std::runtime_error("WaxStore::Put not implemented");
}

std::vector<std::uint64_t> WaxStore::PutBatch(const std::vector<std::vector<std::byte>>& /*contents*/,
                                              const std::vector<Metadata>& /*metadatas*/) {
  throw std::runtime_error("WaxStore::PutBatch not implemented");
}

void WaxStore::Delete(std::uint64_t /*frame_id*/) {
  throw std::runtime_error("WaxStore::Delete not implemented");
}

void WaxStore::Supersede(std::uint64_t /*superseded_id*/, std::uint64_t /*superseding_id*/) {
  throw std::runtime_error("WaxStore::Supersede not implemented");
}

void WaxStore::Commit() {
  throw std::runtime_error("WaxStore::Commit not implemented");
}

void WaxStore::Close() {
  is_open_ = false;
}

WaxStats WaxStore::Stats() const {
  return stats_;
}

WaxStore::WaxStore(std::filesystem::path path) : path_(std::move(path)) {}

void WaxStore::LoadState(bool deep_verify) {
  if (!std::filesystem::exists(path_)) {
    throw StoreError("store file does not exist: " + path_.string());
  }
  const auto file_size = FileSize(path_);
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

  file_generation_ = footer_slice->footer.generation;
  wal_committed_seq_ = footer_slice->footer.wal_committed_seq;
  footer_offset_ = footer_slice->footer_offset;
  is_open_ = true;

  stats_.generation = file_generation_;
  stats_.frame_count = toc_summary.frame_count;
  stats_.pending_frames = 0;
}

}  // namespace waxcpp
