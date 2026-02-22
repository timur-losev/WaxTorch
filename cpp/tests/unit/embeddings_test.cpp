#include "waxcpp/embeddings.hpp"

#include "../test_logger.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

double L2Norm(const std::vector<float>& values) {
  double sum = 0.0;
  for (const auto value : values) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  return std::sqrt(sum);
}

bool ApproxEqual(double lhs, double rhs, double eps) {
  return std::fabs(lhs - rhs) <= eps;
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

void SetEnvVar(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
  if (!value.has_value()) {
    (void)_putenv_s(name, "");
    return;
  }
  (void)_putenv_s(name, value->c_str());
#else
  if (!value.has_value()) {
    (void)unsetenv(name);
    return;
  }
  (void)setenv(name, value->c_str(), 1);
#endif
}

class ScopedEnvVar final {
 public:
  ScopedEnvVar(const char* name, std::optional<std::string> value) : name_(name) {
    original_ = GetEnvValue(name_);
    SetEnvVar(name_, value);
  }

  ~ScopedEnvVar() {
    SetEnvVar(name_, original_);
  }

 private:
  const char* name_;
  std::optional<std::string> original_{};
};

void ScenarioIdentityAndShape() {
  waxcpp::tests::Log("scenario: identity and shape");
  waxcpp::MiniLMEmbedderTorch embedder;
  Require(embedder.dimensions() == 384, "unexpected embedding dimension");
  Require(embedder.normalize(), "embedder should normalize vectors");
  const auto identity = embedder.identity();
  Require(identity.has_value(), "identity should be present");
  Require(identity->model.has_value() && *identity->model == "MiniLM-Torch", "identity model mismatch");
}

void ScenarioDeterministicEmbedding() {
  waxcpp::tests::Log("scenario: deterministic embedding");
  waxcpp::MiniLMEmbedderTorch embedder;
  const auto first = embedder.Embed("hello deterministic world");
  const auto second = embedder.Embed("hello deterministic world");
  const auto third = embedder.Embed("different content");

  Require(first.size() == static_cast<std::size_t>(embedder.dimensions()), "embedding size mismatch");
  Require(first == second, "same text should produce identical embedding");
  Require(first != third, "different text should produce different embedding");
}

void ScenarioNormalizationAndEmptyInput() {
  waxcpp::tests::Log("scenario: normalization and empty input");
  waxcpp::MiniLMEmbedderTorch embedder;
  const auto non_empty = embedder.Embed("alpha beta gamma");
  const auto norm = L2Norm(non_empty);
  Require(ApproxEqual(norm, 1.0, 1e-5), "non-empty embedding must be L2 normalized");

  const auto empty = embedder.Embed("");
  const auto empty_norm = L2Norm(empty);
  Require(ApproxEqual(empty_norm, 0.0, 1e-6), "empty embedding should stay zero vector");
}

void ScenarioBatchParity() {
  waxcpp::tests::Log("scenario: batch parity");
  waxcpp::MiniLMEmbedderTorch embedder;
  const std::vector<std::string> texts = {"first item", "second item", "third item"};
  const auto batch = embedder.EmbedBatch(texts);

  Require(batch.size() == texts.size(), "batch result size mismatch");
  for (std::size_t i = 0; i < texts.size(); ++i) {
    Require(batch[i] == embedder.Embed(texts[i]), "batch item must match single Embed output");
  }
}

void ScenarioMemoizationCapacity() {
  waxcpp::tests::Log("scenario: memoization capacity");
  waxcpp::MiniLMEmbedderTorch cached_embedder(2);
  const auto first_a = cached_embedder.Embed("alpha");
  const auto second_a = cached_embedder.Embed("alpha");
  Require(first_a == second_a, "memoized embedding must remain deterministic");
  Require(cached_embedder.cache_size() == 1, "repeated key should not increase cache size");

  (void)cached_embedder.Embed("beta");
  Require(cached_embedder.cache_size() == 2, "second unique key should fill cache");
  (void)cached_embedder.Embed("gamma");
  Require(cached_embedder.cache_size() == 2, "cache should enforce capacity bound");
  const auto third_a = cached_embedder.Embed("alpha");
  Require(third_a == first_a, "evicted key recomputation should remain deterministic");

  waxcpp::MiniLMEmbedderTorch uncached_embedder(0);
  (void)uncached_embedder.Embed("alpha");
  Require(uncached_embedder.cache_size() == 0, "zero-capacity embedder should not memoize");
}

void ScenarioAsciiTokenizationDeterminism() {
  waxcpp::tests::Log("scenario: ascii tokenization determinism");
  waxcpp::MiniLMEmbedderTorch embedder;
  const auto a = embedder.Embed("Alpha-42");
  const auto b = embedder.Embed("alpha 42");
  Require(a == b, "ASCII case-fold + delimiter tokenization should be deterministic");

  const auto c = embedder.Embed("alpha \xC3\xA9 42");
  const auto d = embedder.Embed("alpha 42");
  Require(c == d, "non-ASCII bytes should not perturb ASCII tokenization path");
}

void ScenarioRuntimeInfoAndManifestPolicy() {
  waxcpp::tests::Log("scenario: runtime info and manifest policy");
  const ScopedEnvVar clear_override("WAXCPP_LIBTORCH_MANIFEST", std::nullopt);
  const ScopedEnvVar clear_require("WAXCPP_REQUIRE_LIBTORCH_MANIFEST", std::nullopt);
  const ScopedEnvVar clear_runtime("WAXCPP_TORCH_RUNTIME", std::nullopt);

  {
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.fallback_active, "fallback backend should remain active in current build");
    Require(info.runtime_policy == "cpu_only", "default torch runtime policy should be cpu_only");
    Require(!info.cuda_preferred_requested, "default runtime should not request cuda");
    Require(!info.cuda_runtime_available, "cuda runtime should be unavailable in fallback build");
    Require(info.selected_backend == "fallback_cpu", "fallback backend should report fallback_cpu");
    if (info.libtorch_manifest_detected) {
      Require(info.libtorch_manifest_path.has_value(), "manifest path should be present when detected");
    }
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("cuda_preferred"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.runtime_policy == "cuda_preferred", "runtime policy should reflect cuda_preferred override");
    Require(info.cuda_preferred_requested, "cuda_preferred override should set request flag");
    Require(info.selected_backend == "fallback_cpu", "fallback build should keep fallback_cpu backend");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("CPU_ONLY"));
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.runtime_policy == "cpu_only", "runtime policy parsing should be case-insensitive");
    Require(!info.cuda_preferred_requested, "cpu_only should clear cuda request flag");
  }

  {
    const ScopedEnvVar set_runtime("WAXCPP_TORCH_RUNTIME", std::string("gpu_auto"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "invalid torch runtime policy should be rejected");
  }

  const auto temp_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_runtime_info.json";
  const auto empty_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_empty.json";
  const auto malformed_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_malformed.json";
  const auto bad_sha_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_bad_sha.json";
  const auto split_fields_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_split_fields.json";
  const auto nested_fields_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_nested_fields.json";
  const auto nested_plus_top_level_manifest =
      std::filesystem::temp_directory_path() / "waxcpp_test_libtorch_manifest_nested_plus_top_level.json";
  {
    std::ofstream out(temp_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create temp manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }
  {
    std::ofstream out(empty_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create empty manifest file");
    }
  }
  {
    std::ofstream out(malformed_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create malformed manifest file");
    }
    out << "not-a-json-manifest";
  }
  {
    std::ofstream out(bad_sha_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create bad-sha manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip","sha256":"1234"}]})";
  }
  {
    std::ofstream out(split_fields_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create split-fields manifest file");
    }
    out << R"({"artifacts":[{"path":"libtorch-cpu.zip"},{"sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }
  {
    std::ofstream out(nested_fields_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create nested-fields manifest file");
    }
    out << R"({"artifacts":[{"meta":{"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}}]})";
  }
  {
    std::ofstream out(nested_plus_top_level_manifest, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("failed to create nested-plus-top-level manifest file");
    }
    out << R"({"artifacts":[{"meta":{"path":"ignored-nested.zip","sha256":"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"},"path":"libtorch-cpu.zip","sha256":"0000000000000000000000000000000000000000000000000000000000000000"}]})";
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", temp_manifest.string());
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_detected, "manifest override should be detected");
    Require(info.libtorch_manifest_valid, "valid manifest override should pass validation");
    Require(info.libtorch_manifest_artifact_count > 0, "valid manifest should report artifact count");
    Require(info.libtorch_manifest_path.has_value(), "manifest override path should be preserved");
    Require(*info.libtorch_manifest_path == std::filesystem::absolute(temp_manifest).string(),
            "manifest override absolute path mismatch");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", empty_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "empty manifest should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", malformed_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "malformed manifest should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", bad_sha_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest with invalid sha256 should be rejected");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", split_fields_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest must reject artifacts where path and sha are split across different objects");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", nested_fields_manifest.string());
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "manifest must reject artifacts with path/sha only in nested fields");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", nested_plus_top_level_manifest.string());
    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();
    Require(info.libtorch_manifest_valid, "manifest should accept top-level path/sha even with nested metadata");
    Require(info.libtorch_manifest_artifact_count == 1, "expected one valid artifact object in nested-plus-top-level manifest");
  }

  {
    const ScopedEnvVar set_override("WAXCPP_LIBTORCH_MANIFEST", temp_manifest.string() + ".missing");
    const ScopedEnvVar require_manifest("WAXCPP_REQUIRE_LIBTORCH_MANIFEST", std::string("1"));
    bool threw = false;
    try {
      waxcpp::MiniLMEmbedderTorch embedder;
      (void)embedder;
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "required manifest policy should throw when override path is missing");
  }

  std::error_code ec;
  std::filesystem::remove(temp_manifest, ec);
  std::filesystem::remove(empty_manifest, ec);
  std::filesystem::remove(malformed_manifest, ec);
  std::filesystem::remove(bad_sha_manifest, ec);
  std::filesystem::remove(split_fields_manifest, ec);
  std::filesystem::remove(nested_fields_manifest, ec);
  std::filesystem::remove(nested_plus_top_level_manifest, ec);
}

