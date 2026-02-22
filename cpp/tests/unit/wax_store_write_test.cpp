#include "waxcpp/wax_store.hpp"

#include "../../src/core/mv2s_format.hpp"
#include "../../src/core/wal_ring.hpp"
#include "../../src/core/wax_store_test_hooks.hpp"
#include "../test_logger.hpp"

#include <array>
#include <chrono>
#include <cmath>
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

void RunScenarioPutBatchContracts(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putBatch contracts");
  auto store = waxcpp::WaxStore::Create(path);

  const std::vector<std::vector<std::byte>> payloads = {
      {std::byte{0x11}},
      {std::byte{0x12}, std::byte{0x13}},
      {std::byte{0x14}, std::byte{0x15}, std::byte{0x16}},
  };

  const auto ids = store.PutBatch(payloads, {});
  Require(ids.size() == payloads.size(), "PutBatch must return id for each payload");
  Require(ids[0] == 0 && ids[1] == 1 && ids[2] == 2, "PutBatch must allocate dense monotonic frame ids");
  Require(store.Stats().pending_frames == payloads.size(), "PutBatch must stage all mutations as pending");
  store.Commit();
  store.Close();

  auto reopened = waxcpp::WaxStore::Open(path);
  Require(reopened.Stats().frame_count == payloads.size(), "PutBatch commit must persist all frames");
  reopened.Close();

  bool threw = false;
  try {
    auto mismatch_store = waxcpp::WaxStore::Open(path);
    const std::vector<waxcpp::Metadata> mismatched_metadata = {
        {{"k", "v"}},
    };
    (void)mismatch_store.PutBatch(payloads, mismatched_metadata);
    mismatch_store.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutBatch must reject metadata size mismatch");
}

void RunScenarioPutEmbeddingContracts(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding contracts");
  auto store = waxcpp::WaxStore::Create(path);
  const auto frame_id = store.Put({std::byte{0xE1}});
  Require(frame_id == 0, "expected initial frame id for putEmbedding contract scenario");
  const auto pre_stats = store.Stats();
  const auto pre_wal = store.WalStats();

  store.PutEmbedding(frame_id, {0.25F, 0.50F, 0.75F});
  const auto staged_stats = store.Stats();
  const auto staged_wal = store.WalStats();
  Require(staged_stats.pending_frames == pre_stats.pending_frames,
          "putEmbedding must not affect pending_frames counter");
  Require(staged_wal.last_seq > pre_wal.last_seq, "putEmbedding must append WAL mutation");

  store.Commit();
  const auto after_commit = store.Stats();
  const auto after_commit_wal = store.WalStats();
  Require(after_commit.frame_count == pre_stats.frame_count + 1,
          "commit should persist the one staged putFrame alongside putEmbedding");
  Require(after_commit.pending_frames == 0, "commit should clear pending WAL state");
  Require(after_commit_wal.committed_seq >= staged_wal.last_seq,
          "commit should checkpoint putEmbedding WAL sequence");
  store.Close();

  bool threw = false;
  try {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.PutEmbeddingBatch({frame_id, frame_id}, {{0.1F}, {0.2F, 0.3F}});
    reopened.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutEmbeddingBatch must reject mixed embedding dimensions");

  threw = false;
  try {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.PutEmbeddingBatch({frame_id, frame_id}, {{0.1F}});
    reopened.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutEmbeddingBatch must reject frame_ids/vectors size mismatch");

  threw = false;
  try {
    auto reopened = waxcpp::WaxStore::Open(path);
    reopened.PutEmbedding(frame_id, {});
    reopened.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "PutEmbedding must reject empty vector");
}

void RunScenarioPendingEmbeddingSnapshot(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending embedding snapshot");
  auto store = waxcpp::WaxStore::Create(path);
  const auto frame_ids = store.PutBatch(
      {{std::byte{0xD1}}, {std::byte{0xD2}}, {std::byte{0xD3}}},
      {});
  Require(frame_ids.size() == 3, "expected 3 frame ids in pending embedding snapshot scenario");
  store.Commit();

  store.PutEmbedding(frame_ids[0], {0.1F, 0.2F});
  store.PutEmbeddingBatch({frame_ids[1], frame_ids[2]}, {{0.3F, 0.4F}, {0.5F, 0.6F}});

  const auto snapshot = store.PendingEmbeddingMutations();
  Require(snapshot.embeddings.size() == 3, "expected three pending embeddings");
  Require(snapshot.latest_sequence.has_value(), "expected latest_sequence for pending embeddings");
  bool has_frame0 = false;
  bool has_frame1 = false;
  bool has_frame2 = false;
  bool has_expected_value = false;
  for (const auto& embedding : snapshot.embeddings) {
    if (embedding.frame_id == frame_ids[0]) {
      has_frame0 = true;
      if (embedding.dimension == 2 &&
          embedding.vector.size() == 2 &&
          std::fabs(embedding.vector[0] - 0.1F) < 1e-6F &&
          std::fabs(embedding.vector[1] - 0.2F) < 1e-6F) {
        has_expected_value = true;
      }
    } else if (embedding.frame_id == frame_ids[1]) {
      has_frame1 = true;
    } else if (embedding.frame_id == frame_ids[2]) {
      has_frame2 = true;
    }
  }
  Require(has_frame0 && has_frame1 && has_frame2, "pending embedding snapshot missing expected frame ids");
  Require(has_expected_value, "pending embedding snapshot missing expected vector payload for first frame");

  const auto filtered = store.PendingEmbeddingMutations(snapshot.latest_sequence);
  Require(filtered.embeddings.empty(), "since=latest_sequence should return empty pending embedding set");
  Require(filtered.latest_sequence == snapshot.latest_sequence,
          "latest_sequence should still reflect current pending embedding head");

  store.Commit();
  const auto after_commit = store.PendingEmbeddingMutations();
  Require(after_commit.embeddings.empty(), "commit should clear pending embedding mutations");
  Require(!after_commit.latest_sequence.has_value(), "latest_sequence should be empty after embedding commit");
  store.Close();
}

void RunScenarioPendingEmbeddingSnapshotReopenRecovery(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending embedding snapshot survives reopen recovery");
  std::uint64_t persisted_frame_id = 0;
  {
    auto store = waxcpp::WaxStore::Create(path);
    persisted_frame_id = store.Put({std::byte{0xE2}});
    store.Commit();
    store.PutEmbedding(persisted_frame_id, {1.25F, 2.5F});
    // Simulate crash: no explicit Close()/Commit().
  }

  std::optional<std::uint64_t> first_latest{};
  {
    auto reopened = waxcpp::WaxStore::Open(path);
    const auto stats = reopened.Stats();
    Require(stats.frame_count == 1, "reopen should preserve previously committed frame_count");
    Require(stats.pending_frames == 0, "embedding-only pending should not affect pending_frames counter");

    const auto snapshot = reopened.PendingEmbeddingMutations();
    Require(snapshot.embeddings.size() == 1, "expected one recovered pending embedding");
    Require(snapshot.latest_sequence.has_value(), "expected latest sequence for recovered pending embedding");
    Require(snapshot.embeddings[0].frame_id == persisted_frame_id, "unexpected recovered pending embedding frame_id");
    Require(snapshot.embeddings[0].vector.size() == 2, "unexpected recovered embedding vector size");
    first_latest = snapshot.latest_sequence;

    // Recovered-only pending state must not auto-commit on close.
    reopened.Close();
  }

  {
    auto reopened_again = waxcpp::WaxStore::Open(path);
    const auto snapshot = reopened_again.PendingEmbeddingMutations();
    Require(snapshot.embeddings.size() == 1, "recovered pending embedding must remain after close");
    Require(snapshot.latest_sequence == first_latest, "recovered pending embedding sequence must remain stable");
    reopened_again.Commit();
    const auto after_commit = reopened_again.PendingEmbeddingMutations();
    Require(after_commit.embeddings.empty(), "commit should clear recovered pending embedding");
    reopened_again.Close();
  }
}

void RunScenarioPutEmbeddingUnknownFrameRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding unknown frame rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    store.PutEmbedding(999, {0.9F, 1.1F});
    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject putEmbedding targeting unknown frame_id");
    // Simulate abrupt stop; avoid Close() auto-commit retry path.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 0, "rejected putEmbedding commit must not change frame_count");
  reopened.Close();
}

void RunScenarioPutEmbeddingBatchUnknownFrameRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding batch with unknown frame rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const auto valid_frame = store.Put({std::byte{0xE3}});
    store.PutEmbeddingBatch({valid_frame, 777}, {{0.2F, 0.4F}, {0.6F, 0.8F}});
    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject putEmbeddingBatch containing unknown frame_id");
    // Simulate abrupt stop; avoid Close() auto-commit retry path.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 0, "rejected putEmbeddingBatch commit must not change frame_count");
  reopened.Close();
}

void RunScenarioPutEmbeddingForwardReferenceRejected(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: putEmbedding forward reference rejected at commit");
  {
    auto store = waxcpp::WaxStore::Create(path);
    // Sequence order is important: putEmbedding(frame=0) comes before putFrame(frame=0).
    store.PutEmbedding(0, {0.7F, 0.9F});
    (void)store.Put({std::byte{0xE4}});
    bool threw = false;
    try {
      store.Commit();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "commit must reject forward-reference putEmbedding that precedes putFrame");
    // Simulate abrupt stop; avoid Close() auto-commit retry path.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 0, "forward-reference reject must keep committed frame_count unchanged");
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

void RunScenarioPendingRecoverySkipsUndecodableTail(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: pending WAL recovery skips undecodable tail record");
  const std::vector<std::byte> payload = {
      std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
  };
  waxcpp::WaxWALStats crashed_wal{};

  {
    auto store = waxcpp::WaxStore::Create(path);
    (void)store.Put(payload);
    crashed_wal = store.WalStats();
    // Simulate abrupt crash: leave one valid pending record in WAL.
  }

  Require(crashed_wal.last_seq >= 1, "expected wal last_seq >= 1 after pending put");
  Require(crashed_wal.wal_size > 0, "expected non-zero wal_size");

  {
    waxcpp::core::wal::WalRingWriter writer(path,
                                            waxcpp::core::mv2s::kWalOffset,
                                            crashed_wal.wal_size,
                                            crashed_wal.write_pos,
                                            crashed_wal.checkpoint_pos,
                                            crashed_wal.pending_bytes,
                                            crashed_wal.last_seq);
    const std::vector<std::byte> unknown_opcode_payload = {std::byte{0xFF}};
    (void)writer.Append(unknown_opcode_payload);
    // Do not publish updated header state to simulate torn process after WAL append.
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  const auto before = reopened.Stats();
  const auto before_wal = reopened.WalStats();
  Require(before.frame_count == 0, "undecodable WAL tail must not change committed frame_count");
  Require(before.pending_frames == 1, "only decodable pending putFrame should be exposed");
  Require(before_wal.last_seq >= 2, "scan state should advance through undecodable tail record");

  reopened.Commit();
  const auto after = reopened.Stats();
  Require(after.frame_count == 1, "commit should apply decodable pending putFrame");
  Require(after.pending_frames == 0, "pending WAL state should clear after commit");
  Require(reopened.FrameContent(0) == payload, "committed frame payload mismatch after decode-stop recovery");
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

void RunScenarioCrashWindowAfterCheckpointBeforeHeaders(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after checkpoint (before headers)");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload0 = {std::byte{0x57}};
    const std::vector<std::byte> payload1 = {std::byte{0x58}};
    (void)store.Put(payload0);
    store.Commit();

    (void)store.Put(payload1);
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(5);
      try {
        store.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at injected step 5");
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after checkpoint-before-headers crash");
  Require(stats.pending_frames == 0, "expected no pending put after checkpoint-before-headers crash");
  reopened.Close();
}

void RunScenarioCrashWindowAfterHeaderB(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: crash-window after header B write");
  {
    auto store = waxcpp::WaxStore::Create(path);
    const std::vector<std::byte> payload0 = {std::byte{0x53}};
    const std::vector<std::byte> payload1 = {std::byte{0x54}};
    (void)store.Put(payload0);
    store.Commit();

    (void)store.Put(payload1);
    bool threw = false;
    {
      ScopedCommitFailStep fail_step(4);
      try {
        store.Commit();
      } catch (const std::exception&) {
        threw = true;
      }
    }
    Require(threw, "commit should fail at injected step 4");
    // Simulate crash: do not call Close().
  }

  auto reopened = waxcpp::WaxStore::Open(path);
  reopened.Verify(true);
  const auto stats = reopened.Stats();
  Require(stats.frame_count == 2, "expected new committed frame_count after header-B crash");
  Require(stats.pending_frames == 0, "expected no pending put after header-B crash");
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

  const auto closed_wal_stats = store.WalStats();
  Require(closed_wal_stats.auto_commit_count == 1,
          "Close() with local pending mutations must increment wal auto_commit_count");
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

    const auto closed_wal_stats = reopened.WalStats();
    Require(closed_wal_stats.auto_commit_count == 0,
            "Close() on recovered pending-only state must not increment wal auto_commit_count");
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
    RunScenarioPutBatchContracts(path);
    RunScenarioPutEmbeddingContracts(path);
    RunScenarioPendingEmbeddingSnapshot(path);
    RunScenarioPendingEmbeddingSnapshotReopenRecovery(path);
    RunScenarioPutEmbeddingUnknownFrameRejected(path);
    RunScenarioPutEmbeddingBatchUnknownFrameRejected(path);
    RunScenarioPutEmbeddingForwardReferenceRejected(path);
    RunScenarioPendingRecoveryCommit(path);
    RunScenarioPendingRecoverySkipsUndecodableTail(path);
    RunScenarioDeleteAndSupersedePersist(path);
    RunScenarioCrashWindowAfterTocWrite(path);
    RunScenarioCrashWindowAfterFooterWrite(path);
    RunScenarioCrashWindowAfterCheckpointBeforeHeaders(path);
    RunScenarioCrashWindowAfterHeaderA(path);
    RunScenarioCrashWindowAfterHeaderB(path);
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
