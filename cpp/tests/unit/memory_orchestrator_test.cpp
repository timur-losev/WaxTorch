#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/wax_store.hpp"

#include "../test_logger.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::filesystem::path UniquePath() {
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("waxcpp_orchestrator_test_" + std::to_string(static_cast<long long>(now)) + ".mv2s");
}

std::vector<std::byte> StringToBytes(const std::string& text) {
  std::vector<std::byte> bytes{};
  bytes.reserve(text.size());
  for (const char ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

class CountingEmbedder final : public waxcpp::EmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override { return std::nullopt; }

  std::vector<float> Embed(const std::string& text) override {
    ++calls_;
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  void ResetCalls() { calls_ = 0; }
  int calls() const { return calls_; }

 private:
  int calls_ = 0;
};

void ScenarioVectorPolicyValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector policy validation");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = true;

  bool threw = false;
  try {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Close();
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "vector-enabled config must require embedder");
}

void ScenarioRememberFlushPersistsFrame(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember/flush persists frame");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("hello orchestrator", {{"source", "unit-test"}});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 1, "orchestrator should persist one frame");
    const auto content = store.FrameContent(0);
    Require(content == StringToBytes("hello orchestrator"), "persisted frame content mismatch");
    store.Close();
  }
}

void ScenarioRecallReturnsRankedItems(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall returns ranked items");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  config.rag.search_top_k = 10;
  config.rag.preview_max_bytes = 256;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple apple banana", {});
    orchestrator.Remember("apple", {});
    orchestrator.Remember("banana", {});
    orchestrator.Flush();
    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "recall should return non-empty context for matching query");
    Require(context.items[0].frame_id == 0, "higher overlap document should rank first");
    Require(context.items[0].text == "apple apple banana", "unexpected top recalled text");
    orchestrator.Close();
  }
}

void ScenarioHybridRecallWithEmbedder(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: hybrid recall with embedder");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};
  config.rag.search_top_k = 5;

  auto embedder = std::make_shared<waxcpp::MiniLMEmbedderTorch>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector apple context", {});
    orchestrator.Remember("banana only", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "hybrid recall should return at least one result");
    Require(context.items[0].sources.size() >= 1, "hybrid result should include at least one source");
    orchestrator.Close();
  }
}

void ScenarioEmbeddingMemoizationInRecall(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: embedding memoization in recall");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.embedding_cache_capacity = 32;

  auto embedder = std::make_shared<CountingEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("cached doc one", {});
    orchestrator.Remember("cached doc two", {});
    orchestrator.Flush();

    embedder->ResetCalls();
    (void)orchestrator.Recall("doc");
    (void)orchestrator.Recall("doc");
    // Expect query embedding per recall only, docs should come from cache.
    Require(embedder->calls() == 2, "recall should reuse cached document embeddings");
    orchestrator.Close();
  }
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("memory_orchestrator_test: start");
    const auto path0 = UniquePath();
    const auto path1 = UniquePath();
    const auto path2 = UniquePath();
    const auto path3 = UniquePath();
    const auto path4 = UniquePath();

    ScenarioVectorPolicyValidation(path0);
    ScenarioRememberFlushPersistsFrame(path1);
    ScenarioRecallReturnsRankedItems(path2);
    ScenarioHybridRecallWithEmbedder(path3);
    ScenarioEmbeddingMemoizationInRecall(path4);

    std::error_code ec;
    std::filesystem::remove(path0, ec);
    std::filesystem::remove(path0.string() + ".writer.lock", ec);
    std::filesystem::remove(path1, ec);
    std::filesystem::remove(path1.string() + ".writer.lock", ec);
    std::filesystem::remove(path2, ec);
    std::filesystem::remove(path2.string() + ".writer.lock", ec);
    std::filesystem::remove(path3, ec);
    std::filesystem::remove(path3.string() + ".writer.lock", ec);
    std::filesystem::remove(path4, ec);
    std::filesystem::remove(path4.string() + ".writer.lock", ec);
    waxcpp::tests::Log("memory_orchestrator_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
