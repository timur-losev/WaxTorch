#pragma once

#include "waxcpp/types.hpp"

namespace waxcpp {

SearchResponse UnifiedSearch(const SearchRequest& request);
SearchResponse UnifiedSearchWithCandidates(const SearchRequest& request,
                                           const std::vector<SearchResult>& text_results,
                                           const std::vector<SearchResult>& vector_results);
RAGContext BuildFastRAGContext(const SearchRequest& request, const SearchResponse& response);

}  // namespace waxcpp
