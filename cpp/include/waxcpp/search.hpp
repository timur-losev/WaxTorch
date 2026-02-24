#pragma once

#include "waxcpp/query_analyzer.hpp"
#include "waxcpp/types.hpp"

namespace waxcpp {

SearchResponse UnifiedSearch(const SearchRequest& request);
SearchResponse UnifiedSearchWithCandidates(const SearchRequest& request,
                                           const std::vector<SearchResult>& text_results,
                                           const std::vector<SearchResult>& vector_results);

/// Adaptive search: classifies query, selects fusion weights, then runs search.
/// Uses AdaptiveFusionConfig to determine alpha based on query type.
SearchResponse UnifiedSearchAdaptive(const SearchRequest& request,
                                     const std::vector<SearchResult>& text_results,
                                     const std::vector<SearchResult>& vector_results,
                                     const AdaptiveFusionConfig& fusion_config = AdaptiveFusionConfig::Default());

RAGContext BuildFastRAGContext(const SearchRequest& request, const SearchResponse& response);

}  // namespace waxcpp
