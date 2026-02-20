#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp {

using Metadata = std::unordered_map<std::string, std::string>;

enum class SearchSource {
  kText,
  kVector,
  kTimeline,
  kStructuredMemory,
};

enum class SearchModeKind {
  kTextOnly,
  kVectorOnly,
  kHybrid,
};

struct SearchMode {
  SearchModeKind kind = SearchModeKind::kTextOnly;
  float alpha = 0.5f;
};

enum class VectorEnginePreference {
  kAuto,
  kMetalPreferred,
  kCpuOnly,
};

struct SearchRequest {
  std::optional<std::string> query;
  std::optional<std::vector<float>> embedding;
  VectorEnginePreference vector_preference = VectorEnginePreference::kAuto;
  SearchMode mode{};
  int top_k = 10;
  int rrf_k = 60;
  int preview_max_bytes = 512;
};

struct SearchResult {
  std::uint64_t frame_id = 0;
  float score = 0.0f;
  std::optional<std::string> preview_text;
  std::vector<SearchSource> sources;
};

struct SearchResponse {
  std::vector<SearchResult> results;
};

enum class RAGItemKind {
  kSnippet,
  kExpanded,
  kSurrogate,
};

struct RAGItem {
  RAGItemKind kind = RAGItemKind::kSnippet;
  std::uint64_t frame_id = 0;
  float score = 0.0f;
  std::vector<SearchSource> sources;
  std::string text;
};

struct RAGContext {
  std::string query;
  std::vector<RAGItem> items;
  int total_tokens = 0;
};

struct ChunkingStrategy {
  int target_tokens = 400;
  int overlap_tokens = 40;
};

struct FastRAGConfig {
  int max_context_tokens = 1500;
  int expansion_max_tokens = 600;
  int snippet_max_tokens = 200;
  int max_snippets = 24;
  int search_top_k = 24;
  SearchMode search_mode{SearchModeKind::kHybrid, 0.5f};
  int rrf_k = 60;
  int preview_max_bytes = 512;
};

struct OrchestratorConfig {
  bool enable_text_search = true;
  bool enable_vector_search = true;
  FastRAGConfig rag{};
  ChunkingStrategy chunking{};
  int ingest_concurrency = 1;
  int ingest_batch_size = 32;
  int embedding_cache_capacity = 2048;
  bool use_metal_vector_search = true;
  bool require_on_device_providers = true;
};

}  // namespace waxcpp
