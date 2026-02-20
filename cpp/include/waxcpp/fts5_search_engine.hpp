#pragma once

#include "waxcpp/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace waxcpp {

class FTS5SearchEngine {
 public:
  FTS5SearchEngine();

  void Index(std::uint64_t frame_id, const std::string& text);
  void IndexBatch(const std::vector<std::uint64_t>& frame_ids, const std::vector<std::string>& texts);
  void Remove(std::uint64_t frame_id);
  std::vector<SearchResult> Search(const std::string& query, int top_k);
};

}  // namespace waxcpp
