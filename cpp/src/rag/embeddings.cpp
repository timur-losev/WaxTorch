#include "waxcpp/embeddings.hpp"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace waxcpp {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::string ToAsciiLowerString(std::string_view text) {
  std::string out{};
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch >= 'A' && ch <= 'Z') {
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::optional<std::string> GetEnvValue(const char* name) {
#ifdef _WIN32
  char* raw = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&raw, &len, name) != 0 || raw == nullptr) {
    return std::nullopt;
  }
  std::string value(raw);
  std::free(raw);
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
#else
  const char* raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return std::nullopt;
  }
  return std::string(raw);
#endif
}

bool EnvIsTruthy(const char* name) {
  const auto raw = GetEnvValue(name);
  if (!raw.has_value()) {
    return false;
  }
  const auto value = ToAsciiLowerString(*raw);
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::optional<std::filesystem::path> ResolveLibTorchManifestPath(bool* override_was_set = nullptr) {
  if (override_was_set != nullptr) {
    *override_was_set = false;
  }

  if (const auto raw_override = GetEnvValue("WAXCPP_LIBTORCH_MANIFEST"); raw_override.has_value()) {
    if (override_was_set != nullptr) {
      *override_was_set = true;
    }
    const std::filesystem::path candidate(*raw_override);
    if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::absolute(candidate);
    }
    return std::nullopt;
  }

  const auto cwd = std::filesystem::current_path();
  const std::vector<std::filesystem::path> candidates = {
      cwd / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
      cwd / ".." / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
      cwd / ".." / ".." / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
      cwd / "cpp" / "third_party" / "libtorch-dist" / "manifest" / "libtorch-manifest.json",
  };
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate)) {
      return std::filesystem::absolute(candidate);
    }
  }
  return std::nullopt;
}

bool IsAsciiAlphaNum(unsigned char ch) {
  return (ch >= static_cast<unsigned char>('0') && ch <= static_cast<unsigned char>('9')) ||
         (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')) ||
         (ch >= static_cast<unsigned char>('a') && ch <= static_cast<unsigned char>('z'));
}

char ToAsciiLower(unsigned char ch) {
  if (ch >= static_cast<unsigned char>('A') && ch <= static_cast<unsigned char>('Z')) {
    return static_cast<char>(ch - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
  }
  return static_cast<char>(ch);
}

std::vector<std::string> Tokenize(std::string_view text) {
  std::vector<std::string> tokens{};
  std::string current{};
  current.reserve(32);

  for (const unsigned char ch : text) {
    if (IsAsciiAlphaNum(ch)) {
      current.push_back(ToAsciiLower(ch));
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
    : memoization_capacity_(memoization_capacity) {
  bool override_was_set = false;
  const auto manifest_path = ResolveLibTorchManifestPath(&override_was_set);
  if (manifest_path.has_value()) {
    runtime_info_.libtorch_manifest_detected = true;
    runtime_info_.libtorch_manifest_path = manifest_path->string();
  }
  if (EnvIsTruthy("WAXCPP_REQUIRE_LIBTORCH_MANIFEST") && !runtime_info_.libtorch_manifest_detected) {
    if (override_was_set) {
      throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest is missing at WAXCPP_LIBTORCH_MANIFEST");
    }
    throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest was not found");
  }
}

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
    std::lock_guard<std::mutex> lock(memoization_mutex_);
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
    std::lock_guard<std::mutex> lock(memoization_mutex_);
    const auto cached = memoized_embeddings_.find(text);
    if (cached != memoized_embeddings_.end()) {
      return cached->second;
    }
    while (memoized_embeddings_.size() >= memoization_capacity_ && !memoization_order_.empty()) {
      const auto evict_key = memoization_order_.front();
      memoization_order_.pop_front();
      (void)memoized_embeddings_.erase(evict_key);
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
  std::lock_guard<std::mutex> lock(memoization_mutex_);
  return memoized_embeddings_.size();
}

MiniLMRuntimeInfo MiniLMEmbedderTorch::runtime_info() const {
  return runtime_info_;
}

}  // namespace waxcpp
