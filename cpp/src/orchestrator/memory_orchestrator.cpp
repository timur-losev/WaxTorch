#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/search.hpp"

#include <stdexcept>
#include <vector>
#include <utility>

namespace waxcpp {

MemoryOrchestrator::MemoryOrchestrator(const std::filesystem::path& path,
                                       const OrchestratorConfig& config,
                                       std::shared_ptr<EmbeddingProvider> embedder)
    : config_(config),
      store_(std::filesystem::exists(path) ? WaxStore::Open(path) : WaxStore::Create(path)),
      embedder_(std::move(embedder)) {
  if (config_.enable_vector_search && !embedder_) {
    throw std::runtime_error("vector search enabled requires embedder in current scaffold");
  }
}

void MemoryOrchestrator::Remember(const std::string& content, const Metadata& metadata) {
  std::vector<std::byte> payload{};
  payload.reserve(content.size());
  for (const char ch : content) {
    payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  (void)store_.Put(payload, metadata);
}

RAGContext MemoryOrchestrator::Recall(const std::string& query) {
  SearchRequest req;
  req.query = query;
  req.mode = config_.rag.search_mode;
  req.top_k = config_.rag.search_top_k;
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;
  auto response = UnifiedSearch(req);
  return BuildFastRAGContext(req, response);
}

RAGContext MemoryOrchestrator::Recall(const std::string& query, const std::vector<float>& embedding) {
  SearchRequest req;
  req.query = query;
  req.embedding = embedding;
  req.mode = config_.rag.search_mode;
  req.top_k = config_.rag.search_top_k;
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;
  auto response = UnifiedSearch(req);
  return BuildFastRAGContext(req, response);
}

void MemoryOrchestrator::Flush() {
  store_.Commit();
}

void MemoryOrchestrator::Close() {
  store_.Close();
}

}  // namespace waxcpp
