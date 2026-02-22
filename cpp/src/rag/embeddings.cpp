#include "waxcpp/embeddings.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
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

bool IsAsciiHex(char ch) {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
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

std::string_view TrimAsciiWhitespace(std::string_view text) {
  std::size_t begin = 0;
  while (begin < text.size()) {
    const char ch = text[begin];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      ++begin;
      continue;
    }
    break;
  }

  std::size_t end = text.size();
  while (end > begin) {
    const char ch = text[end - 1];
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
      --end;
      continue;
    }
    break;
  }

  return text.substr(begin, end - begin);
}

std::optional<std::pair<std::size_t, std::size_t>> FindDelimitedRange(
    std::string_view text,
    std::size_t open_index,
    char open_char,
    char close_char) {
  if (open_index >= text.size() || text[open_index] != open_char) {
    return std::nullopt;
  }

  bool in_string = false;
  bool escaped = false;
  int depth = 0;
  for (std::size_t i = open_index; i < text.size(); ++i) {
    const char ch = text[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == open_char) {
      ++depth;
      continue;
    }
    if (ch == close_char) {
      --depth;
      if (depth == 0) {
        return std::make_pair(open_index, i);
      }
      if (depth < 0) {
        return std::nullopt;
      }
    }
  }
  return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> FindArtifactArrayRange(std::string_view json) {
  if (json.empty()) {
    return std::nullopt;
  }
  if (json.front() == '[') {
    return FindDelimitedRange(json, 0, '[', ']');
  }

  constexpr std::array<std::string_view, 3> kArtifactKeys = {
      "\"artifacts\"",
      "\"files\"",
      "\"entries\"",
  };

  for (const auto key : kArtifactKeys) {
    std::size_t cursor = 0;
    while (cursor < json.size()) {
      const auto key_pos = json.find(key, cursor);
      if (key_pos == std::string_view::npos) {
        break;
      }
      const auto colon = json.find(':', key_pos + key.size());
      if (colon == std::string_view::npos) {
        break;
      }
      const auto array_open = json.find('[', colon + 1);
      if (array_open == std::string_view::npos) {
        break;
      }
      if (const auto range = FindDelimitedRange(json, array_open, '[', ']'); range.has_value()) {
        return range;
      }
      cursor = key_pos + key.size();
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> ExtractJsonStringField(std::string_view object,
                                                       std::string_view key_a,
                                                       std::string_view key_b) {
  if (object.size() < 2 || object.front() != '{' || object.back() != '}') {
    return std::nullopt;
  }

  auto parse_string = [&](std::size_t open_quote_index) -> std::optional<std::pair<std::string_view, std::size_t>> {
    if (open_quote_index >= object.size() || object[open_quote_index] != '"') {
      return std::nullopt;
    }
    const auto open_quote = open_quote_index;
    bool escaped_inner = false;
    for (std::size_t i = open_quote + 1; i < object.size(); ++i) {
      const char ch = object[i];
      if (escaped_inner) {
        escaped_inner = false;
        continue;
      }
      if (ch == '\\') {
        escaped_inner = true;
        continue;
      }
      if (ch == '"') {
        if (i == open_quote + 1) {
          return std::nullopt;
        }
        return std::make_pair(object.substr(open_quote + 1, i - open_quote - 1), i);
      }
    }
    return std::nullopt;
  };

  auto key_matches = [&](std::string_view parsed_key) -> bool {
    const auto quoted = std::string("\"") + std::string(parsed_key) + std::string("\"");
    return quoted == key_a || quoted == key_b;
  };

  int nested_depth = 0;
  std::size_t i = 1;
  bool escaped = false;
  bool in_string = false;
  while (i + 1 < object.size()) {
    const char ch = object[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
        ++i;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        ++i;
        continue;
      }
      if (ch == '"') {
        in_string = false;
      }
      ++i;
      continue;
    }

    if (ch == '"') {
      if (nested_depth != 0) {
        in_string = true;
        ++i;
        continue;
      }

      const auto key = parse_string(i);
      if (!key.has_value()) {
        ++i;
        continue;
      }
      const auto [parsed_key, key_end] = *key;
      i = key_end + 1;
      while (i < object.size()) {
        const char ws = object[i];
        if (ws == ' ' || ws == '\t' || ws == '\r' || ws == '\n') {
          ++i;
          continue;
        }
        break;
      }
      if (i >= object.size() || object[i] != ':') {
        continue;
      }
      ++i;
      while (i < object.size()) {
        const char ws = object[i];
        if (ws == ' ' || ws == '\t' || ws == '\r' || ws == '\n') {
          ++i;
          continue;
        }
        break;
      }
      if (!key_matches(parsed_key)) {
        continue;
      }
      if (i >= object.size() || object[i] != '"') {
        return std::nullopt;
      }
      const auto value = parse_string(i);
      if (!value.has_value() || value->first.empty()) {
        return std::nullopt;
      }
      return value->first;
    }

    if (ch == '{' || ch == '[') {
      ++nested_depth;
      ++i;
      continue;
    }
    if (ch == '}' || ch == ']') {
      if (nested_depth > 0) {
        --nested_depth;
      }
      ++i;
      continue;
    }
    ++i;
  }

  return std::nullopt;
}

bool HasNonEmptyArtifactPath(std::string_view object) {
  const auto path = ExtractJsonStringField(object, "\"path\"", "\"file\"");
  return path.has_value() && !path->empty();
}

bool HasValidSha256(std::string_view object) {
  const auto sha = ExtractJsonStringField(object, "\"sha256\"", "\"sha256sum\"");
  if (!sha.has_value() || sha->size() != 64) {
    return false;
  }
  for (const char ch : *sha) {
    if (!IsAsciiHex(ch)) {
      return false;
    }
  }
  return true;
}

std::size_t CountValidArtifactObjects(std::string_view json,
                                      const std::pair<std::size_t, std::size_t>& array_range) {
  const std::size_t begin = array_range.first + 1;
  const std::size_t end = array_range.second;
  if (begin >= end || end > json.size()) {
    return 0;
  }

  std::size_t valid = 0;
  bool in_string = false;
  bool escaped = false;
  int brace_depth = 0;
  std::size_t object_begin = std::string_view::npos;
  for (std::size_t i = begin; i < end; ++i) {
    const char ch = json[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (ch == '\\') {
        escaped = true;
        continue;
      }
      if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '{') {
      if (brace_depth == 0) {
        object_begin = i;
      }
      ++brace_depth;
      continue;
    }
    if (ch == '}') {
      if (brace_depth <= 0) {
        continue;
      }
      --brace_depth;
      if (brace_depth == 0 && object_begin != std::string_view::npos && i > object_begin) {
        const auto artifact_object = json.substr(object_begin, i - object_begin + 1);
        if (HasNonEmptyArtifactPath(artifact_object) && HasValidSha256(artifact_object)) {
          ++valid;
        }
      }
    }
  }
  return valid;
}

std::size_t ValidateManifestFile(const std::filesystem::path& manifest_path) {
  constexpr std::uintmax_t kMaxManifestBytes = 8U * 1024U * 1024U;
  std::error_code ec{};
  const auto size = std::filesystem::file_size(manifest_path, ec);
  if (ec) {
    throw std::runtime_error("failed to read libtorch manifest size");
  }
  if (size == 0) {
    throw std::runtime_error("libtorch manifest is empty");
  }
  if (size > kMaxManifestBytes) {
    throw std::runtime_error("libtorch manifest exceeds size limit");
  }

  std::ifstream input(manifest_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("failed to open libtorch manifest");
  }
  std::string content(static_cast<std::size_t>(size), '\0');
  if (!input.read(content.data(), static_cast<std::streamsize>(content.size()))) {
    throw std::runtime_error("failed to read libtorch manifest");
  }

  const auto trimmed = TrimAsciiWhitespace(content);
  if (trimmed.empty()) {
    throw std::runtime_error("libtorch manifest is blank");
  }
  const char first = trimmed.front();
  if (first != '{' && first != '[') {
    throw std::runtime_error("libtorch manifest does not look like JSON");
  }

  const bool has_artifact_list =
      trimmed.find("\"artifacts\"") != std::string_view::npos ||
      trimmed.find("\"files\"") != std::string_view::npos ||
      trimmed.find("\"entries\"") != std::string_view::npos ||
      first == '[';
  if (!has_artifact_list) {
    throw std::runtime_error("libtorch manifest does not define artifact list keys");
  }

  const bool has_path_key =
      trimmed.find("\"path\"") != std::string_view::npos ||
      trimmed.find("\"file\"") != std::string_view::npos;
  if (!has_path_key) {
    throw std::runtime_error("libtorch manifest does not contain artifact path keys");
  }

  const bool has_sha_key =
      trimmed.find("\"sha256\"") != std::string_view::npos ||
      trimmed.find("\"sha256sum\"") != std::string_view::npos;
  if (!has_sha_key) {
    throw std::runtime_error("libtorch manifest does not contain sha256 keys");
  }

  const auto artifacts_range = FindArtifactArrayRange(trimmed);
  if (!artifacts_range.has_value()) {
    throw std::runtime_error("libtorch manifest does not contain a parseable artifact array");
  }

  const auto valid_artifacts = CountValidArtifactObjects(trimmed, *artifacts_range);
  if (valid_artifacts == 0) {
    throw std::runtime_error("libtorch manifest does not contain artifact objects with path and valid sha256");
  }

  return valid_artifacts;
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
    try {
      runtime_info_.libtorch_manifest_artifact_count = ValidateManifestFile(*manifest_path);
      runtime_info_.libtorch_manifest_valid = true;
    } catch (const std::exception& ex) {
      throw std::runtime_error(std::string("MiniLMEmbedderTorch libtorch manifest is invalid: ") + ex.what());
    }
  }
  if (EnvIsTruthy("WAXCPP_REQUIRE_LIBTORCH_MANIFEST")) {
    if (!runtime_info_.libtorch_manifest_detected) {
      if (override_was_set) {
        throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest is missing at WAXCPP_LIBTORCH_MANIFEST");
      }
      throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest was not found");
    }
    if (!runtime_info_.libtorch_manifest_valid) {
      throw std::runtime_error("MiniLMEmbedderTorch required libtorch manifest is invalid");
    }
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
