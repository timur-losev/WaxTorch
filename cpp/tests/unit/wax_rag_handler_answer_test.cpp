#include "../../server/wax_rag_handler.hpp"

#include "../temp_artifacts.hpp"
#include "../test_logger.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <optional>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string TempName(const std::string& prefix, const std::string& suffix) {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::ostringstream out;
  out << prefix << static_cast<long long>(now) << suffix;
  return out.str();
}

void SetEnvVar(const char* key, const std::string& value) {
#if defined(_MSC_VER)
  if (_putenv_s(key, value.c_str()) != 0) {
    throw std::runtime_error(std::string("failed to set env var: ") + key);
  }
#else
  if (setenv(key, value.c_str(), 1) != 0) {
    throw std::runtime_error(std::string("failed to set env var: ") + key);
  }
#endif
}

std::optional<std::string> GetEnvVar(const char* key) {
#if defined(_MSC_VER)
  char* value = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&value, &len, key) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string out(value);
  std::free(value);
  if (out.empty()) {
    return std::nullopt;
  }
  return out;
#else
  const char* value = std::getenv(key);
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  return std::string(value);
#endif
}

void UnsetEnvVar(const char* key) {
#if defined(_MSC_VER)
  if (_putenv_s(key, "") != 0) {
    throw std::runtime_error(std::string("failed to unset env var: ") + key);
  }
#else
  if (unsetenv(key) != 0) {
    throw std::runtime_error(std::string("failed to unset env var: ") + key);
  }
#endif
}

class EnvVarGuard {
 public:
  explicit EnvVarGuard(const char* key) : key_(key), previous_(GetEnvVar(key)) {}
  ~EnvVarGuard() noexcept {
    try {
      if (previous_.has_value()) {
        SetEnvVar(key_.c_str(), *previous_);
      } else {
        UnsetEnvVar(key_.c_str());
      }
    } catch (...) {
    }
  }

 private:
  std::string key_{};
  std::optional<std::string> previous_{};
};

Poco::JSON::Object::Ptr ParseObject(const std::string& json) {
  Poco::JSON::Parser parser;
  const Poco::Dynamic::Var parsed = parser.parse(json);
  auto object = parsed.extract<Poco::JSON::Object::Ptr>();
  if (object.isNull()) {
    throw std::runtime_error("expected JSON object");
  }
  return object;
}

void RememberWithMetadata(waxcpp::server::WaxRAGHandler& handler,
                          const std::string& content,
                          const std::string& path,
                          int line_start,
                          int line_end,
                          const std::string& symbol) {
  Poco::JSON::Object::Ptr metadata = new Poco::JSON::Object();
  metadata->set("relative_path", path);
  metadata->set("line_start", std::to_string(line_start));
  metadata->set("line_end", std::to_string(line_end));
  metadata->set("symbol", symbol);

  Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
  params->set("content", content);
  params->set("metadata", metadata);
  const auto reply = handler.handle_remember(params);
  Require(reply == "OK", "remember call failed: " + reply);
}

