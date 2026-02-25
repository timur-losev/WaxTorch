#pragma once

#include "waxcpp/access_stats.hpp"
#include "waxcpp/answer_extractor.hpp"
#include "waxcpp/embeddings.hpp"
#include "waxcpp/fts5_search_engine.hpp"
#include "waxcpp/tier_selector.hpp"
#include "waxcpp/token_counter.hpp"
#include "waxcpp/types.hpp"
#include "waxcpp/structured_memory.hpp"
#include "waxcpp/vector_engine.hpp"
#include "waxcpp/wax_store.hpp"

#include <filesystem>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp {

class MemoryOrchestrator {
 public:
  MemoryOrchestrator(const std::filesystem::path& path,
                     const OrchestratorConfig& config,
                     std::shared_ptr<EmbeddingProvider> embedder = nullptr,
                     const TokenCounter* token_counter = nullptr);

  void Remember(const std::string& content, const Metadata& metadata = {});
  RAGContext Recall(const std::string& query);
  RAGContext Recall(const std::string& query, const std::vector<float>& embedding);
  void RememberFact(const std::string& entity,
                    const std::string& attribute,
                    const std::string& value,
                    const Metadata& metadata = {});
  bool ForgetFact(const std::string& entity, const std::string& attribute);
  std::vector<StructuredMemoryEntry> RecallFactsByEntityPrefix(const std::string& entity_prefix, int limit = 32);

  void Flush();
  void Close();

  /// Access the frame access stats manager (thread-safe).
  /// May be used to export/import stats for persistence.
  AccessStatsManager& GetAccessStats() { return access_stats_; }
  const AccessStatsManager& GetAccessStats() const { return access_stats_; }

 private:
  OrchestratorConfig config_;
  WaxStore store_;
  std::shared_ptr<EmbeddingProvider> embedder_;
  const TokenCounter* token_counter_ = nullptr;
  std::unordered_map<std::uint64_t, std::vector<float>> embedding_cache_;
  StructuredMemoryStore structured_memory_;
  FTS5SearchEngine store_text_index_;
  FTS5SearchEngine structured_text_index_;
  std::unique_ptr<USearchVectorEngine> vector_index_;
  AccessStatsManager access_stats_;
  SurrogateTierSelector tier_selector_;
  DeterministicAnswerExtractor answer_extractor_;
  bool closed_ = false;
  mutable std::mutex mutex_{};
};

}  // namespace waxcpp
