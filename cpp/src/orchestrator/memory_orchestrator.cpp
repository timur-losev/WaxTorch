#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/search.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <span>
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

float Dot(std::span<const float> lhs, std::span<const float> rhs) {
  float dot = 0.0F;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    dot += lhs[i] * rhs[i];
  }
  return dot;
}

float Norm(std::span<const float> v) {
  const auto d = Dot(v, v);
  return std::sqrt(std::max(d, 0.0F));
}

float CosineSimilarity(std::span<const float> lhs, std::span<const float> rhs) {
  const auto lhs_norm = Norm(lhs);
  const auto rhs_norm = Norm(rhs);
  if (lhs_norm <= 0.0F || rhs_norm <= 0.0F) {
    return 0.0F;
  }
  return Dot(lhs, rhs) / (lhs_norm * rhs_norm);
}

struct StoreSearchChannels {
  std::vector<SearchResult> text_results;
  std::vector<SearchResult> vector_results;
};

StoreSearchChannels BuildStoreChannels(WaxStore& store,
                                       std::shared_ptr<EmbeddingProvider> embedder,
                                       bool enable_text_search,
                                       bool enable_vector_search,
                                       std::unordered_map<std::uint64_t, std::vector<float>>* embedding_cache,
                                       int embedding_cache_capacity,
                                       const SearchRequest& request) {
  StoreSearchChannels channels{};
  if (!request.query.has_value() || request.query->empty() || request.top_k <= 0) {
    return channels;
  }

  std::optional<std::vector<float>> query_embedding = request.embedding;
  if (!query_embedding.has_value() && enable_vector_search && embedder != nullptr) {
    query_embedding = embedder->Embed(*request.query);
  }

  const auto metas = store.FrameMetas();
  channels.text_results.reserve(metas.size());
  channels.vector_results.reserve(metas.size());
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    const auto text = BytesToString(payload);

    if (enable_text_search) {
      const auto text_score = TextOverlapScore(*request.query, text);
      if (text_score > 0.0F) {
        SearchResult text_result{};
        text_result.frame_id = meta.id;
        text_result.score = text_score;
        text_result.preview_text = text;
        text_result.sources = {SearchSource::kText};
        channels.text_results.push_back(std::move(text_result));
      }
    }

    if (enable_vector_search && embedder != nullptr && query_embedding.has_value()) {
      const std::vector<float>* doc_embedding_ptr = nullptr;
      std::vector<float> doc_embedding{};
      if (embedding_cache != nullptr) {
        const auto cache_it = embedding_cache->find(meta.id);
        if (cache_it != embedding_cache->end()) {
          doc_embedding_ptr = &cache_it->second;
        }
      }
      if (doc_embedding_ptr == nullptr) {
        doc_embedding = embedder->Embed(text);
        if (embedding_cache != nullptr && embedding_cache_capacity > 0 &&
            doc_embedding.size() == query_embedding->size()) {
          if (embedding_cache->size() >= static_cast<std::size_t>(embedding_cache_capacity)) {
            embedding_cache->clear();
          }
          auto [it, inserted] = embedding_cache->emplace(meta.id, doc_embedding);
          if (!inserted) {
            it->second = doc_embedding;
          }
          doc_embedding_ptr = &it->second;
        }
      }
      if (doc_embedding_ptr == nullptr) {
        doc_embedding_ptr = &doc_embedding;
      }

      if (doc_embedding_ptr->size() != query_embedding->size()) {
        continue;
      }
      const auto vector_score = CosineSimilarity(std::span<const float>(doc_embedding_ptr->data(), doc_embedding_ptr->size()),
                                                 std::span<const float>(query_embedding->data(), query_embedding->size()));
      SearchResult vector_result{};
      vector_result.frame_id = meta.id;
      vector_result.score = vector_score;
      vector_result.preview_text = text;
      vector_result.sources = {SearchSource::kVector};
      channels.vector_results.push_back(std::move(vector_result));
    }
  }
  return channels;
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
  const auto frame_id = store_.Put(payload, metadata);

  if (config_.enable_vector_search && embedder_ != nullptr && config_.embedding_cache_capacity > 0) {
    auto embedding = embedder_->Embed(content);
    if (!embedding.empty()) {
      if (embedding_cache_.size() >= static_cast<std::size_t>(config_.embedding_cache_capacity)) {
        embedding_cache_.clear();
      }
      embedding_cache_[frame_id] = std::move(embedding);
    }
  }
}

RAGContext MemoryOrchestrator::Recall(const std::string& query) {
  SearchRequest req;
  req.query = query;
  req.mode = config_.rag.search_mode;
  req.top_k = config_.rag.search_top_k;
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;
  const auto channels = BuildStoreChannels(
      store_,
      embedder_,
      config_.enable_text_search,
      config_.enable_vector_search,
      &embedding_cache_,
      config_.embedding_cache_capacity,
      req);
  const auto response = UnifiedSearchWithCandidates(req, channels.text_results, channels.vector_results);
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
  const auto channels = BuildStoreChannels(
      store_,
      embedder_,
      config_.enable_text_search,
      config_.enable_vector_search,
      &embedding_cache_,
      config_.embedding_cache_capacity,
      req);
  const auto response = UnifiedSearchWithCandidates(req, channels.text_results, channels.vector_results);
  return BuildFastRAGContext(req, response);
}

void MemoryOrchestrator::Flush() {
  store_.Commit();
}

void MemoryOrchestrator::Close() {
  store_.Close();
}

}  // namespace waxcpp
