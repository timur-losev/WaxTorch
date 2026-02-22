#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/fts5_search_engine.hpp"
#include "waxcpp/search.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <span>
#include <string_view>
#include <thread>
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

inline constexpr std::array<std::byte, 6> kEmbeddingRecordMagic = {
    std::byte{'W'},
    std::byte{'A'},
    std::byte{'X'},
    std::byte{'E'},
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

struct EmbeddingRecord {
  std::uint64_t frame_id = 0;
  std::vector<float> embedding;
};

void AppendU8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void AppendU64LE(std::vector<std::byte>& out, std::uint64_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendU32LE(std::vector<std::byte>& out, std::uint32_t value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    out.push_back(static_cast<std::byte>((value >> (8U * i)) & 0xFFU));
  }
}

void AppendF32LE(std::vector<std::byte>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU32LE(out, bits);
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

std::vector<std::byte> BuildEmbeddingRecordPayload(std::uint64_t frame_id, const std::vector<float>& embedding) {
  if (embedding.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::runtime_error("embedding record vector length exceeds uint32");
  }
  std::vector<std::byte> out{};
  out.reserve(kEmbeddingRecordMagic.size() + 8 + 4 + embedding.size() * sizeof(float));
  out.insert(out.end(), kEmbeddingRecordMagic.begin(), kEmbeddingRecordMagic.end());
  AppendU64LE(out, frame_id);
  AppendU32LE(out, static_cast<std::uint32_t>(embedding.size()));
  for (const float value : embedding) {
    AppendF32LE(out, value);
  }
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

std::optional<EmbeddingRecord> ParseEmbeddingRecordPayload(const std::vector<std::byte>& payload) {
  if (payload.size() < kEmbeddingRecordMagic.size() + 8 + 4) {
    return std::nullopt;
  }
  if (!std::equal(kEmbeddingRecordMagic.begin(), kEmbeddingRecordMagic.end(), payload.begin())) {
    return std::nullopt;
  }

  std::size_t cursor = kEmbeddingRecordMagic.size();
  auto read_u64 = [&]() -> std::optional<std::uint64_t> {
    if (cursor + 8 > payload.size()) {
      return std::nullopt;
    }
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < 8; ++i) {
      out |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(payload[cursor + i])) << (8U * i);
    }
    cursor += 8;
    return out;
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
  auto read_f32 = [&]() -> std::optional<float> {
    const auto bits = read_u32();
    if (!bits.has_value()) {
      return std::nullopt;
    }
    float value = 0.0F;
    std::uint32_t raw = *bits;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
  };

  const auto frame_id = read_u64();
  const auto count = read_u32();
  if (!frame_id.has_value() || !count.has_value()) {
    return std::nullopt;
  }
  std::vector<float> embedding{};
  embedding.reserve(*count);
  for (std::uint32_t i = 0; i < *count; ++i) {
    const auto value = read_f32();
    if (!value.has_value()) {
      return std::nullopt;
    }
    embedding.push_back(*value);
  }
  if (cursor != payload.size()) {
    return std::nullopt;
  }

  EmbeddingRecord record{};
  record.frame_id = *frame_id;
  record.embedding = std::move(embedding);
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

bool IsInternalOrchestratorPayload(const std::vector<std::byte>& payload) {
  return ParseStructuredFactPayload(payload).has_value() || ParseEmbeddingRecordPayload(payload).has_value();
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
      enable_vector_search && vector_mode_enabled && query_embedding.has_value();
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
      if (IsInternalOrchestratorPayload(payload)) {
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
      if (IsInternalOrchestratorPayload(payload)) {
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
    if (IsInternalOrchestratorPayload(payload)) {
      continue;
    }
    store_text_index.StageIndex(meta.id, BytesToString(payload));
  }
  store_text_index.CommitStaged();
}

struct PersistedEmbeddingSnapshot {
  std::unordered_map<std::uint64_t, std::vector<float>> by_frame{};
  std::optional<int> dimensions{};
};

PersistedEmbeddingSnapshot LoadPersistedEmbeddingsFromStore(WaxStore& store) {
  PersistedEmbeddingSnapshot snapshot{};
  const auto metas = store.FrameMetas();
  for (const auto& meta : metas) {
    if (meta.status != 0) {
      continue;
    }
    const auto payload = store.FrameContent(meta.id);
    const auto embedding_record = ParseEmbeddingRecordPayload(payload);
    if (!embedding_record.has_value()) {
      continue;
    }
    if (!snapshot.dimensions.has_value() && !embedding_record->embedding.empty()) {
      snapshot.dimensions = static_cast<int>(embedding_record->embedding.size());
    }
    snapshot.by_frame[embedding_record->frame_id] = std::move(embedding_record->embedding);
  }
  return snapshot;
}

std::vector<std::vector<float>> BuildEmbeddingsForTexts(std::shared_ptr<EmbeddingProvider> embedder,
                                                         const std::vector<std::string>& texts,
                                                         int ingest_batch_size,
                                                         int ingest_concurrency,
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
    const std::size_t worker_count = ingest_concurrency > 1
                                         ? std::min(texts.size(), static_cast<std::size_t>(ingest_concurrency))
                                         : 1ULL;
    if (worker_count <= 1) {
      for (const auto& text : texts) {
        out.push_back(embedder->Embed(text));
      }
      return out;
    }

    out.assign(texts.size(), {});
    std::atomic<std::size_t> next_index{0};
    std::atomic<bool> stop_workers{false};
    std::exception_ptr first_error{};
    std::mutex error_mutex{};

    auto worker = [&]() {
      while (true) {
        if (stop_workers.load(std::memory_order_acquire)) {
          return;
        }
        const auto index = next_index.fetch_add(1);
        if (index >= texts.size()) {
          return;
        }
        try {
          out[index] = embedder->Embed(texts[index]);
        } catch (...) {
          std::lock_guard<std::mutex> error_lock(error_mutex);
          if (first_error == nullptr) {
            first_error = std::current_exception();
          }
          stop_workers.store(true, std::memory_order_release);
          return;
        }
      }
    };

    std::vector<std::thread> workers{};
    workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
      workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
      thread.join();
    }
    if (first_error != nullptr) {
      std::rethrow_exception(first_error);
    }
  }
  return out;
}

void RebuildVectorIndexFromStore(WaxStore& store,
                                 const PersistedEmbeddingSnapshot& persisted_embeddings,
                                 std::shared_ptr<EmbeddingProvider> embedder,
                                 int ingest_batch_size,
                                 int ingest_concurrency,
                                 USearchVectorEngine& vector_index) {
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
    if (IsInternalOrchestratorPayload(payload)) {
      continue;
    }
    frame_ids.push_back(meta.id);
    texts.push_back(BytesToString(payload));
  }

  std::vector<std::uint64_t> missing_ids{};
  std::vector<std::string> missing_texts{};
  missing_ids.reserve(frame_ids.size());
  missing_texts.reserve(frame_ids.size());

  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    const auto persisted_it = persisted_embeddings.by_frame.find(frame_ids[i]);
    if (persisted_it == persisted_embeddings.by_frame.end() ||
        persisted_it->second.size() != static_cast<std::size_t>(vector_index.dimensions())) {
      missing_ids.push_back(frame_ids[i]);
      missing_texts.push_back(texts[i]);
      continue;
    }
    vector_index.StageAdd(frame_ids[i], persisted_it->second);
  }

  if (embedder != nullptr && !missing_ids.empty()) {
    auto embeddings = BuildEmbeddingsForTexts(
        embedder, missing_texts, ingest_batch_size, ingest_concurrency, "rebuild vector index");
    if (embeddings.size() != missing_ids.size()) {
      throw std::runtime_error("rebuild vector index: embedding count mismatch");
    }
    for (std::size_t i = 0; i < missing_ids.size(); ++i) {
      if (embeddings[i].size() != static_cast<std::size_t>(vector_index.dimensions())) {
        continue;
      }
      vector_index.StageAdd(missing_ids[i], embeddings[i]);
    }
  }

  vector_index.CommitStaged();
}

