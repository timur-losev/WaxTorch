#include "waxcpp/embeddings.hpp"

#include "../test_logger.hpp"

#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>
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

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("embeddings_test: start");
    ScenarioIdentityAndShape();
    ScenarioDeterministicEmbedding();
    ScenarioNormalizationAndEmptyInput();
    ScenarioBatchParity();
    ScenarioMemoizationCapacity();
    waxcpp::tests::Log("embeddings_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
