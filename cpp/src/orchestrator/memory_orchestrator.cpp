#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/search.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <utility>

namespace waxcpp {
namespace {

inline constexpr std::array<std::byte, 6> kStructuredFactMagic = {
    std::byte{'W'},
    std::byte{'A'},
    std::byte{'X'},
    std::byte{'S'},
    std::byte{'M'},
    std::byte{'1'},
};

enum class StructuredFactOpcode : std::uint8_t {
  kUpsert = 1,
};

struct StructuredFactRecord {
  StructuredFactOpcode opcode = StructuredFactOpcode::kUpsert;
  std::string entity;
  std::string attribute;
  std::string value;
  Metadata metadata;
};

void AppendU8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void AppendU32LE(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendString(std::vector<std::byte>& out, const std::string& value) {
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("structured fact field exceeds uint32 length");
  }
  AppendU32LE(out, static_cast<std::uint32_t>(value.size()));
  for (const char ch : value) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
}

std::vector<std::byte> BuildStructuredFactUpsertPayload(const std::string& entity,
                                                        const std::string& attribute,
                                                        const std::string& value,
                                                        const Metadata& metadata) {
  std::vector<std::byte> out{};
  out.reserve(64 + entity.size() + attribute.size() + value.size());
  out.insert(out.end(), kStructuredFactMagic.begin(), kStructuredFactMagic.end());
  AppendU8(out, static_cast<std::uint8_t>(StructuredFactOpcode::kUpsert));
  AppendString(out, entity);
  AppendString(out, attribute);
  AppendString(out, value);
  if (metadata.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("structured fact metadata count exceeds uint32");
  }
  AppendU32LE(out, static_cast<std::uint32_t>(metadata.size()));
  for (const auto& [key, val] : metadata) {
    AppendString(out, key);
    AppendString(out, val);
  }
  return out;
}

std::optional<StructuredFactRecord> ParseStructuredFactPayload(const std::vector<std::byte>& payload) {
  if (payload.size() < kStructuredFactMagic.size() + 1 + 4 + 4 + 4 + 4) {
    return std::nullopt;
  }
  if (!std::equal(kStructuredFactMagic.begin(), kStructuredFactMagic.end(), payload.begin())) {
    return std::nullopt;
  }

  std::size_t cursor = kStructuredFactMagic.size();
  auto read_u8 = [&]() -> std::optional<std::uint8_t> {
    if (cursor >= payload.size()) {
      return std::nullopt;
    }
    return std::to_integer<std::uint8_t>(payload[cursor++]);
  };
  auto read_u32 = [&]() -> std::optional<std::uint32_t> {
    if (cursor + 4 > payload.size()) {
      return std::nullopt;
    }
    std::uint32_t out = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      out |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(payload[cursor + i])) << (8U * i);
    }
    cursor += 4;
    return out;
  };
  auto read_string = [&]() -> std::optional<std::string> {
    const auto length = read_u32();
    if (!length.has_value()) {
      return std::nullopt;
    }
    if (cursor + *length > payload.size()) {
      return std::nullopt;
    }
    std::string out{};
    out.reserve(*length);
    for (std::size_t i = 0; i < *length; ++i) {
      out.push_back(static_cast<char>(std::to_integer<std::uint8_t>(payload[cursor + i])));
    }
    cursor += *length;
    return out;
  };

  const auto opcode_u8 = read_u8();
  if (!opcode_u8.has_value()) {
    return std::nullopt;
  }
  if (*opcode_u8 != static_cast<std::uint8_t>(StructuredFactOpcode::kUpsert)) {
    return std::nullopt;
  }

  const auto entity = read_string();
  const auto attribute = read_string();
  const auto value = read_string();
  const auto metadata_count = read_u32();
  if (!entity.has_value() || !attribute.has_value() || !value.has_value() || !metadata_count.has_value()) {
    return std::nullopt;
  }

  Metadata metadata{};
  for (std::uint32_t i = 0; i < *metadata_count; ++i) {
    const auto key = read_string();
    const auto val = read_string();
    if (!key.has_value() || !val.has_value()) {
      return std::nullopt;
    }
    metadata[*key] = *val;
  }
  if (cursor != payload.size()) {
    return std::nullopt;
  }

