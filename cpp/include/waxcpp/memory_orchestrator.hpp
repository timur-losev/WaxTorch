#pragma once

#include "waxcpp/embeddings.hpp"
#include "waxcpp/types.hpp"
#include "waxcpp/structured_memory.hpp"
#include "waxcpp/wax_store.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp {

class MemoryOrchestrator {
 public:
  MemoryOrchestrator(const std::filesystem::path& path,
                     const OrchestratorConfig& config,
                     std::shared_ptr<EmbeddingProvider> embedder = nullptr);

  void Remember(const std::string& content, const Metadata& metadata = {});
  RAGContext Recall(const std::string& query);
  RAGContext Recall(const std::string& query, const std::vector<float>& embedding);
  void RememberFact(const std::string& entity,
                    const std::string& attribute,
                    const std::string& value,
                    const Metadata& metadata = {});
  std::vector<StructuredMemoryEntry> RecallFactsByEntityPrefix(const std::string& entity_prefix, int limit = 32);

  void Flush();
  void Close();

 private:
  OrchestratorConfig config_;
  WaxStore store_;
  std::shared_ptr<EmbeddingProvider> embedder_;
  std::unordered_map<std::uint64_t, std::vector<float>> embedding_cache_;
  StructuredMemoryStore structured_memory_;
};

}  // namespace waxcpp
