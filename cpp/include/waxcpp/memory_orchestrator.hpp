#pragma once

#include "waxcpp/embeddings.hpp"
#include "waxcpp/types.hpp"
#include "waxcpp/wax_store.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace waxcpp {

class MemoryOrchestrator {
 public:
  MemoryOrchestrator(const std::filesystem::path& path,
                     const OrchestratorConfig& config,
                     std::shared_ptr<EmbeddingProvider> embedder = nullptr);

  void Remember(const std::string& content, const Metadata& metadata = {});
  RAGContext Recall(const std::string& query);
  RAGContext Recall(const std::string& query, const std::vector<float>& embedding);

  void Flush();
  void Close();

 private:
  OrchestratorConfig config_;
  WaxStore store_;
  std::shared_ptr<EmbeddingProvider> embedder_;
};

}  // namespace waxcpp