  StructuredFactRecord record{};
  record.opcode = StructuredFactOpcode::kUpsert;
  record.entity = *entity;
  record.attribute = *attribute;
  record.value = *value;
  record.metadata = std::move(metadata);
  return record;
}

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

std::vector<std::string> TokenizeWhitespace(std::string_view text) {
  std::vector<std::string> tokens{};
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
      ++start;
    }
    if (start >= text.size()) {
      break;
    }
    std::size_t end = start;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) {
      ++end;
    }
    tokens.emplace_back(text.substr(start, end - start));
    start = end;
  }
  return tokens;
}

std::string JoinTokenRange(const std::vector<std::string>& tokens, std::size_t begin, std::size_t end) {
  if (begin >= end || begin >= tokens.size()) {
    return {};
  }
  end = std::min(end, tokens.size());
  std::string out = tokens[begin];
  for (std::size_t i = begin + 1; i < end; ++i) {
    out.push_back(' ');
    out.append(tokens[i]);
  }
  return out;
}

std::vector<std::string> ChunkContent(const std::string& content, int target_tokens, int overlap_tokens) {
  if (target_tokens <= 0) {
    return {content};
  }
  auto tokens = TokenizeWhitespace(content);
  if (tokens.empty()) {
    return {content};
  }
  if (tokens.size() <= static_cast<std::size_t>(target_tokens)) {
    return {JoinTokenRange(tokens, 0, tokens.size())};
  }

  const int step = std::max(1, target_tokens - std::max(0, overlap_tokens));
  std::vector<std::string> chunks{};
  for (std::size_t start = 0; start < tokens.size(); start += static_cast<std::size_t>(step)) {
    const auto end = std::min(tokens.size(), start + static_cast<std::size_t>(target_tokens));
    chunks.push_back(JoinTokenRange(tokens, start, end));
    if (end == tokens.size()) {
      break;
    }
  }
  return chunks;
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

struct DocCandidate {
  std::uint64_t frame_id = 0;
  std::string text;
};

void ReplayStructuredFactsFromStore(WaxStore& store, StructuredMemoryStore& structured_memory) {
  const auto metas = store.FrameMetas();
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    const auto fact = ParseStructuredFactPayload(payload);
    if (!fact.has_value()) {
      continue;
    }
    if (fact->opcode == StructuredFactOpcode::kUpsert) {
      (void)structured_memory.Upsert(fact->entity, fact->attribute, fact->value, fact->metadata);
    }
  }
}

