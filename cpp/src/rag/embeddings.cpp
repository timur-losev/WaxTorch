#include "waxcpp/embeddings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace waxcpp {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

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
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

std::uint64_t HashToken(std::string_view token) {
  std::uint64_t hash = kFnvOffset;
  for (const unsigned char ch : token) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

void NormalizeL2(std::vector<float>& v) {
  double sum_sq = 0.0;
  for (const auto x : v) {
    sum_sq += static_cast<double>(x) * static_cast<double>(x);
  }
  if (sum_sq <= 0.0) {
    return;
  }
  const auto inv_norm = 1.0 / std::sqrt(sum_sq);
  for (auto& x : v) {
    x = static_cast<float>(static_cast<double>(x) * inv_norm);
  }
}

}  // namespace

MiniLMEmbedderTorch::MiniLMEmbedderTorch(std::size_t memoization_capacity)
    : memoization_capacity_(memoization_capacity) {}

int MiniLMEmbedderTorch::dimensions() const {
  return 384;
}

bool MiniLMEmbedderTorch::normalize() const {
  return true;
}

std::optional<EmbeddingIdentity> MiniLMEmbedderTorch::identity() const {
  return EmbeddingIdentity{
      .provider = std::string("WaxCpp"),
      .model = std::string("MiniLM-Torch"),
      .dimensions = 384,
      .normalized = true,
  };
}

std::vector<float> MiniLMEmbedderTorch::Embed(const std::string& text) {
  if (memoization_capacity_ > 0) {
    const auto cached = memoized_embeddings_.find(text);
    if (cached != memoized_embeddings_.end()) {
      return cached->second;
    }
  }

  constexpr int kDims = 384;
  std::vector<float> embedding(static_cast<std::size_t>(kDims), 0.0F);

  const auto tokens = Tokenize(text);
  for (const auto& token : tokens) {
    const auto hash = HashToken(token);
    const auto index = static_cast<std::size_t>(hash % static_cast<std::uint64_t>(kDims));
    const float sign = ((hash >> 63U) != 0U) ? -1.0F : 1.0F;
    embedding[index] += sign;
  }

  if (normalize()) {
    NormalizeL2(embedding);
  }

  if (memoization_capacity_ > 0) {
    while (memoized_embeddings_.size() >= memoization_capacity_ && !memoization_order_.empty()) {
      const auto& key = memoization_order_.front();
      memoized_embeddings_.erase(key);
      memoization_order_.pop_front();
    }
    memoization_order_.push_back(text);
    memoized_embeddings_[text] = embedding;
  }

  return embedding;
}

std::vector<std::vector<float>> MiniLMEmbedderTorch::EmbedBatch(const std::vector<std::string>& texts) {
  std::vector<std::vector<float>> out{};
  out.reserve(texts.size());
  for (const auto& text : texts) {
    out.push_back(Embed(text));
  }
  return out;
}

std::size_t MiniLMEmbedderTorch::cache_size() const {
  return memoized_embeddings_.size();
}

}  // namespace waxcpp
