#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/fts5_search_engine.hpp"
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
  kRemove = 2,
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

std::vector<std::byte> BuildStructuredFactRemovePayload(const std::string& entity,
                                                        const std::string& attribute) {
  std::vector<std::byte> out{};
  out.reserve(32 + entity.size() + attribute.size());
  out.insert(out.end(), kStructuredFactMagic.begin(), kStructuredFactMagic.end());
  AppendU8(out, static_cast<std::uint8_t>(StructuredFactOpcode::kRemove));
  AppendString(out, entity);
  AppendString(out, attribute);
  return out;
}

std::optional<StructuredFactRecord> ParseStructuredFactPayload(const std::vector<std::byte>& payload) {
  if (payload.size() < kStructuredFactMagic.size() + 1 + 4 + 4) {
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
  StructuredFactRecord record{};
  if (*opcode_u8 == static_cast<std::uint8_t>(StructuredFactOpcode::kUpsert)) {
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
    record.opcode = StructuredFactOpcode::kUpsert;
    record.entity = *entity;
    record.attribute = *attribute;
    record.value = *value;
    record.metadata = std::move(metadata);
  } else if (*opcode_u8 == static_cast<std::uint8_t>(StructuredFactOpcode::kRemove)) {
    const auto entity = read_string();
    const auto attribute = read_string();
    if (!entity.has_value() || !attribute.has_value()) {
      return std::nullopt;
    }
    record.opcode = StructuredFactOpcode::kRemove;
    record.entity = *entity;
    record.attribute = *attribute;
  } else {
    return std::nullopt;
  }
  if (cursor != payload.size()) {
    return std::nullopt;
  }
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

void ThrowIfClosed(bool closed) {
  if (closed) {
    throw std::runtime_error("memory orchestrator is closed");
  }
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

struct StoreSearchChannels {
  std::vector<SearchResult> text_results;
  std::vector<SearchResult> vector_results;
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
      continue;
    }
    if (fact->opcode == StructuredFactOpcode::kRemove) {
      (void)structured_memory.Remove(fact->entity, fact->attribute);
    }
  }
}

StoreSearchChannels BuildStoreChannels(WaxStore& store,
                                       const FTS5SearchEngine* store_text_index,
                                       const FTS5SearchEngine* structured_text_index,
                                       const VectorSearchEngine* vector_index,
                                       std::shared_ptr<EmbeddingProvider> embedder,
                                       bool enable_text_search,
                                       bool enable_vector_search,
                                       const SearchRequest& request) {
  StoreSearchChannels channels{};
  if (request.top_k <= 0) {
    return channels;
  }
  const bool text_mode_enabled = request.mode.kind != SearchModeKind::kVectorOnly;
  const bool vector_mode_enabled = request.mode.kind != SearchModeKind::kTextOnly;
  const bool has_query_text = request.query.has_value() && !request.query->empty();
  const bool text_channel_enabled = enable_text_search && text_mode_enabled && has_query_text;

  std::optional<std::vector<float>> query_embedding = request.embedding;
  if (!query_embedding.has_value() && enable_vector_search && vector_mode_enabled && embedder != nullptr &&
      has_query_text) {
    query_embedding = embedder->Embed(*request.query);
  }
  const bool vector_channel_enabled =
      enable_vector_search && vector_mode_enabled && embedder != nullptr && query_embedding.has_value();
  if (!text_channel_enabled && !vector_channel_enabled) {
    return channels;
  }

  if (text_channel_enabled && store_text_index != nullptr) {
    const auto indexed_text_results = store_text_index->Search(*request.query, request.top_k);
    channels.text_results.reserve(indexed_text_results.size());
    for (const auto& indexed : indexed_text_results) {
      const auto meta = store.FrameMeta(indexed.frame_id);
      if (!meta.has_value() || meta->status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(indexed.frame_id);
      if (ParseStructuredFactPayload(payload).has_value()) {
        continue;
      }
      SearchResult store_text_result{};
      store_text_result.frame_id = indexed.frame_id;
      store_text_result.score = indexed.score;
      store_text_result.preview_text = BytesToString(payload);
      store_text_result.sources = {SearchSource::kText};
      channels.text_results.push_back(std::move(store_text_result));
    }
  }

  if (vector_channel_enabled && vector_index != nullptr) {
    const auto vector_hits = vector_index->Search(*query_embedding, request.top_k);
    channels.vector_results.reserve(vector_hits.size());
    for (const auto& [frame_id, score] : vector_hits) {
      const auto meta = store.FrameMeta(frame_id);
      if (!meta.has_value() || meta->status != 0) {
        continue;
      }
      const auto payload = store.FrameContent(frame_id);
      if (ParseStructuredFactPayload(payload).has_value()) {
        continue;
      }
      SearchResult vector_result{};
      vector_result.frame_id = frame_id;
      vector_result.score = score;
      vector_result.preview_text = BytesToString(payload);
      vector_result.sources = {SearchSource::kVector};
      channels.vector_results.push_back(std::move(vector_result));
    }
  }

  if (text_channel_enabled && structured_text_index != nullptr) {
    auto fact_results = structured_text_index->Search(*request.query, request.top_k);
    for (auto& result : fact_results) {
      result.sources = {SearchSource::kStructuredMemory};
      channels.text_results.push_back(std::move(result));
    }
  }
  return channels;
}

inline constexpr std::uint64_t kStructuredMemoryFrameIdBase = (1ULL << 63);

std::string StructuredFactPreviewText(const StructuredMemoryEntry& entry) {
  return entry.entity + " " + entry.attribute + " " + entry.value;
}

void RebuildTextIndexFromStore(WaxStore& store, FTS5SearchEngine& store_text_index) {
  store_text_index = FTS5SearchEngine{};

  const auto metas = store.FrameMetas();
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    if (ParseStructuredFactPayload(payload).has_value()) {
      continue;
    }
    store_text_index.StageIndex(meta.id, BytesToString(payload));
  }
  store_text_index.CommitStaged();
}

std::vector<std::vector<float>> BuildEmbeddingsForTexts(std::shared_ptr<EmbeddingProvider> embedder,
                                                         const std::vector<std::string>& texts,
                                                         int ingest_batch_size,
                                                         const char* error_context) {
  if (texts.empty()) {
    return {};
  }
  std::vector<std::vector<float>> out{};
  out.reserve(texts.size());

  if (auto* batch_embedder = dynamic_cast<BatchEmbeddingProvider*>(embedder.get()); batch_embedder != nullptr &&
      texts.size() > 1) {
    const std::size_t batch_size =
        ingest_batch_size > 0 ? static_cast<std::size_t>(ingest_batch_size) : texts.size();
    for (std::size_t start = 0; start < texts.size(); start += batch_size) {
      const auto end = std::min(texts.size(), start + batch_size);
      std::vector<std::string> slice{};
      slice.reserve(end - start);
      for (std::size_t i = start; i < end; ++i) {
        slice.push_back(texts[i]);
      }
      auto partial = batch_embedder->EmbedBatch(slice);
      if (partial.size() != slice.size()) {
        throw std::runtime_error(std::string(error_context) + ": mismatched embedding batch size");
      }
      out.insert(out.end(),
                 std::make_move_iterator(partial.begin()),
                 std::make_move_iterator(partial.end()));
    }
  } else {
    for (const auto& text : texts) {
      out.push_back(embedder->Embed(text));
    }
  }
  return out;
}

void RebuildVectorIndexFromStore(WaxStore& store,
                                 std::shared_ptr<EmbeddingProvider> embedder,
                                 int ingest_batch_size,
                                 USearchVectorEngine& vector_index) {
  vector_index = USearchVectorEngine(embedder->dimensions());

  const auto metas = store.FrameMetas();
  std::vector<std::uint64_t> frame_ids{};
  std::vector<std::string> texts{};
  frame_ids.reserve(metas.size());
  texts.reserve(metas.size());
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    if (ParseStructuredFactPayload(payload).has_value()) {
      continue;
    }
    frame_ids.push_back(meta.id);
    texts.push_back(BytesToString(payload));
  }
  auto embeddings = BuildEmbeddingsForTexts(embedder, texts, ingest_batch_size, "rebuild vector index");
  if (embeddings.size() != frame_ids.size()) {
    throw std::runtime_error("rebuild vector index: embedding count mismatch");
  }

  std::vector<std::uint64_t> valid_ids{};
  std::vector<std::vector<float>> valid_embeddings{};
  valid_ids.reserve(frame_ids.size());
  valid_embeddings.reserve(frame_ids.size());
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    if (embeddings[i].size() != static_cast<std::size_t>(vector_index.dimensions())) {
      continue;
    }
    valid_ids.push_back(frame_ids[i]);
    valid_embeddings.push_back(std::move(embeddings[i]));
  }
  if (!valid_ids.empty()) {
    vector_index.StageAddBatch(valid_ids, valid_embeddings);
    vector_index.CommitStaged();
  }
}

