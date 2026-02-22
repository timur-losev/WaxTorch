#include "waxcpp/search.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string_view>
#include <vector>

namespace waxcpp {
namespace {

bool IsAsciiWhitespace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

std::vector<std::string> SplitWhitespaceTokens(std::string_view text) {
  std::vector<std::string> tokens{};
  std::size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && IsAsciiWhitespace(text[start])) {
      ++start;
    }
    if (start >= text.size()) {
      break;
    }
    std::size_t end = start;
    while (end < text.size() && !IsAsciiWhitespace(text[end])) {
      ++end;
    }
    tokens.emplace_back(text.substr(start, end - start));
    start = end;
  }
  return tokens;
}

std::string JoinPrefixTokens(const std::vector<std::string>& tokens, std::size_t count) {
  if (count == 0 || tokens.empty()) {
    return {};
  }
  std::string out = tokens[0];
  for (std::size_t i = 1; i < count && i < tokens.size(); ++i) {
    out.push_back(' ');
    out.append(tokens[i]);
  }
  return out;
}

std::string BuildSurrogateText(std::uint64_t frame_id) {
  return "frame " + std::to_string(frame_id);
}

std::string TruncateBytes(const std::string& text, int max_bytes) {
  if (max_bytes <= 0) {
    return {};
  }
  const auto limit = static_cast<std::size_t>(max_bytes);
  if (text.size() <= limit) {
    return text;
  }
  return text.substr(0, limit);
}

bool ScoreLess(const SearchResult& lhs, const SearchResult& rhs) {
  const float lhs_score = std::isnan(lhs.score) ? 0.0F : lhs.score;
  const float rhs_score = std::isnan(rhs.score) ? 0.0F : rhs.score;
  if (lhs_score != rhs_score) {
    return lhs_score > rhs_score;
  }
  return lhs.frame_id < rhs.frame_id;
}

std::vector<SearchSource> NormalizeSources(std::vector<SearchSource> sources) {
  std::sort(sources.begin(), sources.end(), [](const auto lhs, const auto rhs) {
    return static_cast<int>(lhs) < static_cast<int>(rhs);
  });
  sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
  return sources;
}

bool PreferPreviewText(const std::optional<std::string>& candidate, const std::optional<std::string>& current) {
  if (!candidate.has_value()) {
    return false;
  }
  if (!current.has_value()) {
    return true;
  }
  // Deterministic tie-break for equal-score duplicate frame entries.
  return *candidate < *current;
}

float ClampAlpha(float alpha) {
  return std::min(1.0F, std::max(0.0F, alpha));
}

std::vector<SearchResult> MergeDuplicateFrameResults(std::vector<SearchResult> results) {
  struct ChannelAggregate {
    float best_score = 0.0F;
    std::optional<std::string> preview_text{};
    std::unordered_set<SearchSource> sources{};
    bool seen = false;
  };

  std::unordered_map<std::uint64_t, ChannelAggregate> by_frame{};
  by_frame.reserve(results.size());
  for (const auto& result : results) {
    auto& agg = by_frame[result.frame_id];
    const float normalized_score = std::isnan(result.score) ? 0.0F : result.score;
    if (!agg.seen || normalized_score > agg.best_score) {
      agg.best_score = normalized_score;
      agg.preview_text = result.preview_text;
      agg.seen = true;
    } else if (normalized_score == agg.best_score && PreferPreviewText(result.preview_text, agg.preview_text)) {
      agg.preview_text = result.preview_text;
    }
    for (const auto source : result.sources) {
      agg.sources.insert(source);
    }
  }

  results.clear();
  results.reserve(by_frame.size());
  for (auto& [frame_id, agg] : by_frame) {
    SearchResult merged{};
    merged.frame_id = frame_id;
    merged.score = agg.best_score;
    merged.preview_text = std::move(agg.preview_text);
    merged.sources.assign(agg.sources.begin(), agg.sources.end());
    std::sort(merged.sources.begin(), merged.sources.end(), [](const auto lhs, const auto rhs) {
      return static_cast<int>(lhs) < static_cast<int>(rhs);
    });
    results.push_back(std::move(merged));
  }
  return results;
}

std::vector<SearchResult> SortedChannel(std::vector<SearchResult> results, int top_k) {
  results = MergeDuplicateFrameResults(std::move(results));
  std::sort(results.begin(), results.end(), ScoreLess);
  if (top_k > 0 && results.size() > static_cast<std::size_t>(top_k)) {
    results.resize(static_cast<std::size_t>(top_k));
  }
  return results;
}

SearchResponse BuildSingleChannelResponse(std::vector<SearchResult> results, int top_k) {
  SearchResponse out{};
  out.results = SortedChannel(std::move(results), top_k);
  return out;
}

