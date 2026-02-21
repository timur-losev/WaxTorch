#include "waxcpp/search.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <string_view>
#include <vector>

namespace waxcpp {
namespace {

int CountWhitespaceTokens(std::string_view text) {
  int tokens = 0;
  bool in_token = false;
  for (const unsigned char ch : text) {
    if (std::isspace(ch) != 0) {
      in_token = false;
      continue;
    }
    if (!in_token) {
      ++tokens;
      in_token = true;
    }
  }
  return tokens;
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
  for (const auto& result : sorted_results) {
    if (!result.preview_text.has_value()) {
      continue;
    }
    const auto text = TruncateBytes(*result.preview_text, request.preview_max_bytes);
    if (text.empty()) {
      continue;
    }

    RAGItem item{};
    item.kind = RAGItemKind::kSnippet;
    item.frame_id = result.frame_id;
    item.score = std::isnan(result.score) ? 0.0F : result.score;
    item.sources = result.sources;
    item.text = text;
    context.total_tokens += CountWhitespaceTokens(item.text);
    context.items.push_back(std::move(item));
  }
  return context;
}

}  // namespace waxcpp