void RebuildStructuredFactIndex(const StructuredMemoryStore& structured_memory, FTS5SearchEngine& structured_text_index) {
  structured_text_index = FTS5SearchEngine{};
  const auto facts = structured_memory.All(-1);
  for (const auto& fact : facts) {
    structured_text_index.StageIndex(kStructuredMemoryFrameIdBase + fact.id, StructuredFactPreviewText(fact));
  }
  structured_text_index.CommitStaged();
}

}  // namespace

MemoryOrchestrator::MemoryOrchestrator(const std::filesystem::path& path,
                                       const OrchestratorConfig& config,
                                       std::shared_ptr<EmbeddingProvider> embedder)
    : config_(config),
      store_(std::filesystem::exists(path) ? WaxStore::Open(path) : WaxStore::Create(path)),
      embedder_(std::move(embedder)) {
  if (config_.rag.search_mode.kind == SearchModeKind::kTextOnly && !config_.enable_text_search) {
    throw std::runtime_error("text-only search mode requires text search to be enabled");
  }
  if (config_.rag.search_mode.kind == SearchModeKind::kVectorOnly && !config_.enable_vector_search) {
    throw std::runtime_error("vector-only search mode requires vector search to be enabled");
  }
  if (config_.rag.search_mode.kind == SearchModeKind::kHybrid &&
      !config_.enable_text_search &&
      !config_.enable_vector_search) {
    throw std::runtime_error("hybrid search mode requires at least one enabled search channel");
  }
  if (config_.enable_vector_search && !embedder_) {
    throw std::runtime_error("vector search enabled requires embedder in current scaffold");
  }
  ReplayStructuredFactsFromStore(store_, structured_memory_);
  RebuildTextIndexFromStore(store_, store_text_index_);
  RebuildStructuredFactIndex(structured_memory_, structured_text_index_);
  if (config_.enable_vector_search && embedder_ != nullptr) {
    vector_index_ = std::make_unique<USearchVectorEngine>(embedder_->dimensions());
    RebuildVectorIndexFromStore(store_, embedder_, config_.ingest_batch_size, *vector_index_);
  }
}