std::optional<int> ResolveVectorDimensions(std::shared_ptr<EmbeddingProvider> embedder,
                                           const PersistedEmbeddingSnapshot& persisted_embeddings) {
  if (embedder != nullptr) {
    return embedder->dimensions();
  }
  if (persisted_embeddings.dimensions.has_value() && *persisted_embeddings.dimensions > 0) {
    return persisted_embeddings.dimensions;
  }
  return std::nullopt;
}

void EnsureEmbedderRequiredForRemember(const OrchestratorConfig& config,
                                       std::shared_ptr<EmbeddingProvider> embedder) {
  if (config.enable_vector_search && embedder == nullptr) {
    throw std::runtime_error("remember requires embedder when vector search is enabled");
  }
}

void StagePersistedEmbeddingRecord(WaxStore& store,
                                   std::uint64_t frame_id,
                                   const std::vector<float>& embedding,
                                   int expected_dimensions) {
  if (expected_dimensions <= 0 || embedding.size() != static_cast<std::size_t>(expected_dimensions)) {
    return;
  }
  const auto payload = BuildEmbeddingRecordPayload(frame_id, embedding);
  (void)store.Put(payload, {});
}

void StageVectorIndexEmbedding(USearchVectorEngine* vector_index,
                               std::uint64_t frame_id,
                               const std::vector<float>& embedding) {
  if (vector_index == nullptr) {
    return;
  }
  if (embedding.size() != static_cast<std::size_t>(vector_index->dimensions())) {
    return;
  }
  vector_index->StageAdd(frame_id, embedding);
}

