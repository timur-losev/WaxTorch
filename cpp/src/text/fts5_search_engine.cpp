#include "waxcpp/fts5_search_engine.hpp"

#include <stdexcept>

namespace waxcpp {

FTS5SearchEngine::FTS5SearchEngine() = default;

void FTS5SearchEngine::Index(std::uint64_t /*frame_id*/, const std::string& /*text*/) {
  throw std::runtime_error("FTS5SearchEngine::Index not implemented");
}

void FTS5SearchEngine::IndexBatch(const std::vector<std::uint64_t>& /*frame_ids*/,
                                  const std::vector<std::string>& /*texts*/) {
  throw std::runtime_error("FTS5SearchEngine::IndexBatch not implemented");
}

void FTS5SearchEngine::Remove(std::uint64_t /*frame_id*/) {
  throw std::runtime_error("FTS5SearchEngine::Remove not implemented");
}

std::vector<SearchResult> FTS5SearchEngine::Search(const std::string& /*query*/, int /*top_k*/) {
  throw std::runtime_error("FTS5SearchEngine::Search not implemented");
}

}  // namespace waxcpp
