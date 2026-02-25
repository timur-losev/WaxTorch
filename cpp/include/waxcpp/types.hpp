#pragma once

#include "waxcpp/live_set_rewrite.hpp"

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
  int max_snippets = 24;
  int rrf_k = 60;
  int preview_max_bytes = 512;
  int expansion_max_tokens = 600;
  int max_context_tokens = 1500;
  int snippet_max_tokens = 200;
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

  /// Optional extracted answer span (populated by DeterministicAnswerExtractor).
  std::string extracted_answer;
};

struct ChunkingStrategy {
  int target_tokens = 400;
  int overlap_tokens = 40;
};

/// Assembly mode for FastRAG context builder.
enum class FastRAGMode {
  kFast,          // Expansion + snippets.
  kDenseCached,   // Expansion + surrogates + snippets.
};

struct FastRAGConfig {
  FastRAGMode mode = FastRAGMode::kFast;

  int max_context_tokens = 1500;
  int expansion_max_tokens = 600;
  int expansion_max_bytes = 2 * 1024 * 1024;
  int snippet_max_tokens = 200;
  int max_snippets = 24;
  int max_surrogates = 8;
  int surrogate_max_tokens = 60;
  int search_top_k = 24;
  SearchMode search_mode{SearchModeKind::kHybrid, 0.5f};
  int rrf_k = 60;
  int preview_max_bytes = 512;

  /// Enable deterministic query-aware reranking for context item ordering.
  bool enable_answer_focused_ranking = true;
  int answer_rerank_window = 12;
  float answer_distractor_penalty = 0.30f;

  /// Enable deterministic answer extraction as post-processing on RAGContext.
  bool enable_answer_extraction = true;

  /// Enable query-aware tier selection (boosts tier for specific queries).
  bool enable_query_aware_tier_selection = true;

  /// Optional fixed "now" timestamp for deterministic tier selection.
  /// When nullopt, uses wall clock time.
  std::optional<std::int64_t> deterministic_now_ms;
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

  /// Scheduled live-set file compaction configuration.
  /// By default, disabled. Set `live_set_rewrite_schedule.enabled = true`
  /// and configure cadence/threshold/cooldown to enable automatic compaction.
  LiveSetRewriteSchedule live_set_rewrite_schedule{};
};

}  // namespace waxcpp