void ScenarioConcurrentEmbedThreadSafety() {
  waxcpp::tests::Log("scenario: concurrent embed thread safety");
  waxcpp::MiniLMEmbedderTorch embedder(32);
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> workers{};
  workers.reserve(kThreads);
  for (int thread_id = 0; thread_id < kThreads; ++thread_id) {
    workers.emplace_back([&embedder, thread_id]() {
      for (int i = 0; i < kPerThread; ++i) {
        const std::string text = (i % 3 == 0)
                                     ? "shared-key"
                                     : ("t" + std::to_string(thread_id) + "-i" + std::to_string(i % 20));
        const auto vec = embedder.Embed(text);
        if (vec.size() != static_cast<std::size_t>(embedder.dimensions())) {
          throw std::runtime_error("concurrent embed produced invalid shape");
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  const auto a = embedder.Embed("shared-key");
  const auto b = embedder.Embed("shared-key");
  Require(a == b, "concurrent memoization must remain deterministic for same key");
  Require(embedder.cache_size() <= 32, "concurrent memoization must respect capacity bound");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("embeddings_test: start");
    ScenarioIdentityAndShape();
    ScenarioDeterministicEmbedding();
    ScenarioNormalizationAndEmptyInput();
    ScenarioBatchParity();
    ScenarioMemoizationCapacity();
    ScenarioAsciiTokenizationDeterminism();
    ScenarioRuntimeInfoAndManifestPolicy();
    ScenarioConcurrentEmbedThreadSafety();
    waxcpp::tests::Log("embeddings_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