void RebuildStructuredFactIndex(const StructuredMemoryStore& structured_memory, FTS5SearchEngine& structured_text_index) {
  structured_text_index = FTS5SearchEngine{};
  const auto facts = structured_memory.All(-1);
  for (const auto& fact : facts) {
    structured_text_index.StageIndex(kStructuredMemoryFrameIdBase + fact.id, StructuredFactPreviewText(fact));
  }
  structured_text_index.CommitStaged();
}

void RebuildRuntimeStateFromStore(WaxStore& store,
                                  const OrchestratorConfig& config,
                                  const std::shared_ptr<EmbeddingProvider>& embedder,
                                  StructuredMemoryStore& structured_memory,
                                  FTS5SearchEngine& store_text_index,
                                  FTS5SearchEngine& structured_text_index,
                                  std::unique_ptr<USearchVectorEngine>& vector_index,
                                  std::unordered_map<std::uint64_t, std::vector<float>>& embedding_cache) {
  structured_memory = StructuredMemoryStore{};
  ReplayStructuredFactsFromStore(store, structured_memory);
  embedding_cache.clear();

  if (config.enable_text_search) {
    RebuildTextIndexFromStore(store, store_text_index);
    RebuildStructuredFactIndex(structured_memory, structured_text_index);
  } else {
    store_text_index = FTS5SearchEngine{};
    structured_text_index = FTS5SearchEngine{};
  }

  if (!config.enable_vector_search) {
    vector_index.reset();
    return;
  }

  const auto persisted_embeddings = LoadPersistedEmbeddingsFromStore(store);
  const auto vector_dims = ResolveVectorDimensions(embedder, persisted_embeddings);
  if (!vector_dims.has_value() || *vector_dims <= 0) {
    vector_index.reset();
    return;
  }

  vector_index = std::make_unique<USearchVectorEngine>(*vector_dims);
  RebuildVectorIndexFromStore(store,
                              persisted_embeddings,
                              embedder,
                              config.ingest_batch_size,
                              config.ingest_concurrency,
                              *vector_index);
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
  if (config_.enable_vector_search && embedder_ == nullptr) {
    throw std::runtime_error("vector-enabled config requires embedder");
  }
  RebuildRuntimeStateFromStore(store_,
                               config_,
                               embedder_,
                               structured_memory_,
                               store_text_index_,
                               structured_text_index_,
                               vector_index_,
                               embedding_cache_);
}

void MemoryOrchestrator::Remember(const std::string& content, const Metadata& metadata) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  EnsureEmbedderRequiredForRemember(config_, embedder_);
  const auto chunks = ChunkContent(content, config_.chunking.target_tokens, config_.chunking.overlap_tokens);

  std::optional<std::vector<std::vector<float>>> chunk_embeddings{};
  if (config_.enable_vector_search && embedder_ != nullptr) {
    chunk_embeddings = BuildEmbeddingsForTexts(
        embedder_, chunks, config_.ingest_batch_size, config_.ingest_concurrency, "remember");
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
      StageVectorIndexEmbedding(vector_index_.get(), frame_id, embedding);
      const int vector_dims = vector_index_ != nullptr ? vector_index_->dimensions() : 0;
      StagePersistedEmbeddingRecord(store_, frame_id, embedding, vector_dims);
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
  std::lock_guard<std::mutex> lock(mutex_);
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
  std::lock_guard<std::mutex> lock(mutex_);
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
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  const auto fact_id = structured_memory_.StageUpsert(entity, attribute, value, metadata);
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
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  const auto removed_id = structured_memory_.StageRemove(entity, attribute);
  if (!removed_id.has_value()) {
    return false;
  }
  if (config_.enable_text_search) {
    structured_text_index_.StageRemove(kStructuredMemoryFrameIdBase + *removed_id);
  }
  const auto payload = BuildStructuredFactRemovePayload(entity, attribute);
  (void)store_.Put(payload, {});
  return true;
}

std::vector<StructuredMemoryEntry> MemoryOrchestrator::RecallFactsByEntityPrefix(const std::string& entity_prefix,
                                                                                  int limit) {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  return structured_memory_.QueryByEntityPrefix(entity_prefix, limit);
}

void MemoryOrchestrator::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  ThrowIfClosed(closed_);
  store_.Commit();
  try {
    structured_memory_.CommitStaged();
    if (config_.enable_text_search) {
      store_text_index_.CommitStaged();
      structured_text_index_.CommitStaged();
    }
    if (vector_index_ != nullptr) {
      vector_index_->CommitStaged();
    }
  } catch (...) {
    RebuildRuntimeStateFromStore(store_,
                                 config_,
                                 embedder_,
                                 structured_memory_,
                                 store_text_index_,
                                 structured_text_index_,
                                 vector_index_,
                                 embedding_cache_);
    throw;
  }
}

void MemoryOrchestrator::Close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closed_) {
    return;
  }
  store_.Close();
  closed_ = true;
}

}  // namespace waxcpp
