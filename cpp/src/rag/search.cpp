#include "waxcpp/search.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string_view>
#include <vector>

namespace waxcpp {
namespace {

std::vector<std::string> SplitWhitespaceTokens(std::string_view text) {
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

float ClampAlpha(float alpha) {
  return std::max(0.0F, std::min(1.0F, alpha));
}

std::vector<SearchResult> SortedChannel(std::vector<SearchResult> results, int top_k) {
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
  const float base = static_cast<float>(request.rrf_k <= 0 ? 60 : request.rrf_k);

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

  apply_channel(sorted_text, text_weight);
  apply_channel(sorted_vector, vector_weight);

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

  std::vector<SearchResult> sorted_results = response.results;
  std::sort(sorted_results.begin(), sorted_results.end(), ScoreLess);
  if (request.top_k > 0 && sorted_results.size() > static_cast<std::size_t>(request.top_k)) {
    sorted_results.resize(static_cast<std::size_t>(request.top_k));
  }

  context.total_tokens = 0;
  context.items.reserve(sorted_results.size());
  const int max_context_tokens = request.max_context_tokens;
  const int snippet_max_tokens = request.snippet_max_tokens;
  for (const auto& result : sorted_results) {
    if (!result.preview_text.has_value()) {
      continue;
    }
    const auto text = TruncateBytes(*result.preview_text, request.preview_max_bytes);
    if (text.empty()) {
      continue;
    }
    auto tokens = SplitWhitespaceTokens(text);
    if (tokens.empty()) {
      continue;
    }

    const std::size_t per_item_limit = snippet_max_tokens > 0
                                           ? std::min<std::size_t>(tokens.size(), static_cast<std::size_t>(snippet_max_tokens))
                                           : tokens.size();
    std::size_t emit_tokens = per_item_limit;
    if (max_context_tokens > 0) {
      const int remaining = max_context_tokens - context.total_tokens;
      if (remaining <= 0) {
        break;
      }
      emit_tokens = std::min<std::size_t>(emit_tokens, static_cast<std::size_t>(remaining));
      if (emit_tokens == 0) {
        break;
      }
    }

    RAGItem item{};
    item.kind = RAGItemKind::kSnippet;
    item.frame_id = result.frame_id;
    item.score = std::isnan(result.score) ? 0.0F : result.score;
    item.sources = result.sources;
    item.text = JoinPrefixTokens(tokens, emit_tokens);
    context.total_tokens += static_cast<int>(emit_tokens);
    context.items.push_back(std::move(item));
    if (max_context_tokens > 0 && context.total_tokens >= max_context_tokens) {
      break;
    }
  }
  return context;
}

}  // namespace waxcpp
