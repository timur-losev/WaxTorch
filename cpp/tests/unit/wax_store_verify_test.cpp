#include "waxcpp/wax_store.hpp"

#include "../../src/core/mv2s_format.hpp"
#include "../../src/core/sha256.hpp"
#include "../test_logger.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_m2_verify_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

void WriteZeros(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for corruption write");
  }
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) {
    throw std::runtime_error("failed to seek file for corruption write");
  }
  std::vector<char> zeros(length, 0);
  file.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
  if (!file) {
    throw std::runtime_error("failed to write corruption bytes");
  }
}

std::vector<std::byte> ReadBytesAt(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file for byte read");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw std::runtime_error("failed to seek file for byte read");
  }
  std::vector<std::byte> bytes(length);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("short read for bytes");
  }
  return bytes;
}

void WriteBytesAt(const std::filesystem::path& path, std::uint64_t offset, const std::vector<std::byte>& bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for bytes write");
  }
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) {
    throw std::runtime_error("failed to seek file for bytes write");
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    throw std::runtime_error("failed to write bytes");
  }
}

void WriteBytesAt(const std::filesystem::path& path,
                  std::uint64_t offset,
                  const std::array<std::byte, waxcpp::core::mv2s::kHeaderPageSize>& bytes) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for page write");
  }
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) {
    throw std::runtime_error("failed to seek file for page write");
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    throw std::runtime_error("failed to write page bytes");
  }
}

void AppendBytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    throw std::runtime_error("failed to open file for append");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw std::runtime_error("failed to append bytes");
  }
}

void AppendBytes(const std::filesystem::path& path,
                 const std::array<std::byte, waxcpp::core::mv2s::kFooterSize>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out) {
    throw std::runtime_error("failed to open file for append");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!out) {
    throw std::runtime_error("failed to append bytes");
  }
}

void ExtendFileSparse(const std::filesystem::path& path, std::uint64_t extra_bytes) {
  if (extra_bytes == 0) {
    return;
  }
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for sparse extension");
  }
  file.seekp(static_cast<std::streamoff>(extra_bytes - 1), std::ios::end);
  if (!file) {
    throw std::runtime_error("failed to seek file for sparse extension");
  }
  const char zero = 0;
  file.write(&zero, 1);
  if (!file) {
    throw std::runtime_error("failed to extend file");
  }
}

std::uint64_t ReadLE64At(const std::filesystem::path& path, std::uint64_t offset) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file for read");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw std::runtime_error("failed to seek file");
  }
  std::array<unsigned char, 8> bytes{};
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (in.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw std::runtime_error("short read for uint64");
  }
  std::uint64_t out = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out |= static_cast<std::uint64_t>(bytes[i]) << (8U * i);
  }
  return out;
}

void FlipByteAt(const std::filesystem::path& path, std::uint64_t offset) {
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!file) {
    throw std::runtime_error("failed to open file for byte flip");
  }
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  char value = 0;
  file.read(&value, 1);
  if (file.gcount() != 1) {
    throw std::runtime_error("short read for byte flip");
  }
  value ^= static_cast<char>(0x01);
  file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
  file.write(&value, 1);
  if (!file) {
    throw std::runtime_error("failed to write flipped byte");
  }
}

void ExpectThrow(const std::string& name, const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception& ex) {
    waxcpp::tests::Log("expected exception in " + name + ": " + ex.what());
    return;
  } catch (...) {
    waxcpp::tests::Log("expected non-std exception in " + name);
    return;
  }
  throw std::runtime_error("expected throw: " + name);
}

}  // namespace