SearchResponse BuildHybridRrfResponse(const SearchRequest& request,
                                      std::vector<SearchResult> text_results,
                                      std::vector<SearchResult> vector_results) {
  const auto sorted_text = SortedChannel(std::move(text_results), request.top_k);
  const auto sorted_vector = SortedChannel(std::move(vector_results), request.top_k);

  struct Aggregate {
    float score = 0.0F;
    std::optional<std::string> preview_text;
    std::unordered_set<SearchSource> sources;
  };
  std::unordered_map<std::uint64_t, Aggregate> aggregates{};
  const float alpha = ClampAlpha(request.mode.alpha);
  const float text_weight = alpha;
  const float vector_weight = 1.0F - alpha;
  const float base = static_cast<float>(std::max(0, request.rrf_k));

  auto apply_channel = [&](const std::vector<SearchResult>& channel, float weight) {
    for (std::size_t i = 0; i < channel.size(); ++i) {
      const auto rank = static_cast<float>(i + 1U);
      const auto contribution = weight * (1.0F / (base + rank));
      auto& agg = aggregates[channel[i].frame_id];
      agg.score += contribution;
      if (!agg.preview_text.has_value() && channel[i].preview_text.has_value()) {
        agg.preview_text = channel[i].preview_text;
      }
      for (const auto source : channel[i].sources) {
        agg.sources.insert(source);
      }
    }
  };

  if (text_weight > 0.0F) {
    apply_channel(sorted_text, text_weight);
  }
  if (vector_weight > 0.0F) {
    apply_channel(sorted_vector, vector_weight);
  }

  SearchResponse out{};
  out.results.reserve(aggregates.size());
  for (const auto& [frame_id, agg] : aggregates) {
    SearchResult item{};
    item.frame_id = frame_id;
    item.score = agg.score;
    item.preview_text = agg.preview_text;
    item.sources.assign(agg.sources.begin(), agg.sources.end());
    std::sort(item.sources.begin(), item.sources.end(), [](const auto lhs, const auto rhs) {
      return static_cast<int>(lhs) < static_cast<int>(rhs);
    });
    out.results.push_back(std::move(item));
  }

  std::sort(out.results.begin(), out.results.end(), ScoreLess);
  if (request.top_k > 0 && out.results.size() > static_cast<std::size_t>(request.top_k)) {
    out.results.resize(static_cast<std::size_t>(request.top_k));
  }
  return out;
}

}  // namespace

SearchResponse UnifiedSearch(const SearchRequest& request) {
  return UnifiedSearchWithCandidates(request, {}, {});
}

SearchResponse UnifiedSearchWithCandidates(const SearchRequest& request,
                                           const std::vector<SearchResult>& text_results,
                                           const std::vector<SearchResult>& vector_results) {
  if (request.top_k <= 0) {
    return {};
  }

  switch (request.mode.kind) {
    case SearchModeKind::kTextOnly:
      return BuildSingleChannelResponse(text_results, request.top_k);
    case SearchModeKind::kVectorOnly:
      return BuildSingleChannelResponse(vector_results, request.top_k);
    case SearchModeKind::kHybrid:
      return BuildHybridRrfResponse(request, text_results, vector_results);
  }
  return {};
}

RAGContext BuildFastRAGContext(const SearchRequest& request, const SearchResponse& response) {
  RAGContext context;
  if (request.query.has_value()) {
    context.query = *request.query;
  }

  const int clamped_top_k = std::max(0, request.top_k);
  const int clamped_max_snippets = std::max(0, request.max_snippets);
  const int clamped_max_context_tokens = std::max(0, request.max_context_tokens);
  const int clamped_snippet_max_tokens = std::max(0, request.snippet_max_tokens);
  const int clamped_expansion_max_tokens =
      std::min(std::max(0, request.expansion_max_tokens), clamped_max_context_tokens);

  if (clamped_top_k == 0 || clamped_max_context_tokens == 0) {
    context.total_tokens = 0;
    return context;
  }

  std::vector<SearchResult> sorted_results = MergeDuplicateFrameResults(response.results);
  std::sort(sorted_results.begin(), sorted_results.end(), ScoreLess);
  if (sorted_results.size() > static_cast<std::size_t>(clamped_top_k)) {
    sorted_results.resize(static_cast<std::size_t>(clamped_top_k));
  }

  context.total_tokens = 0;
  context.items.reserve(sorted_results.size());
  int emitted_snippets = 0;
  const bool expansion_enabled = clamped_expansion_max_tokens > 0;
  for (const auto& result : sorted_results) {
    const bool is_first_item = context.items.empty();
    const bool use_expanded_tier = expansion_enabled && is_first_item;
    auto item_kind = use_expanded_tier ? RAGItemKind::kExpanded : RAGItemKind::kSnippet;
    const bool counts_towards_snippet_cap = !use_expanded_tier;

    if (counts_towards_snippet_cap && emitted_snippets >= clamped_max_snippets) {
      continue;
    }

    std::string candidate_text{};
    if (result.preview_text.has_value()) {
      candidate_text = TruncateBytes(*result.preview_text, request.preview_max_bytes);
    }
    if (candidate_text.empty()) {
      item_kind = RAGItemKind::kSurrogate;
      candidate_text = BuildSurrogateText(result.frame_id);
    }

    auto tokens = SplitWhitespaceTokens(candidate_text);
    if (tokens.empty()) {
      continue;
    }
    const int configured_limit = use_expanded_tier ? clamped_expansion_max_tokens : clamped_snippet_max_tokens;
    if (configured_limit == 0) {
      continue;
    }
    const std::size_t per_item_limit =
        configured_limit > 0 ? std::min<std::size_t>(tokens.size(), static_cast<std::size_t>(configured_limit))
                             : tokens.size();
    std::size_t emit_tokens = per_item_limit;
    const int remaining = clamped_max_context_tokens - context.total_tokens;
    if (remaining <= 0) {
      break;
    }
    emit_tokens = std::min<std::size_t>(emit_tokens, static_cast<std::size_t>(remaining));
    if (emit_tokens == 0) {
      break;
    }

    RAGItem item{};
    item.kind = item_kind;
    item.frame_id = result.frame_id;
    item.score = std::isnan(result.score) ? 0.0F : result.score;
    item.sources = NormalizeSources(result.sources);
    item.text = JoinPrefixTokens(tokens, emit_tokens);
    context.total_tokens += static_cast<int>(emit_tokens);
    if (counts_towards_snippet_cap) {
      ++emitted_snippets;
    }
    context.items.push_back(std::move(item));
    if (context.total_tokens >= clamped_max_context_tokens) {
      break;
    }
  }
  return context;
}

}  // namespace waxcpp
