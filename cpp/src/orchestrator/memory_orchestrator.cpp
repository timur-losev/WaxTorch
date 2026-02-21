#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/search.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

namespace waxcpp {
namespace {

std::string BytesToString(const std::vector<std::byte>& payload) {
  std::string out{};
  out.reserve(payload.size());
  for (const auto b : payload) {
    out.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return out;
}

std::vector<std::string> TokenizeLower(std::string_view text) {
  std::vector<std::string> tokens{};
  std::string current{};
  current.reserve(32);
  for (const unsigned char ch : text) {
    if (std::isalnum(ch) != 0) {
      current.push_back(static_cast<char>(std::tolower(ch)));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

float TextOverlapScore(std::string_view query, std::string_view text) {
  const auto query_tokens = TokenizeLower(query);
  if (query_tokens.empty()) {
    return 0.0F;
  }

  std::unordered_map<std::string, std::uint32_t> text_freq{};
  for (const auto& token : TokenizeLower(text)) {
    auto it = text_freq.find(token);
    if (it == text_freq.end()) {
      text_freq.emplace(token, 1U);
    } else {
      it->second += 1U;
    }
  }

  float score = 0.0F;
  for (const auto& token : query_tokens) {
    const auto it = text_freq.find(token);
    if (it != text_freq.end()) {
      score += static_cast<float>(it->second);
    }
  }
  return score;
}

SearchResponse BuildStoreTextResponse(WaxStore& store, const SearchRequest& request) {
  SearchResponse response{};
  if (!request.query.has_value() || request.query->empty() || request.top_k <= 0) {
    return response;
  }

  const auto metas = store.FrameMetas();
  response.results.reserve(metas.size());
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    const auto text = BytesToString(payload);
    const auto score = TextOverlapScore(*request.query, text);
    if (score <= 0.0F) {
      continue;
    }

    SearchResult result{};
    result.frame_id = meta.id;
    result.score = score;
    result.preview_text = text;
    result.sources = {SearchSource::kText};
    response.results.push_back(std::move(result));
  }

  std::sort(response.results.begin(), response.results.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score > rhs.score;
    }
    return lhs.frame_id < rhs.frame_id;
  });
  if (response.results.size() > static_cast<std::size_t>(request.top_k)) {
    response.results.resize(static_cast<std::size_t>(request.top_k));
  }
  return response;
}

}  // namespace

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
  auto response = BuildStoreTextResponse(store_, req);
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
  auto response = BuildStoreTextResponse(store_, req);
  return BuildFastRAGContext(req, response);
}

void MemoryOrchestrator::Flush() {
  store_.Commit();
}

void MemoryOrchestrator::Close() {
  store_.Close();
}

}  // namespace waxcpp