int main() {
  const auto path = UniquePath();

  try {
    waxcpp::tests::Log("wax_store_verify_test: start");
    waxcpp::tests::LogKV("test_store_path", path.string());
    {
      waxcpp::tests::Log("scenario: create + verify empty store");
      auto store = waxcpp::WaxStore::Create(path);
      store.Verify(false);
      const auto stats = store.Stats();
      waxcpp::tests::LogKV("create_stats_frame_count", stats.frame_count);
      waxcpp::tests::LogKV("create_stats_generation", stats.generation);
      if (stats.frame_count != 0) {
        throw std::runtime_error("expected empty frame_count after create");
      }
      store.Close();
    }

    {
      waxcpp::tests::Log("scenario: reopen + verify");
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      reopened.Close();
    }

    // Corrupt TOC checksum and ensure verify/open fail.
    waxcpp::tests::Log("scenario: corrupt TOC checksum -> open should fail");
    const auto footer_offset = ReadLE64At(path, 24);            // header.footer_offset
    const auto toc_len = ReadLE64At(path, footer_offset + 8);   // footer.toc_len
    const auto toc_checksum_last_byte = (footer_offset - toc_len) + toc_len - 1;
    waxcpp::tests::LogKV("corrupt_toc_footer_offset", footer_offset);
    waxcpp::tests::LogKV("corrupt_toc_len", toc_len);
    waxcpp::tests::LogKV("corrupt_toc_checksum_last_byte", toc_checksum_last_byte);
    FlipByteAt(path, toc_checksum_last_byte);
    ExpectThrow("open_with_corrupt_toc_checksum", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
    });

    // Recreate clean file for header-corruption tests.
    {
      waxcpp::tests::Log("scenario: recreate clean store");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    // Append a newer valid footer and ensure scan picks it when active header is stale.
    waxcpp::tests::Log("scenario: stale header footer pointer -> choose latest scanned footer");
    const auto original_footer_offset = ReadLE64At(path, 24);                 // header.footer_offset
    const auto original_toc_len = ReadLE64At(path, original_footer_offset + 8);
    const auto original_toc_offset = original_footer_offset - original_toc_len;
    waxcpp::tests::LogKV("stale_header_original_footer_offset", original_footer_offset);
    waxcpp::tests::LogKV("stale_header_original_toc_len", original_toc_len);
    const auto toc_bytes = ReadBytesAt(path, original_toc_offset, static_cast<std::size_t>(original_toc_len));
    waxcpp::core::mv2s::Footer appended_footer{};
    appended_footer.toc_len = toc_bytes.size();
    std::copy(toc_bytes.end() - 32, toc_bytes.end(), appended_footer.toc_hash.begin());
    appended_footer.generation = 7;
    appended_footer.wal_committed_seq = 11;

    AppendBytes(path, toc_bytes);
    AppendBytes(path, waxcpp::core::mv2s::EncodeFooter(appended_footer));
    waxcpp::tests::LogKV("stale_header_appended_footer_generation", appended_footer.generation);

    // Corrupt header page A (newer header) so open falls back to stale header B.
    WriteZeros(path, 0, static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      waxcpp::tests::LogKV("stale_header_recovered_generation", reopened.Stats().generation);
      if (reopened.Stats().generation != 7) {
        throw std::runtime_error("expected open() to select latest scanned footer generation");
      }
      reopened.Close();
    }

    // Recreate clean file for deep-verify checksum tests.
    {
      waxcpp::tests::Log("scenario: deep verify detects payload corruption");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    // Build one-frame TOC and payload, invalidate old footer path, and verify deep checksum.
    const std::uint64_t data_start = waxcpp::core::mv2s::kWalOffset + waxcpp::core::mv2s::kDefaultWalSize;
    std::vector<std::byte> payload = {
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}, std::byte{0x55},
        std::byte{0x66}, std::byte{0x77}, std::byte{0x88}, std::byte{0x99}, std::byte{0xAB},
    };
    WriteBytesAt(path, data_start, payload);

    waxcpp::core::mv2s::FrameSummary frame{};
    frame.id = 0;
    frame.payload_offset = data_start;
    frame.payload_length = payload.size();
    frame.payload_checksum = waxcpp::core::Sha256Digest(payload);

    const auto frame_toc = waxcpp::core::mv2s::EncodeTocV1({&frame, 1});
    const auto frame_footer_offset = data_start + payload.size() + frame_toc.size();
    waxcpp::tests::LogKV("deep_verify_data_start", data_start);
    waxcpp::tests::LogKV("deep_verify_payload_size", static_cast<std::uint64_t>(payload.size()));
    waxcpp::tests::LogKV("deep_verify_toc_size", static_cast<std::uint64_t>(frame_toc.size()));
    waxcpp::tests::LogKV("deep_verify_footer_offset", frame_footer_offset);

    waxcpp::core::mv2s::Footer frame_footer{};
    frame_footer.toc_len = frame_toc.size();
    std::copy(frame_toc.end() - 32, frame_toc.end(), frame_footer.toc_hash.begin());
    frame_footer.generation = 21;
    frame_footer.wal_committed_seq = 34;

    WriteBytesAt(path, data_start + payload.size(), frame_toc);
    const auto footer_bytes = waxcpp::core::mv2s::EncodeFooter(frame_footer);
    WriteBytesAt(path, frame_footer_offset, std::vector<std::byte>(footer_bytes.begin(), footer_bytes.end()));

    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
      reopened.Close();
    }

    FlipByteAt(path, data_start + 1);
    ExpectThrow("verify_deep_detects_payload_corruption", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
    });

    // Recreate clean file for compressed-frame deep verify semantics.
    {
      waxcpp::tests::Log("scenario: deep verify compressed frame uses stored checksum");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    std::vector<std::byte> compressed_like_payload = {
        std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF},
        std::byte{0xBA}, std::byte{0xAD}, std::byte{0xF0}, std::byte{0x0D},
    };
    WriteBytesAt(path, data_start, compressed_like_payload);

    waxcpp::core::mv2s::FrameSummary compressed_frame{};
    compressed_frame.id = 0;
    compressed_frame.payload_offset = data_start;
    compressed_frame.payload_length = compressed_like_payload.size();
    compressed_frame.payload_checksum.fill(std::byte{0xCC});  // canonical checksum placeholder
    compressed_frame.canonical_encoding = 2;                  // lz4
    compressed_frame.canonical_length = 64;
    compressed_frame.stored_checksum = waxcpp::core::Sha256Digest(compressed_like_payload);

    const auto compressed_toc = waxcpp::core::mv2s::EncodeTocV1({&compressed_frame, 1});
    const auto compressed_footer_offset = data_start + compressed_like_payload.size() + compressed_toc.size();
    waxcpp::tests::LogKV("compressed_verify_toc_size", static_cast<std::uint64_t>(compressed_toc.size()));
    waxcpp::tests::LogKV("compressed_verify_footer_offset", compressed_footer_offset);

    waxcpp::core::mv2s::Footer compressed_footer{};
    compressed_footer.toc_len = compressed_toc.size();
    std::copy(compressed_toc.end() - 32, compressed_toc.end(), compressed_footer.toc_hash.begin());
    compressed_footer.generation = 31;
    compressed_footer.wal_committed_seq = 45;

    WriteBytesAt(path, data_start + compressed_like_payload.size(), compressed_toc);
    const auto compressed_footer_bytes = waxcpp::core::mv2s::EncodeFooter(compressed_footer);
    WriteBytesAt(path,
                 compressed_footer_offset,
                 std::vector<std::byte>(compressed_footer_bytes.begin(), compressed_footer_bytes.end()));

    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
      reopened.Close();
    }

    FlipByteAt(path, data_start + 2);
    ExpectThrow("verify_deep_detects_compressed_stored_checksum_corruption", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(true);
    });

    // Recreate clean file for replay snapshot footer lookup test.
    {
      waxcpp::tests::Log("scenario: replay snapshot footer fallback");
      auto recreated = waxcpp::WaxStore::Create(path);
      recreated.Verify(false);
      recreated.Close();
    }

    const auto committed_footer_offset = ReadLE64At(path, 24);
    waxcpp::tests::LogKV("snapshot_committed_footer_offset", committed_footer_offset);
    const auto header_page_a = ReadBytesAt(path, 0, static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    auto header = waxcpp::core::mv2s::DecodeHeaderPage(header_page_a);

    // Break fast footer pointer and keep the valid committed offset only in replay snapshot.
    header.footer_offset += 123;
    waxcpp::core::mv2s::ReplaySnapshot snapshot{};
    snapshot.file_generation = header.file_generation;
    snapshot.wal_committed_seq = header.wal_committed_seq;
    snapshot.footer_offset = committed_footer_offset;
    snapshot.wal_write_pos = header.wal_write_pos;
    snapshot.wal_checkpoint_pos = header.wal_checkpoint_pos;
    snapshot.wal_pending_bytes = 0;
    snapshot.wal_last_sequence = header.wal_committed_seq;
    header.replay_snapshot = snapshot;

    WriteBytesAt(path, 0, waxcpp::core::mv2s::EncodeHeaderPage(header));

    // Move EOF far enough so footer scan window cannot see the committed footer.
    ExtendFileSparse(path, waxcpp::core::mv2s::kMaxFooterScanBytes + 1024);
    waxcpp::tests::LogKV("snapshot_sparse_extension", waxcpp::core::mv2s::kMaxFooterScanBytes + 1024);

    {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
      reopened.Close();
    }

    // Corrupt both header pages; open must fail.
    waxcpp::tests::Log("scenario: both headers corrupt -> open should fail");
    WriteZeros(path, 0, static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    WriteZeros(path, waxcpp::core::mv2s::kHeaderPageSize,
               static_cast<std::size_t>(waxcpp::core::mv2s::kHeaderPageSize));
    ExpectThrow("open_with_both_headers_corrupt", [&]() {
      auto reopened = waxcpp::WaxStore::Open(path);
      reopened.Verify(false);
    });

    std::filesystem::remove(path);
    waxcpp::tests::Log("wax_store_verify_test: finished");
    std::cout << "wax_store_verify_test passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::cerr << "wax_store_verify_test failed: " << ex.what() << "\n";
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return EXIT_FAILURE;
  }
}
