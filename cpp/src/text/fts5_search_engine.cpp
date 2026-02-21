#include "waxcpp/fts5_search_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <string_view>

namespace waxcpp {
namespace {

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens{};
  std::string current{};
  current.reserve(32);

  for (const unsigned char ch : text) {
    if (std::isalnum(ch) != 0) {
      current.push_back(static_cast<char>(std::tolower(ch)));
      continue;
    }
    if (!current.empty()) {
      tokens.push_back(std::move(current));
      current.clear();
      current.reserve(32);
    }
  }
  if (!current.empty()) {
    tokens.push_back(std::move(current));
  }
  return tokens;
}

std::unordered_map<std::string, std::uint32_t> TokenFreq(std::string_view text) {
  std::unordered_map<std::string, std::uint32_t> freq{};
  for (auto token : Tokenize(text)) {
    auto it = freq.find(token);
    if (it == freq.end()) {
      freq.emplace(std::move(token), 1U);
    } else {
      it->second += 1U;
    }
  }
  return freq;
}

}  // namespace

FTS5SearchEngine::FTS5SearchEngine() = default;

void FTS5SearchEngine::Index(std::uint64_t frame_id, const std::string& text) {
  docs_[frame_id] = text;
}

void FTS5SearchEngine::IndexBatch(const std::vector<std::uint64_t>& frame_ids,
                                  const std::vector<std::string>& texts) {
  if (frame_ids.size() != texts.size()) {
    throw std::runtime_error("FTS5SearchEngine::IndexBatch size mismatch");
  }
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    docs_[frame_ids[i]] = texts[i];
  }
}

void FTS5SearchEngine::Remove(std::uint64_t frame_id) {
  docs_.erase(frame_id);
}

std::vector<SearchResult> FTS5SearchEngine::Search(const std::string& query, int top_k) {
  if (top_k <= 0 || docs_.empty()) {
    return {};
  }
  const auto query_tokens_raw = Tokenize(query);
  if (query_tokens_raw.empty()) {
    return {};
  }

  std::unordered_set<std::string> unique_query_tokens{};
  unique_query_tokens.reserve(query_tokens_raw.size());
  for (const auto& token : query_tokens_raw) {
    unique_query_tokens.insert(token);
  }

  std::unordered_map<std::string, std::uint32_t> doc_freq{};
  doc_freq.reserve(unique_query_tokens.size());

  std::vector<std::pair<std::uint64_t, std::unordered_map<std::string, std::uint32_t>>> doc_freq_maps{};
  doc_freq_maps.reserve(docs_.size());

  for (const auto& [frame_id, text] : docs_) {
    auto freq = TokenFreq(text);
    for (const auto& token : unique_query_tokens) {
      if (freq.find(token) != freq.end()) {
        doc_freq[token] += 1U;
      }
    }
    doc_freq_maps.emplace_back(frame_id, std::move(freq));
  }

  const double doc_count = static_cast<double>(docs_.size());
  std::vector<SearchResult> results{};
  results.reserve(docs_.size());

  for (const auto& [frame_id, freq] : doc_freq_maps) {
    double score = 0.0;
    for (const auto& token : query_tokens_raw) {
      const auto tf_it = freq.find(token);
      if (tf_it == freq.end()) {
        continue;
      }
      const auto df_it = doc_freq.find(token);
      const double df = (df_it == doc_freq.end()) ? 0.0 : static_cast<double>(df_it->second);
      const double idf = std::log((doc_count + 1.0) / (df + 1.0)) + 1.0;
      score += static_cast<double>(tf_it->second) * idf;
    }
    if (score <= 0.0) {
      continue;
    }
    SearchResult result{};
    result.frame_id = frame_id;
    result.score = static_cast<float>(score);
    result.preview_text = docs_[frame_id];
    result.sources = {SearchSource::kText};
    results.push_back(std::move(result));
  }

  std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.score != rhs.score) {
      return lhs.score > rhs.score;
    }
    return lhs.frame_id < rhs.frame_id;
  });

  if (results.size() > static_cast<std::size_t>(top_k)) {
    results.resize(static_cast<std::size_t>(top_k));
  }
  return results;
}

}  // namespace waxcpp
