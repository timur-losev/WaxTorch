#include "../../server/llama_cpp_embedding_provider.hpp"

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

bool ApproxEqual(float lhs, float rhs, float eps = 1e-5F) {
  return std::fabs(lhs - rhs) <= eps;
}

void ScenarioParseSupportsMultipleSchemas() {
  waxcpp::tests::Log("scenario: parse supports multiple schemas");
  {
    const auto parsed = waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"embedding":[1.0,2.0,3.0]})",
        3);
    Require(parsed.size() == 3, "direct embedding schema parse size mismatch");
  }
  {
    const auto parsed = waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"embeddings":[[0.1,0.2,0.3],[9,9,9]]})",
        3);
    Require(parsed.size() == 3 && ApproxEqual(parsed[0], 0.1F), "embeddings schema parse mismatch");
  }
  {
    const auto parsed = waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"data":[{"embedding":[7.0,8.0,9.0]}]})",
        3);
    Require(parsed.size() == 3 && ApproxEqual(parsed[2], 9.0F), "openai-style data schema parse mismatch");
  }
}

void ScenarioParseRejectsMalformedOrMismatched() {
  waxcpp::tests::Log("scenario: parse rejects malformed or mismatched payloads");
  bool threw = false;
  try {
    (void)waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"embedding":[1.0,2.0]})",
        3);
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "dimension mismatch must throw");

  threw = false;
  try {
    (void)waxcpp::server::LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
        R"({"foo":"bar"})",
        3);
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "missing embedding field must throw");
}

void ScenarioRequestFnEmbedAndMemoization() {
  waxcpp::tests::Log("scenario: request_fn embed and memoization");
  int request_count = 0;
  waxcpp::server::LlamaCppEmbeddingProvider provider(
      waxcpp::server::LlamaCppEmbeddingProviderConfig{
          .endpoint = "",
          .model_path = "g:/Proj/Agents1/Models/Qwen/embedding.gguf",
          .dimensions = 2,
          .normalize = true,
          .timeout_ms = 1000,
          .memoization_capacity = 4,
          .request_fn =
              [&](const std::string& body) -> std::string {
            ++request_count;
            if (body.find("\"content\":\"alpha\"") != std::string::npos) {
              return R"({"embedding":[3.0,4.0]})";
            }
            return R"({"embedding":[0.0,5.0]})";
          },
      });

  const auto first = provider.Embed("alpha");
  const auto second = provider.Embed("alpha");
  const auto third = provider.Embed("beta");
  Require(first.size() == 2, "embed dimensions mismatch");
  Require(ApproxEqual(first[0], 0.6F) && ApproxEqual(first[1], 0.8F), "normalized embedding mismatch");
  Require(first == second, "memoized embedding mismatch");
  Require(ApproxEqual(third[0], 0.0F) && ApproxEqual(third[1], 1.0F), "second normalized embedding mismatch");
  Require(request_count == 2, "request_fn should be called once per unique key");

  const auto batch = provider.EmbedBatch({"alpha", "beta", "alpha"});
  Require(batch.size() == 3, "batch size mismatch");
  Require(batch[0] == first && batch[1] == third && batch[2] == first, "batch embeddings mismatch");

  const auto identity = provider.identity();
  Require(identity.has_value(), "identity should be present");
  Require(identity->provider.has_value() && *identity->provider == "llama.cpp", "identity provider mismatch");
  Require(identity->dimensions.has_value() && *identity->dimensions == 2, "identity dimensions mismatch");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("llama_cpp_embedding_provider_test: start");
    ScenarioParseSupportsMultipleSchemas();
    ScenarioParseRejectsMalformedOrMismatched();
    ScenarioRequestFnEmbedAndMemoization();
    waxcpp::tests::Log("llama_cpp_embedding_provider_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
