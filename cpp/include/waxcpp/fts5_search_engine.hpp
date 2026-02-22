#pragma once

#include "waxcpp/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp {

class FTS5SearchEngine {
 public:
  FTS5SearchEngine();
  ~FTS5SearchEngine();
  FTS5SearchEngine(FTS5SearchEngine&&) noexcept;
  FTS5SearchEngine& operator=(FTS5SearchEngine&&) noexcept;
  FTS5SearchEngine(const FTS5SearchEngine&) = delete;
  FTS5SearchEngine& operator=(const FTS5SearchEngine&) = delete;

  void StageIndex(std::uint64_t frame_id, const std::string& text);
  void StageIndexBatch(const std::vector<std::uint64_t>& frame_ids, const std::vector<std::string>& texts);
  void StageRemove(std::uint64_t frame_id);
  void CommitStaged();
  void RollbackStaged();
  [[nodiscard]] std::size_t PendingMutationCount() const;

  void Index(std::uint64_t frame_id, const std::string& text);
  void IndexBatch(const std::vector<std::uint64_t>& frame_ids, const std::vector<std::string>& texts);
  void Remove(std::uint64_t frame_id);
  std::vector<SearchResult> Search(const std::string& query, int top_k) const;

 private:
  struct SQLiteState;

  enum class PendingMutationType {
    kIndex,
    kRemove,
  };
  struct PendingMutation {
    PendingMutationType type = PendingMutationType::kIndex;
    std::uint64_t frame_id = 0;
    std::string text;
  };

  std::unordered_map<std::uint64_t, std::string> docs_;
  std::vector<PendingMutation> pending_mutations_;
  std::unique_ptr<SQLiteState> sqlite_;
};

}  // namespace waxcpp