void ScenarioAnswerGenerateUsesContextBudgetAndCitations() {
  waxcpp::tests::Log("scenario: answer.generate applies context budget and returns citations");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_store_", ".mv2s");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create temp runtime root");
  }
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());

  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = temp_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;

  std::string captured_generation_request{};
  auto generation_client = std::make_unique<waxcpp::server::LlamaCppGenerationClient>(
      waxcpp::server::LlamaCppGenerationConfig{
          .endpoint = "",
          .model_path = models.generation_model.model_path,
          .timeout_ms = 1000,
          .max_retries = 0,
          .retry_backoff_ms = 0,
          .request_fn =
              [&](const std::string& body) {
            captured_generation_request = body;
            return std::string(R"({"content":"stubbed-answer"})");
          },
      });

  waxcpp::server::WaxRAGHandler handler(store_path, models, std::move(generation_client));

  RememberWithMetadata(handler,
                       "AlphaFn shared tokenA tokenB tokenC tokenD tokenE tokenF",
                       "Engine/Source/Alpha.cpp",
                       10,
                       20,
                       "AlphaFn");
  RememberWithMetadata(handler,
                       "BetaFn shared token1 token2 token3 token4 token5 token6",
                       "Engine/Source/Beta.cpp",
                       30,
                       40,
                       "BetaFn");
  Require(handler.handle_flush(Poco::JSON::Object::Ptr{}) == "OK", "flush must succeed");

  Poco::JSON::Object::Ptr params = new Poco::JSON::Object();
  params->set("query", "shared");
  params->set("max_context_items", 10);
  params->set("max_context_tokens", 4);
  params->set("max_output_tokens", 128);
  const auto response_raw = handler.handle_answer_generate(params);
  Require(response_raw.rfind("Error:", 0) != 0, "answer.generate returned error: " + response_raw);
  const auto response = ParseObject(response_raw);

  Require(response->optValue<std::string>("answer", "") == "stubbed-answer", "answer payload mismatch");
  const int context_items_used = response->optValue<int>("context_items_used", -1);
  Require(context_items_used == 1,
          "budget clamp must keep exactly one context item, got " + std::to_string(context_items_used) +
              ", response=" + response_raw);
  Require(response->optValue<int>("context_tokens_used", 0) > 0, "context token count must be positive");
  Require(response->optValue<std::string>("model", "") == models.generation_model.model_path,
          "response model path mismatch");

  auto citations = response->getArray("citations");
  Require(!citations.isNull(), "citations array must be present");
  Require(citations->size() >= 1, "at least one citation expected");
  auto first_citation = citations->getObject(0);
  Require(!first_citation.isNull(), "citation entry must be an object");
  Require(!first_citation->optValue<std::string>("relative_path", "").empty(),
          "citation relative_path must be populated");

  Require(captured_generation_request.find("Citation Map") != std::string::npos,
          "generation request must include citation map text");
  Require(captured_generation_request.find("[frame:") != std::string::npos,
          "generation request must include frame tags");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
}

void ScenarioRejectsInvalidOrchestratorIngestEnvValues() {
  waxcpp::tests::Log("scenario: constructor rejects invalid orchestrator ingest env values");
  const auto temp_root = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_repo_", "");
  const auto store_path = std::filesystem::temp_directory_path() / TempName("waxcpp_answer_store_", ".mv2s");

  std::error_code ec;
  std::filesystem::create_directories(temp_root, ec);
  if (ec) {
    throw std::runtime_error("failed to create temp runtime root");
  }

  EnvVarGuard guard_cpp_root("WAXCPP_LLAMA_CPP_ROOT");
  EnvVarGuard guard_ingest_concurrency("WAXCPP_ORCH_INGEST_CONCURRENCY");
  EnvVarGuard guard_ingest_batch_size("WAXCPP_ORCH_INGEST_BATCH_SIZE");
  SetEnvVar("WAXCPP_LLAMA_CPP_ROOT", temp_root.string());
  SetEnvVar("WAXCPP_ORCH_INGEST_CONCURRENCY", "0");
  SetEnvVar("WAXCPP_ORCH_INGEST_BATCH_SIZE", "not-a-number");

  waxcpp::RuntimeModelsConfig models{};
  models.generation_model.runtime = "llama_cpp";
  models.generation_model.model_path = "test-generation.gguf";
  models.embedding_model.runtime = "disabled";
  models.embedding_model.model_path.clear();
  models.llama_cpp_root = temp_root.string();
  models.enable_vector_search = false;
  models.require_distinct_models = true;

  bool threw = false;
  try {
    waxcpp::server::WaxRAGHandler handler(store_path, models);
    (void)handler;
  } catch (const std::exception& ex) {
    threw = true;
    const std::string message = ex.what();
    Require(message.find("WAXCPP_ORCH_INGEST_CONCURRENCY") != std::string::npos,
            "invalid ingest env error must mention variable name");
  }
  Require(threw, "constructor must reject invalid ingest env values");

  std::filesystem::remove_all(temp_root, ec);
  ec.clear();
  waxcpp::tests::CleanupStoreArtifacts(store_path);
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("wax_rag_handler_answer_test: start");
    ScenarioAnswerGenerateUsesContextBudgetAndCitations();
    ScenarioRejectsInvalidOrchestratorIngestEnvValues();
    waxcpp::tests::Log("wax_rag_handler_answer_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_answer_repo_");
    waxcpp::tests::CleanupTempArtifactsByPrefix("waxcpp_answer_store_");
    return EXIT_FAILURE;
  }
}