void MemoryOrchestrator::Remember(const std::string& content, const Metadata& metadata) {
  ThrowIfClosed(closed_);
  const auto chunks = ChunkContent(content, config_.chunking.target_tokens, config_.chunking.overlap_tokens);

  std::optional<std::vector<std::vector<float>>> chunk_embeddings{};
  if (config_.enable_vector_search && embedder_ != nullptr) {
    chunk_embeddings = BuildEmbeddingsForTexts(
        embedder_, chunks, config_.ingest_batch_size, "remember");
    if (chunk_embeddings->size() != chunks.size()) {
      throw std::runtime_error("remember: embedding count mismatch");
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
    if (config_.enable_text_search) {
      store_text_index_.StageIndex(frame_id, chunk);
    }

    if (config_.enable_vector_search && embedder_ != nullptr) {
      std::vector<float> embedding{};
      if (chunk_embeddings.has_value()) {
        embedding = std::move((*chunk_embeddings)[chunk_index]);
      } else {
        embedding = embedder_->Embed(chunk);
      }
      if (vector_index_ != nullptr && embedding.size() == static_cast<std::size_t>(vector_index_->dimensions())) {
        vector_index_->StageAdd(frame_id, embedding);
      }
      if (!embedding.empty() && config_.embedding_cache_capacity > 0) {
        if (embedding_cache_.size() >= static_cast<std::size_t>(config_.embedding_cache_capacity)) {
          embedding_cache_.clear();
        }
        embedding_cache_[frame_id] = std::move(embedding);
      }
    }
  }
}

RAGContext MemoryOrchestrator::Recall(const std::string& query) {
  ThrowIfClosed(closed_);
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
      config_.enable_text_search ? &store_text_index_ : nullptr,
      config_.enable_text_search ? &structured_text_index_ : nullptr,
      vector_index_.get(),
      embedder_,
      config_.enable_text_search,
      config_.enable_vector_search,
      req);
  const auto response = UnifiedSearchWithCandidates(req, channels.text_results, channels.vector_results);
  return BuildFastRAGContext(req, response);
}

