#include "waxcpp/embeddings.hpp"

#include "../test_logger.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

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
  std::string normalized = *raw;
  for (auto& ch : normalized) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("libtorch_manifest_gate_test: start");
    if (!EnvIsTruthy("WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256")) {
      waxcpp::tests::Log("libtorch_manifest_gate_test: skipped (WAXCPP_REQUIRE_LIBTORCH_ARTIFACT_SHA256 is not enabled)");
      return EXIT_SUCCESS;
    }

    waxcpp::MiniLMEmbedderTorch embedder;
    const auto info = embedder.runtime_info();

    Require(info.libtorch_manifest_detected, "expected detected libtorch manifest");
    Require(info.libtorch_manifest_valid, "expected valid libtorch manifest");
    Require(info.libtorch_selected_artifact_path.has_value(),
            "expected selected artifact path from manifest");
    Require(info.libtorch_selected_artifact_sha256.has_value(),
            "expected selected artifact sha256 from manifest");
    Require(info.libtorch_selected_artifact_class.has_value(),
            "expected selected artifact class from manifest");
    Require(info.libtorch_selected_artifact_resolved_path.has_value(),
            "expected resolved selected artifact file path");
    Require(info.libtorch_selected_artifact_sha256_verified,
            "expected selected artifact sha256 verification to be true");

    const std::filesystem::path resolved(*info.libtorch_selected_artifact_resolved_path);
    Require(std::filesystem::exists(resolved), "resolved selected artifact file does not exist");
    Require(std::filesystem::is_regular_file(resolved), "resolved selected artifact path is not a regular file");

    waxcpp::tests::LogKV("selected_backend", info.selected_backend);
    waxcpp::tests::LogKV("selected_artifact_path", *info.libtorch_selected_artifact_path);
    waxcpp::tests::LogKV("selected_artifact_resolved_path", *info.libtorch_selected_artifact_resolved_path);
    waxcpp::tests::LogKV("selected_artifact_class", *info.libtorch_selected_artifact_class);
    waxcpp::tests::Log("libtorch_manifest_gate_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
