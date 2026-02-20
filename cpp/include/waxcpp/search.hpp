#pragma once

#include "waxcpp/types.hpp"

namespace waxcpp {

SearchResponse UnifiedSearch(const SearchRequest& request);
RAGContext BuildFastRAGContext(const SearchRequest& request, const SearchResponse& response);

}  // namespace waxcpp