RAGContext MemoryOrchestrator::Recall(const std::string& query, const std::vector<float>& embedding) {
  ThrowIfClosed(closed_);
  if (!config_.enable_vector_search) {
    throw std::runtime_error("Recall(query, embedding) requires vector search to be enabled");
  }
  if (vector_index_ != nullptr && embedding.size() != static_cast<std::size_t>(vector_index_->dimensions())) {
    throw std::runtime_error("Recall(query, embedding) dimension mismatch with vector index");
  }
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
      config_.enable_text_search ? &store_text_index_ : nullptr,
      config_.enable_text_search ? &structured_text_index_ : nullptr,
      vector_index_.get(),
      embedder_,
      config_.enable_text_search,
      config_.enable_vector_search,
      req);
  const auto response = UnifiedSearchWithCandidates(req, channels.text_results, channels.vector_results);
  return BuildFastRAGContext(req, response);
}

void MemoryOrchestrator::RememberFact(const std::string& entity,
                                      const std::string& attribute,
                                      const std::string& value,
                                      const Metadata& metadata) {
  ThrowIfClosed(closed_);
  const auto fact_id = structured_memory_.Upsert(entity, attribute, value, metadata);
  if (config_.enable_text_search) {
    StructuredMemoryEntry preview_entry{};
    preview_entry.id = fact_id;
    preview_entry.entity = entity;
    preview_entry.attribute = attribute;
    preview_entry.value = value;
    structured_text_index_.StageIndex(kStructuredMemoryFrameIdBase + fact_id, StructuredFactPreviewText(preview_entry));
  }
  const auto payload = BuildStructuredFactUpsertPayload(entity, attribute, value, metadata);
  (void)store_.Put(payload, {});
}

bool MemoryOrchestrator::ForgetFact(const std::string& entity, const std::string& attribute) {
  ThrowIfClosed(closed_);
  const auto existing = structured_memory_.Get(entity, attribute);
  if (!existing.has_value()) {
    return false;
  }
  (void)structured_memory_.Remove(entity, attribute);
  if (config_.enable_text_search) {
    structured_text_index_.StageRemove(kStructuredMemoryFrameIdBase + existing->id);
  }
  const auto payload = BuildStructuredFactRemovePayload(entity, attribute);
  (void)store_.Put(payload, {});
  return true;
}

std::vector<StructuredMemoryEntry> MemoryOrchestrator::RecallFactsByEntityPrefix(const std::string& entity_prefix,
                                                                                  int limit) {
  ThrowIfClosed(closed_);
  return structured_memory_.QueryByEntityPrefix(entity_prefix, limit);
}

void MemoryOrchestrator::Flush() {
  ThrowIfClosed(closed_);
  store_.Commit();
  if (config_.enable_text_search) {
    store_text_index_.CommitStaged();
    structured_text_index_.CommitStaged();
  }
  if (vector_index_ != nullptr) {
    vector_index_->CommitStaged();
  }
}

void MemoryOrchestrator::Close() {
  if (closed_) {
    return;
  }
  store_.Close();
  closed_ = true;
}

}  // namespace waxcpp