StoreSearchChannels BuildStoreChannels(WaxStore& store,
                                       StructuredMemoryStore* structured_memory,
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
  const bool text_mode_enabled = request.mode.kind != SearchModeKind::kVectorOnly;
  const bool vector_mode_enabled = request.mode.kind != SearchModeKind::kTextOnly;
  const bool text_channel_enabled = enable_text_search && text_mode_enabled;
  constexpr std::uint64_t kStructuredMemoryFrameIdBase = (1ULL << 63);

  std::optional<std::vector<float>> query_embedding = request.embedding;
  if (!query_embedding.has_value() && enable_vector_search && vector_mode_enabled && embedder != nullptr) {
    query_embedding = embedder->Embed(*request.query);
  }
  const bool vector_channel_enabled =
      enable_vector_search && vector_mode_enabled && embedder != nullptr && query_embedding.has_value();

  const auto metas = store.FrameMetas();
  std::vector<DocCandidate> docs{};
  docs.reserve(metas.size());
  channels.text_results.reserve(metas.size());
  channels.vector_results.reserve(metas.size());
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    if (ParseStructuredFactPayload(payload).has_value()) {
      // Internal structured-memory journal entry; excluded from regular text/vector channels.
      continue;
    }
    const auto text = BytesToString(payload);
    docs.push_back(DocCandidate{meta.id, text});

    if (text_channel_enabled) {
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
  }

  if (vector_channel_enabled) {
    std::unordered_map<std::uint64_t, std::vector<float>> computed_embeddings{};
    computed_embeddings.reserve(docs.size());
    std::vector<std::uint64_t> missing_ids{};
    std::vector<std::string> missing_texts{};
    missing_ids.reserve(docs.size());
    missing_texts.reserve(docs.size());

    for (const auto& doc : docs) {
      bool has_cached_embedding = false;
      if (embedding_cache != nullptr) {
        const auto cache_it = embedding_cache->find(doc.frame_id);
        if (cache_it != embedding_cache->end()) {
          has_cached_embedding = true;
        }
      }
      if (!has_cached_embedding) {
        missing_ids.push_back(doc.frame_id);
        missing_texts.push_back(doc.text);
      }
    }

    if (!missing_ids.empty()) {
      std::vector<std::vector<float>> missing_embeddings{};
      if (auto* batch_embedder = dynamic_cast<BatchEmbeddingProvider*>(embedder.get()); batch_embedder != nullptr) {
        missing_embeddings = batch_embedder->EmbedBatch(missing_texts);
      } else {
        missing_embeddings.reserve(missing_texts.size());
        for (const auto& text : missing_texts) {
          missing_embeddings.push_back(embedder->Embed(text));
        }
      }
      if (missing_embeddings.size() != missing_ids.size()) {
        throw std::runtime_error("embedding provider returned mismatched batch size");
      }

      for (std::size_t i = 0; i < missing_ids.size(); ++i) {
        auto& embedding = missing_embeddings[i];
        if (embedding.size() != query_embedding->size()) {
          continue;
        }
        const auto frame_id = missing_ids[i];
        if (embedding_cache != nullptr && embedding_cache_capacity > 0) {
          if (embedding_cache->size() >= static_cast<std::size_t>(embedding_cache_capacity)) {
            embedding_cache->clear();
          }
          auto [it, inserted] = embedding_cache->emplace(frame_id, embedding);
          if (!inserted) {
            it->second = embedding;
          }
        } else {
          computed_embeddings.emplace(frame_id, std::move(embedding));
        }
      }
    }

    for (const auto& doc : docs) {
      const std::vector<float>* doc_embedding_ptr = nullptr;
      if (embedding_cache != nullptr) {
        const auto cache_it = embedding_cache->find(doc.frame_id);
        if (cache_it != embedding_cache->end()) {
          doc_embedding_ptr = &cache_it->second;
        }
      }
      if (doc_embedding_ptr == nullptr) {
        const auto computed_it = computed_embeddings.find(doc.frame_id);
        if (computed_it != computed_embeddings.end()) {
          doc_embedding_ptr = &computed_it->second;
        }
      }
      if (doc_embedding_ptr == nullptr) {
        continue;
      }
      if (doc_embedding_ptr->size() != query_embedding->size()) {
        continue;
      }
      const auto vector_score = CosineSimilarity(std::span<const float>(doc_embedding_ptr->data(), doc_embedding_ptr->size()),
                                                 std::span<const float>(query_embedding->data(), query_embedding->size()));
      SearchResult vector_result{};
      vector_result.frame_id = doc.frame_id;
      vector_result.score = vector_score;
      vector_result.preview_text = doc.text;
      vector_result.sources = {SearchSource::kVector};
      channels.vector_results.push_back(std::move(vector_result));
    }
  }

  if (text_channel_enabled && structured_memory != nullptr) {
    const auto facts = structured_memory->All(-1);
    for (const auto& fact : facts) {
      const std::string fact_text = fact.entity + " " + fact.attribute + " " + fact.value;
      const auto fact_score = TextOverlapScore(*request.query, fact_text);
      if (fact_score <= 0.0F) {
        continue;
      }
      SearchResult fact_result{};
      fact_result.frame_id = kStructuredMemoryFrameIdBase + fact.id;
      fact_result.score = fact_score;
      fact_result.preview_text = fact_text;
      fact_result.sources = {SearchSource::kStructuredMemory};
      channels.text_results.push_back(std::move(fact_result));
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
  ReplayStructuredFactsFromStore(store_, structured_memory_);
}

void MemoryOrchestrator::Remember(const std::string& content, const Metadata& metadata) {
  const auto chunks = ChunkContent(content, config_.chunking.target_tokens, config_.chunking.overlap_tokens);

  std::optional<std::vector<std::vector<float>>> chunk_embeddings{};
  if (config_.enable_vector_search && embedder_ != nullptr && config_.embedding_cache_capacity > 0) {
    if (auto* batch_embedder = dynamic_cast<BatchEmbeddingProvider*>(embedder_.get()); batch_embedder != nullptr &&
        chunks.size() > 1) {
      const std::size_t batch_size =
          config_.ingest_batch_size > 0 ? static_cast<std::size_t>(config_.ingest_batch_size) : chunks.size();
      std::vector<std::vector<float>> embeddings{};
      embeddings.reserve(chunks.size());
      for (std::size_t start = 0; start < chunks.size(); start += batch_size) {
        const auto end = std::min(chunks.size(), start + batch_size);
        std::vector<std::string> slice{};
        slice.reserve(end - start);
        for (std::size_t i = start; i < end; ++i) {
          slice.push_back(chunks[i]);
        }
        auto partial = batch_embedder->EmbedBatch(slice);
        if (partial.size() != slice.size()) {
          throw std::runtime_error("batch embedding provider returned mismatched chunk embedding count");
        }
        embeddings.insert(embeddings.end(),
                          std::make_move_iterator(partial.begin()),
                          std::make_move_iterator(partial.end()));
      }
      chunk_embeddings = std::move(embeddings);
    }
  }

  for (std::size_t chunk_index = 0; chunk_index < chunks.size(); ++chunk_index) {
    const auto& chunk = chunks[chunk_index];
    std::vector<std::byte> payload{};
    payload.reserve(chunk.size());
    for (const char ch : chunk) {
      payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    const auto frame_id = store_.Put(payload, metadata);

    if (config_.enable_vector_search && embedder_ != nullptr && config_.embedding_cache_capacity > 0) {
      std::vector<float> embedding{};
      if (chunk_embeddings.has_value()) {
        embedding = std::move((*chunk_embeddings)[chunk_index]);
      } else {
        embedding = embedder_->Embed(chunk);
      }
      if (!embedding.empty()) {
        if (embedding_cache_.size() >= static_cast<std::size_t>(config_.embedding_cache_capacity)) {
          embedding_cache_.clear();
        }
        embedding_cache_[frame_id] = std::move(embedding);
      }
    }
  }
}

RAGContext MemoryOrchestrator::Recall(const std::string& query) {
  SearchRequest req;
  req.query = query;
  req.mode = config_.rag.search_mode;
  const int max_snippets = config_.rag.max_snippets > 0 ? config_.rag.max_snippets : config_.rag.search_top_k;
  req.top_k = std::min(config_.rag.search_top_k, max_snippets);
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;
  req.expansion_max_tokens = config_.rag.expansion_max_tokens;
  req.max_context_tokens = config_.rag.max_context_tokens;
  req.snippet_max_tokens = config_.rag.snippet_max_tokens;
  const auto channels = BuildStoreChannels(
      store_,
      &structured_memory_,
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
  const int max_snippets = config_.rag.max_snippets > 0 ? config_.rag.max_snippets : config_.rag.search_top_k;
  req.top_k = std::min(config_.rag.search_top_k, max_snippets);
  req.rrf_k = config_.rag.rrf_k;
  req.preview_max_bytes = config_.rag.preview_max_bytes;
  req.expansion_max_tokens = config_.rag.expansion_max_tokens;
  req.max_context_tokens = config_.rag.max_context_tokens;
  req.snippet_max_tokens = config_.rag.snippet_max_tokens;
  const auto channels = BuildStoreChannels(
      store_,
      &structured_memory_,
      embedder_,
      config_.enable_text_search,
      config_.enable_vector_search,
      &embedding_cache_,
      config_.embedding_cache_capacity,
      req);
  const auto response = UnifiedSearchWithCandidates(req, channels.text_results, channels.vector_results);
  return BuildFastRAGContext(req, response);
}

void MemoryOrchestrator::RememberFact(const std::string& entity,
                                      const std::string& attribute,
                                      const std::string& value,
                                      const Metadata& metadata) {
  (void)structured_memory_.Upsert(entity, attribute, value, metadata);
  const auto payload = BuildStructuredFactUpsertPayload(entity, attribute, value, metadata);
  (void)store_.Put(payload, {});
}

std::vector<StructuredMemoryEntry> MemoryOrchestrator::RecallFactsByEntityPrefix(const std::string& entity_prefix,
                                                                                  int limit) {
  return structured_memory_.QueryByEntityPrefix(entity_prefix, limit);
}

void MemoryOrchestrator::Flush() {
  store_.Commit();
}

void MemoryOrchestrator::Close() {
  store_.Close();
}

}  // namespace waxcpp
