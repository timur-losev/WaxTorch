#include "waxcpp/search.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

}  // namespace

SearchResponse UnifiedSearch(const SearchRequest& /*request*/) {
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
