#include "waxcpp/search.hpp"

namespace waxcpp {

SearchResponse UnifiedSearch(const SearchRequest& /*request*/) {
  return {};
}

RAGContext BuildFastRAGContext(const SearchRequest& request, const SearchResponse& response) {
  RAGContext context;
  if (request.query.has_value()) {
    context.query = *request.query;
  }
  context.total_tokens = 0;
  context.items.reserve(response.results.size());
  return context;
}

}  // namespace waxcpp
