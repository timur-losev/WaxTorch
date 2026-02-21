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
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload = {
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    };
    (void)store.Put(payload);
    // Simulate crash/no-graceful-close: do not call Close() so WAL remains pending.
  }

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
  {
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
    // Simulate crash: do not call Close() to avoid graceful auto-commit.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 1, "expected old committed frame_count after TOC-only crash");
  Require(stats.pending_frames == 1, "expected pending WAL mutation after TOC-only crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterFooterWrite(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after footer write (before headers)");
  {
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
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after footer-published crash");
  Require(stats.pending_frames == 0, "expected no pending put after footer-published crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterHeaderA(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after header A write (before header B)");
  {
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
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after header-A crash");
  Require(stats.pending_frames == 0, "expected no pending put after header-A crash");
  reopened.Close();
}

void RunScenarioSupersedeCycleRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: supersede cycle rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload_a = {std::byte{0x61}};
    const std::vector<std::byte> payload_b = {std::byte{0x62}};
    const auto a = store.Put(payload_a);
    const auto b = store.Put(payload_b);
    Require(a == 0 && b == 1, "expected dense ids for cycle scenario");

    store.Supersede(a, b);  // b -> a
    store.Supersede(b, a);  // a -> b (cycle)

    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject supersede cycle");
    // Simulate abrupt end; avoid Close() auto-commit retry.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  // Nothing committed, both puts and supersede ops remain pending.
  Require(reopened.Stats().frame_count == 0, "cycle-rejected commit must not advance committed frame_count");
  reopened.Close();
}

void RunScenarioSupersedeConflictRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: supersede conflict rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto a = store.Put({std::byte{0x71}});
    const auto b = store.Put({std::byte{0x72}});
    const auto c = store.Put({std::byte{0x73}});
    Require(a == 0 && b == 1 && c == 2, "expected dense ids for supersede conflict scenario");

    store.Supersede(a, b);  // b -> a
    store.Supersede(c, b);  // b -> c (conflict: b already supersedes a)

    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject supersede conflict");
    // Simulate abrupt end; avoid Close() auto-commit retry.
  }
}

void RunScenarioCloseAutoCommitsPending(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: close auto-commits pending mutations");
  auto store = waxcpp::WaxStore::Create(path);
  const auto id = store.Put({std::byte{0x81}, std::byte{0x82}});
  Require(id == 0, "expected first id=0 in close auto-commit scenario");
  Require(store.Stats().pending_frames == 1, "pending_frames should be 1 before close");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 1, "close should commit pending put");
  Require(stats.pending_frames == 0, "no pending frames expected after close auto-commit");
  reopened.Close();
}

void RunScenarioFrameReadApis(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: frame read APIs expose committed payloads");
  auto store = waxcpp::WaxStore::Create(path);
  const std::vector<std::byte> payload0 = {std::byte{0x91}, std::byte{0x92}};
  const std::vector<std::byte> payload1 = {std::byte{0xA1}, std::byte{0xA2}, std::byte{0xA3}};
  const auto id0 = store.Put(payload0);
  const auto id1 = store.Put(payload1);
  Require(id0 == 0 && id1 == 1, "expected dense ids for frame read API scenario");
  store.Delete(id0);
  store.Commit();

  const auto maybe_meta0 = store.FrameMeta(id0);
  Require(maybe_meta0.has_value(), "FrameMeta(0) must exist");
  Require(maybe_meta0->status == 1, "FrameMeta(0).status must reflect delete");

  const auto maybe_meta1 = store.FrameMeta(id1);
  Require(maybe_meta1.has_value(), "FrameMeta(1) must exist");
  Require(maybe_meta1->payload_length == payload1.size(), "FrameMeta(1).payload_length mismatch");

  const auto metas = store.FrameMetas();
  Require(metas.size() == 2, "FrameMetas size mismatch");

  const auto content1 = store.FrameContent(id1);
  Require(content1 == payload1, "FrameContent(1) mismatch");

  const auto contents = store.FrameContents({id0, id1});
  Require(contents.size() == 2, "FrameContents size mismatch");
  Require(contents.at(id0) == payload0, "FrameContents(0) mismatch");
  Require(contents.at(id1) == payload1, "FrameContents(1) mismatch");
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto reopened_content1 = reopened.FrameContent(id1);
  Require(reopened_content1 == payload1, "reopened FrameContent(1) mismatch");
  reopened.Close();
}

void RunScenarioCloseDoesNotCommitRecoveredPending(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: close does not auto-commit recovered pending WAL");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0xB1}});
    // Simulate crash/no graceful close.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    Require(stats.frame_count == 0, "recovered pending scenario should still have 0 committed frames");
    Require(stats.pending_frames == 1, "recovered pending scenario should expose pending frame");
    // Close must not auto-commit recovery-only pending mutations.
    reopened.Close();
  }

  {
    auto reopened_again = waxcpp::WaxStore::Open(path);
    const auto stats_again = reopened_again.Stats();
    Require(stats_again.frame_count == 0, "close must not commit recovery-only pending WAL");
    Require(stats_again.pending_frames == 1, "pending WAL should remain after close on recovered state");
    reopened_again.Close();
  }
}

void RunScenarioRecoveredPendingPlusLocalMutationsCommit(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recovered pending plus local mutations commit together");
  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put({std::byte{0xC1}});  // pending from crashed process
    // Simulate crash/no graceful close.
  }

  {
    auto reopened = waxcpp::WaxStore::Open(path);
    auto before = reopened.Stats();
    Require(before.frame_count == 0, "expected no committed frames before merged commit");
    Require(before.pending_frames == 1, "expected one recovered pending frame");

    const auto local_id = reopened.Put({std::byte{0xC2}});
    Require(local_id == 1, "local frame id should continue after recovered pending frame id");
    reopened.Commit();

    auto after = reopened.Stats();
    Require(after.frame_count == 2, "expected two committed frames after merged commit");
    Require(after.pending_frames == 0, "expected no pending frames after merged commit");

    const auto c0 = reopened.FrameContent(0);
    const auto c1 = reopened.FrameContent(1);
    Require(c0 == std::vector<std::byte>{std::byte{0xC1}}, "frame 0 content mismatch after merged commit");
    Require(c1 == std::vector<std::byte>{std::byte{0xC2}}, "frame 1 content mismatch after merged commit");
    reopened.Close();
  }
}

void RunScenarioWriterLeaseExclusion(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: writer lease excludes concurrent open on same store path");
  auto primary = waxcpp::WaxStore::Create(path);

  bool threw = false;
  try {
    auto competing = waxcpp::WaxStore::Open(path);
    competing.Close();
  } catch (const std::exception& ex) {
    threw = true;
    waxcpp::tests::Log(std::string("expected competing-open rejection: ") + ex.what());
  }
  Require(threw, "competing open must fail while primary writer lease is held");

  primary.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
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
    RunScenarioSupersedeCycleRejected(path);
    RunScenarioSupersedeConflictRejected(path);
    RunScenarioCloseAutoCommitsPending(path);
    RunScenarioFrameReadApis(path);
    RunScenarioCloseDoesNotCommitRecoveredPending(path);
    RunScenarioRecoveredPendingPlusLocalMutationsCommit(path);
    RunScenarioWriterLeaseExclusion(path);

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
