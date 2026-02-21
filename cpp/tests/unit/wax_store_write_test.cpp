#include "waxcpp/wax_store.hpp"

#include "../../src/core/mv2s_format.hpp"
#include "../../src/core/wax_store_test_hooks.hpp"
#include "../test_logger.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_write_test_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
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

std::vector<std::byte> ReadExactly(const std::filesystem::path& path, std::uint64_t offset, std::size_t length) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open file for read");
  }
  in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!in) {
    throw std::runtime_error("failed to seek for read");
  }
  std::vector<std::byte> out(length);
  in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
  if (in.gcount() != static_cast<std::streamsize>(length)) {
    throw std::runtime_error("short read");
  }
  return out;
}

class ScopedCommitFailStep {
 public:
  explicit ScopedCommitFailStep(std::optional<std::uint32_t> step) {
    if (step.has_value()) {
      waxcpp::core::testing::SetCommitFailStep(*step);
    } else {
      waxcpp::core::testing::ClearCommitFailStep();
    }
  }

  ~ScopedCommitFailStep() {
    waxcpp::core::testing::ClearCommitFailStep();
  }
};

waxcpp::core::mv2s::TocSummary ReadCommittedToc(const std::filesystem::path& path) {
  const auto footer_offset = ReadLE64At(path, 24);  // header.footer_offset
  const auto footer_bytes = ReadExactly(path, footer_offset, static_cast<std::size_t>(waxcpp::core::mv2s::kFooterSize));
  const auto footer = waxcpp::core::mv2s::DecodeFooter(footer_bytes);
  const auto toc_offset = footer_offset - footer.toc_len;
  const auto toc_bytes = ReadExactly(path, toc_offset, static_cast<std::size_t>(footer.toc_len));
  return waxcpp::core::mv2s::DecodeToc(toc_bytes);
}

void RunScenarioPutCommitReopen(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: put -> commit -> reopen");
  auto store = waxcpp::WaxStore::Create(path);

  const std::vector<std::byte> payload = {
      std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC}, std::byte{0xDD},
  };
  const auto frame_id = store.Put(payload);
  Require(frame_id == 0, "first frame_id must be 0");
  Require(store.Stats().pending_frames == 1, "pending_frames must increment after put");

  store.Commit();
  auto stats = store.Stats();
  Require(stats.frame_count == 1, "frame_count must be 1 after commit");
  Require(stats.pending_frames == 0, "pending_frames must reset after commit");
  Require(stats.generation > 0, "generation must advance after commit");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  auto reopened_stats = reopened.Stats();
  Require(reopened_stats.frame_count == 1, "reopened frame_count must be 1");
  Require(reopened_stats.pending_frames == 0, "reopened pending_frames must be 0");
  reopened.Close();
}

void RunScenarioPendingRecoveryCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending WAL recovery then commit");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload = {
      std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
  };
  (void)store.Put(payload);
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  auto stats_before = reopened.Stats();
  Require(stats_before.frame_count == 0, "uncommitted put must not change committed frame_count");
  Require(stats_before.pending_frames == 1, "pending put must be visible after reopen");
  reopened.Commit();
  auto stats_after = reopened.Stats();
  Require(stats_after.frame_count == 1, "frame_count must be 1 after committing recovered pending put");
  Require(stats_after.pending_frames == 0, "pending_frames must be 0 after commit");
  reopened.Close();
}

void RunScenarioDeleteAndSupersedePersist(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: delete/supersede persist in TOC");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload_a = {std::byte{0x10}, std::byte{0x11}};
  const std::vector<std::byte> payload_b = {std::byte{0x20}, std::byte{0x21}};
  const auto id0 = store.Put(payload_a);
  const auto id1 = store.Put(payload_b);
  Require(id0 == 0 && id1 == 1, "expected dense frame ids 0,1");

  store.Supersede(id0, id1);
  store.Delete(id1);
  store.Commit();
  store.Close();

  const auto toc = ReadCommittedToc(path);
  Require(toc.frames.size() == 2, "expected two committed frames");
  Require(toc.frames[0].superseded_by.has_value(), "frame 0 must have superseded_by");
  Require(*toc.frames[0].superseded_by == 1, "frame 0 superseded_by must be 1");
  Require(toc.frames[1].supersedes.has_value(), "frame 1 must have supersedes");
  Require(*toc.frames[1].supersedes == 0, "frame 1 supersedes must be 0");
  Require(toc.frames[1].status == 1, "frame 1 status must be deleted");
}

void RunScenarioCrashWindowAfterTocWrite(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after TOC write (before footer)");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload0 = {std::byte{0x31}};
  const std::vector<std::byte> payload1 = {std::byte{0x32}};
  (void)store.Put(payload0);
  store.Commit();

  (void)store.Put(payload1);
  bool threw = false;
  {
    ScopedCommitFailStep fail_step(1);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 1");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 1, "expected old committed frame_count after TOC-only crash");
  Require(stats.pending_frames == 1, "expected pending WAL mutation after TOC-only crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterFooterWrite(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after footer write (before headers)");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload0 = {std::byte{0x41}};
  const std::vector<std::byte> payload1 = {std::byte{0x42}};
  (void)store.Put(payload0);
  store.Commit();

  (void)store.Put(payload1);
  bool threw = false;
  {
    ScopedCommitFailStep fail_step(2);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 2");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after footer-published crash");
  Require(stats.pending_frames == 0, "expected no pending put after footer-published crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterHeaderA(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after header A write (before header B)");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload0 = {std::byte{0x51}};
  const std::vector<std::byte> payload1 = {std::byte{0x52}};
  (void)store.Put(payload0);
  store.Commit();

  (void)store.Put(payload1);
  bool threw = false;
  {
    ScopedCommitFailStep fail_step(3);
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
  }
  Require(threw, "commit should fail at injected step 3");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after header-A crash");
  Require(stats.pending_frames == 0, "expected no pending put after header-A crash");
  reopened.Close();
}

}  // namespace

int main() {
  const auto path = UniquePath();
  try {
    waxcpp::tests::Log("wax_store_write_test: start");
    waxcpp::tests::LogKV("wax_store_write_test_path", path.string());

    RunScenarioPutCommitReopen(path);
    RunScenarioPendingRecoveryCommit(path);
    RunScenarioDeleteAndSupersedePersist(path);
    RunScenarioCrashWindowAfterTocWrite(path);
    RunScenarioCrashWindowAfterFooterWrite(path);
    RunScenarioCrashWindowAfterHeaderA(path);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    waxcpp::tests::Log("wax_store_write_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return EXIT_FAILURE;
  }
}
