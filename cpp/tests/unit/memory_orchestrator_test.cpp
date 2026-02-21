#include "waxcpp/memory_orchestrator.hpp"
#include "waxcpp/wax_store.hpp"
#include "../../src/core/wax_store_test_hooks.hpp"

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

std::string BytesToString(const std::vector<std::byte>& bytes) {
  std::string text{};
  text.reserve(bytes.size());
  for (const auto b : bytes) {
    text.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
  }
  return text;
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

class CountingBatchEmbedder final : public waxcpp::BatchEmbeddingProvider {
 public:
  int dimensions() const override { return 4; }
  bool normalize() const override { return true; }
  std::optional<waxcpp::EmbeddingIdentity> identity() const override { return std::nullopt; }

  std::vector<float> Embed(const std::string& text) override {
    ++embed_calls_;
    return BuildEmbedding(text);
  }

  std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) override {
    ++batch_calls_;
    std::vector<std::vector<float>> out{};
    out.reserve(texts.size());
    for (const auto& text : texts) {
      out.push_back(BuildEmbedding(text));
    }
    return out;
  }

  void Reset() {
    embed_calls_ = 0;
    batch_calls_ = 0;
  }

  int embed_calls() const { return embed_calls_; }
  int batch_calls() const { return batch_calls_; }

 private:
  static std::vector<float> BuildEmbedding(const std::string& text) {
    std::vector<float> out(4, 0.0F);
    for (std::size_t i = 0; i < text.size(); ++i) {
      out[i % out.size()] += static_cast<float>(static_cast<unsigned char>(text[i])) / 255.0F;
    }
    return out;
  }

  int embed_calls_ = 0;
  int batch_calls_ = 0;
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

void ScenarioSearchModePolicyValidation(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: search mode policy validation");

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = false;
    config.enable_vector_search = false;
    config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
    bool threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
      orchestrator.Close();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "text-only mode must require enabled text channel");
  }

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = true;
    config.enable_vector_search = false;
    config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
    bool threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
      orchestrator.Close();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "vector-only mode must require enabled vector channel");
  }

  {
    waxcpp::OrchestratorConfig config{};
    config.enable_text_search = false;
    config.enable_vector_search = false;
    config.rag.search_mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};
    bool threw = false;
    try {
      waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
      orchestrator.Close();
    } catch (const std::exception&) {
      threw = true;
    }
    Require(threw, "hybrid mode must require at least one enabled channel");
  }
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

void ScenarioBatchProviderUsedForVectorRecall(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall uses committed vector index");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
  config.embedding_cache_capacity = 0;  // force no memoized doc embeddings.

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("batch doc one", {});
    orchestrator.Remember("batch doc two", {});
    orchestrator.Flush();

    embedder->Reset();
    (void)orchestrator.Recall("doc", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(embedder->batch_calls() == 0, "vector recall should read committed vector index without EmbedBatch");
    Require(embedder->embed_calls() == 0, "vector recall with explicit query embedding should avoid Embed");
    orchestrator.Close();
  }
}

void ScenarioMaxSnippetsClamp(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: max_snippets clamp");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_top_k = 10;
  config.rag.max_snippets = 1;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple alpha", {});
    orchestrator.Remember("apple beta", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(context.items.size() == 1, "max_snippets should clamp recall item count");
    orchestrator.Close();
  }
}

void ScenarioRememberChunking(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember chunking");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;
  config.chunking.target_tokens = 3;
  config.chunking.overlap_tokens = 1;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("a b c d e", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    auto store = waxcpp::WaxStore::Open(path);
    const auto stats = store.Stats();
    Require(stats.frame_count == 2, "chunking should split content into two frames");
    Require(BytesToString(store.FrameContent(0)) == "a b c", "chunk[0] mismatch");
    Require(BytesToString(store.FrameContent(1)) == "c d e", "chunk[1] mismatch");
    store.Close();
  }
}

void ScenarioBatchProviderUsedForRemember(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: batch provider used for remember");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.embedding_cache_capacity = 64;
  config.chunking.target_tokens = 2;
  config.chunking.overlap_tokens = 0;

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("a b c d e", {});  // 3 chunks with target=2
    Require(embedder->batch_calls() == 1, "remember should use EmbedBatch once for multi-chunk ingest");
    Require(embedder->embed_calls() == 0, "remember should avoid per-chunk Embed when batch provider is available");
    orchestrator.Flush();
    orchestrator.Close();
  }
}

void ScenarioRememberRespectsIngestBatchSize(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: remember respects ingest_batch_size");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.embedding_cache_capacity = 64;
  config.ingest_batch_size = 2;
  config.chunking.target_tokens = 1;
  config.chunking.overlap_tokens = 0;

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("a b c d e", {});  // 5 chunks, batch_size=2 -> 3 batch calls
    Require(embedder->batch_calls() == 3, "remember should split EmbedBatch calls by ingest_batch_size");
    Require(embedder->embed_calls() == 0, "remember batch mode should avoid per-chunk Embed");
    orchestrator.Flush();
    orchestrator.Close();
  }
}

void ScenarioTextOnlyRecallSkipsVectorEmbedding(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: text-only recall skips vector embedding");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  config.embedding_cache_capacity = 0;  // ensure recall would need embedder if mode gating were broken.

  auto embedder = std::make_shared<CountingEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("apple story", {});
    orchestrator.Remember("banana story", {});
    orchestrator.Flush();

    embedder->ResetCalls();
    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "text-only recall should still return text results");
    Require(embedder->calls() == 0, "text-only recall must not call embedder");
    orchestrator.Close();
  }
}

void ScenarioStructuredMemoryFacts(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured memory facts");
  waxcpp::OrchestratorConfig config{};
  config.enable_vector_search = false;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:1", "name", "Alice", {{"src", "profile"}});
    orchestrator.RememberFact("user:1", "city", "Paris");
    orchestrator.RememberFact("user:2", "name", "Bob");
    orchestrator.RememberFact("user:1", "name", "Alice B", {{"src", "edit"}});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto user_facts = reopened.RecallFactsByEntityPrefix("user:", 10);
    Require(user_facts.size() == 3, "structured facts prefix query mismatch");
    Require(user_facts[0].entity == "user:1" && user_facts[0].attribute == "city", "fact order mismatch [0]");
    Require(user_facts[1].entity == "user:1" && user_facts[1].attribute == "name", "fact order mismatch [1]");
    Require(user_facts[1].value == "Alice B", "upserted fact value mismatch");
    Require(user_facts[1].version == 2, "fact version should increment on upsert");
    Require(user_facts[1].metadata.at("src") == "edit", "fact metadata mismatch");
    Require(user_facts[2].entity == "user:2", "fact order mismatch [2]");
    reopened.Close();
  }
}

void ScenarioRecallIncludesStructuredMemory(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall includes structured memory");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
  config.rag.search_top_k = 10;

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:42", "city", "tokyo");
    orchestrator.RememberFact("user:42", "favorite", "sushi");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto context = reopened.Recall("tokyo");
    Require(!context.items.empty(), "recall should include structured memory hit");
    bool found_structured = false;
    bool found_text = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          found_structured = true;
          break;
        }
        if (source == waxcpp::SearchSource::kText) {
          found_text = true;
        }
      }
    }
    Require(found_structured, "structured memory source must appear in recall context");
    Require(!found_text, "internal structured records must not surface as text-source hits");
    reopened.Close();
  }
}

void ScenarioRecallTextChannelUsesTextSource(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall text channel uses text source");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("apple orchard", {});
    orchestrator.Flush();

    const auto context = orchestrator.Recall("apple");
    Require(!context.items.empty(), "text recall should return at least one item");
    bool has_text_source = false;
    for (const auto source : context.items.front().sources) {
      if (source == waxcpp::SearchSource::kText) {
        has_text_source = true;
        break;
      }
    }
    Require(has_text_source, "store text recall result should keep kText source");
    orchestrator.Close();
  }
}

void ScenarioRecallVisibilityRequiresFlush(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: recall visibility requires flush");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush gated text apple", {});
    orchestrator.RememberFact("user:flush", "fruit", "apple");

    const auto before_flush = orchestrator.Recall("apple");
    Require(before_flush.items.empty(), "staged text mutations should stay invisible before flush");

    orchestrator.Flush();
    const auto after_flush = orchestrator.Recall("apple");
    Require(!after_flush.items.empty(), "committed text mutations should be visible after flush");
    orchestrator.Close();
  }
}

void ScenarioVectorRecallVisibilityRequiresFlush(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall visibility requires flush");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("vector gated apple", {});

    const auto before_flush = orchestrator.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(before_flush.items.empty(), "staged vector mutation should stay invisible before flush");

    orchestrator.Flush();
    const auto after_flush = orchestrator.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_flush.items.empty(), "committed vector mutation should be visible after flush");
    orchestrator.Close();
  }
}

void ScenarioVectorIndexRebuildOnReopen(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector index rebuild on reopen");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("reopen vector apple", {});
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    embedder->Reset();
    const auto context = reopened.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopen should restore committed vector index");
    Require(embedder->batch_calls() == 0, "explicit vector recall should not re-embed docs after reopen");
    Require(embedder->embed_calls() == 0, "explicit vector recall should avoid query embed calls");
    reopened.Close();
  }
}

void ScenarioVectorCloseWithoutFlushPersistsViaStoreClose(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector close without flush persists via store close");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("close persist vector apple", {});
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    embedder->Reset();
    const auto context = reopened.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "Close() should persist local mutations and reopen should rebuild vector index");
    Require(embedder->batch_calls() == 0, "explicit vector recall should not batch-embed docs");
    Require(embedder->embed_calls() == 0, "explicit vector recall should avoid query embed calls");
    reopened.Close();
  }
}

void ScenarioVectorRecallSupportsExplicitEmbeddingWithoutQuery(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: vector recall supports explicit embedding without query");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("embedding only recall doc", {});
    orchestrator.Flush();

    embedder->Reset();
    const auto context = orchestrator.Recall("", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "explicit embedding recall should work with empty query");
    Require(embedder->embed_calls() == 0, "explicit embedding recall should not call Embed");
    Require(embedder->batch_calls() == 0, "explicit embedding recall should not call EmbedBatch");
    orchestrator.Close();
  }
}

void ScenarioFlushFailureDoesNotExposeStagedText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure does not expose staged text");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("failing flush apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when store commit failpoint is set");

    const auto before_successful_flush = orchestrator.Recall("apple");
    Require(before_successful_flush.items.empty(),
            "failed flush must not expose staged text index mutations");

    orchestrator.Flush();
    const auto after_successful_flush = orchestrator.Recall("apple");
    Require(!after_successful_flush.items.empty(),
            "successful retry flush should expose committed text mutation");
    orchestrator.Close();
  }
}

void ScenarioFlushFailureDoesNotExposeStagedVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure does not expose staged vector");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("failing flush vector apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when store commit failpoint is set");

    const auto before_successful_flush = orchestrator.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(before_successful_flush.items.empty(),
            "failed flush must not expose staged vector index mutations");

    orchestrator.Flush();
    const auto after_successful_flush = orchestrator.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(!after_successful_flush.items.empty(),
            "successful retry flush should expose committed vector mutation");
    orchestrator.Close();
  }
}

void ScenarioFlushFailureThenCloseReopenRecoversText(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure then close/reopen recovers text");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.Remember("flush close reopen apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when failpoint is enabled");

    const auto before_close = orchestrator.Recall("apple");
    Require(before_close.items.empty(), "failed flush should keep staged text hidden in current process");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto context = reopened.Recall("apple");
    Require(!context.items.empty(), "reopen should rebuild text index from committed store state");
    reopened.Close();
  }
}

void ScenarioFlushFailureThenCloseReopenRecoversVector(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure then close/reopen recovers vector");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = false;
  config.enable_vector_search = true;
  config.rag.search_mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};

  auto embedder = std::make_shared<CountingBatchEmbedder>();
  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, embedder);
    orchestrator.Remember("flush close reopen vector apple", {});

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when failpoint is enabled");

    const auto before_close = orchestrator.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(before_close.items.empty(), "failed flush should keep staged vector hidden in current process");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, embedder);
    embedder->Reset();
    const auto context = reopened.Recall("apple", {1.0F, 0.0F, 0.0F, 0.0F});
    Require(!context.items.empty(), "reopen should rebuild vector index from committed store state");
    Require(embedder->batch_calls() == 0, "explicit vector recall should not re-embed docs after reopen");
    Require(embedder->embed_calls() == 0, "explicit vector recall should avoid query embed calls");
    reopened.Close();
  }
}

void ScenarioFlushFailureThenCloseReopenRecoversStructuredFact(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: flush failure then close/reopen recovers structured fact");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:reopen", "city", "rome");

    bool flush_threw = false;
    waxcpp::core::testing::SetCommitFailStep(1);
    try {
      orchestrator.Flush();
    } catch (const std::exception&) {
      flush_threw = true;
    }
    waxcpp::core::testing::ClearCommitFailStep();
    Require(flush_threw, "flush should throw when failpoint is enabled");

    const auto before_close = orchestrator.Recall("rome");
    Require(before_close.items.empty(), "failed flush should keep staged structured fact hidden");
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:reopen", 10);
    Require(facts.size() == 1, "structured fact should be restored after reopen");
    const auto context = reopened.Recall("rome");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(has_structured, "reopen should rebuild structured-text index from committed fact");
    reopened.Close();
  }
}

void ScenarioUseAfterCloseThrows(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: use-after-close throws");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
  orchestrator.Remember("close semantics text", {});
  orchestrator.Flush();
  orchestrator.Close();
  orchestrator.Close();  // idempotent

  bool recall_threw = false;
  try {
    (void)orchestrator.Recall("text");
  } catch (const std::exception&) {
    recall_threw = true;
  }
  Require(recall_threw, "Recall should throw after Close");

  bool remember_threw = false;
  try {
    orchestrator.Remember("again", {});
  } catch (const std::exception&) {
    remember_threw = true;
  }
  Require(remember_threw, "Remember should throw after Close");

  bool flush_threw = false;
  try {
    orchestrator.Flush();
  } catch (const std::exception&) {
    flush_threw = true;
  }
  Require(flush_threw, "Flush should throw after Close");

  bool fact_threw = false;
  try {
    orchestrator.RememberFact("user:closed", "city", "rome");
  } catch (const std::exception&) {
    fact_threw = true;
  }
  Require(fact_threw, "RememberFact should throw after Close");
}

void ScenarioStructuredMemoryRemovePersists(const std::filesystem::path& path) {
  waxcpp::tests::Log("scenario: structured memory remove persists");
  waxcpp::OrchestratorConfig config{};
  config.enable_text_search = true;
  config.enable_vector_search = false;
  config.rag.search_mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};

  {
    waxcpp::MemoryOrchestrator orchestrator(path, config, nullptr);
    orchestrator.RememberFact("user:9", "city", "Paris");
    orchestrator.RememberFact("user:9", "name", "Dora");
    const bool removed = orchestrator.ForgetFact("user:9", "city");
    const bool removed_missing = orchestrator.ForgetFact("user:9", "missing");
    Require(removed, "ForgetFact should return true when key exists");
    Require(!removed_missing, "ForgetFact should return false for missing key");
    orchestrator.Flush();
    orchestrator.Close();
  }

  {
    waxcpp::MemoryOrchestrator reopened(path, config, nullptr);
    const auto facts = reopened.RecallFactsByEntityPrefix("user:9", 10);
    Require(facts.size() == 1, "removed fact should stay removed after reopen");
    Require(facts[0].attribute == "name", "unexpected fact left after remove replay");

    const auto context = reopened.Recall("paris");
    bool has_structured = false;
    for (const auto& item : context.items) {
      for (const auto source : item.sources) {
        if (source == waxcpp::SearchSource::kStructuredMemory) {
          has_structured = true;
          break;
        }
      }
    }
    Require(!has_structured, "removed fact must not participate in recall");
    reopened.Close();
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
    const auto path5 = UniquePath();
    const auto path6 = UniquePath();
    const auto path7 = UniquePath();
    const auto path8 = UniquePath();
    const auto path9 = UniquePath();
    const auto path10 = UniquePath();
    const auto path11 = UniquePath();
    const auto path12 = UniquePath();
    const auto path13 = UniquePath();
    const auto path14 = UniquePath();
    const auto path15 = UniquePath();
    const auto path16 = UniquePath();
    const auto path17 = UniquePath();
    const auto path18 = UniquePath();
    const auto path19 = UniquePath();
    const auto path20 = UniquePath();
    const auto path21 = UniquePath();
    const auto path22 = UniquePath();
    const auto path23 = UniquePath();
    const auto path24 = UniquePath();
    const auto path25 = UniquePath();
    const auto path26 = UniquePath();

    ScenarioVectorPolicyValidation(path0);
    ScenarioSearchModePolicyValidation(path22);
    ScenarioRememberFlushPersistsFrame(path1);
    ScenarioRecallReturnsRankedItems(path2);
    ScenarioHybridRecallWithEmbedder(path3);
    ScenarioEmbeddingMemoizationInRecall(path4);
    ScenarioBatchProviderUsedForVectorRecall(path5);
    ScenarioMaxSnippetsClamp(path6);
    ScenarioRememberChunking(path7);
    ScenarioBatchProviderUsedForRemember(path8);
    ScenarioRememberRespectsIngestBatchSize(path9);
    ScenarioTextOnlyRecallSkipsVectorEmbedding(path10);
    ScenarioStructuredMemoryFacts(path11);
    ScenarioRecallIncludesStructuredMemory(path12);
    ScenarioRecallTextChannelUsesTextSource(path13);
    ScenarioStructuredMemoryRemovePersists(path14);
    ScenarioRecallVisibilityRequiresFlush(path15);
    ScenarioVectorRecallVisibilityRequiresFlush(path16);
    ScenarioVectorIndexRebuildOnReopen(path17);
    ScenarioVectorCloseWithoutFlushPersistsViaStoreClose(path18);
    ScenarioVectorRecallSupportsExplicitEmbeddingWithoutQuery(path19);
    ScenarioFlushFailureDoesNotExposeStagedText(path20);
    ScenarioFlushFailureDoesNotExposeStagedVector(path21);
    ScenarioFlushFailureThenCloseReopenRecoversText(path23);
    ScenarioFlushFailureThenCloseReopenRecoversVector(path24);
    ScenarioFlushFailureThenCloseReopenRecoversStructuredFact(path25);
    ScenarioUseAfterCloseThrows(path26);

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
    std::filesystem::remove(path5, ec);
    std::filesystem::remove(path5.string() + ".writer.lock", ec);
    std::filesystem::remove(path6, ec);
    std::filesystem::remove(path6.string() + ".writer.lock", ec);
    std::filesystem::remove(path7, ec);
    std::filesystem::remove(path7.string() + ".writer.lock", ec);
    std::filesystem::remove(path8, ec);
    std::filesystem::remove(path8.string() + ".writer.lock", ec);
    std::filesystem::remove(path9, ec);
    std::filesystem::remove(path9.string() + ".writer.lock", ec);
    std::filesystem::remove(path10, ec);
    std::filesystem::remove(path10.string() + ".writer.lock", ec);
    std::filesystem::remove(path11, ec);
    std::filesystem::remove(path11.string() + ".writer.lock", ec);
    std::filesystem::remove(path12, ec);
    std::filesystem::remove(path12.string() + ".writer.lock", ec);
    std::filesystem::remove(path13, ec);
    std::filesystem::remove(path13.string() + ".writer.lock", ec);
    std::filesystem::remove(path14, ec);
    std::filesystem::remove(path14.string() + ".writer.lock", ec);
    std::filesystem::remove(path15, ec);
    std::filesystem::remove(path15.string() + ".writer.lock", ec);
    std::filesystem::remove(path16, ec);
    std::filesystem::remove(path16.string() + ".writer.lock", ec);
    std::filesystem::remove(path17, ec);
    std::filesystem::remove(path17.string() + ".writer.lock", ec);
    std::filesystem::remove(path18, ec);
    std::filesystem::remove(path18.string() + ".writer.lock", ec);
    std::filesystem::remove(path19, ec);
    std::filesystem::remove(path19.string() + ".writer.lock", ec);
    std::filesystem::remove(path20, ec);
    std::filesystem::remove(path20.string() + ".writer.lock", ec);
    std::filesystem::remove(path21, ec);
    std::filesystem::remove(path21.string() + ".writer.lock", ec);
    std::filesystem::remove(path22, ec);
    std::filesystem::remove(path22.string() + ".writer.lock", ec);
    std::filesystem::remove(path23, ec);
    std::filesystem::remove(path23.string() + ".writer.lock", ec);
    std::filesystem::remove(path24, ec);
    std::filesystem::remove(path24.string() + ".writer.lock", ec);
    std::filesystem::remove(path25, ec);
    std::filesystem::remove(path25.string() + ".writer.lock", ec);
    std::filesystem::remove(path26, ec);
    std::filesystem::remove(path26.string() + ".writer.lock", ec);
    waxcpp::tests::Log("memory_orchestrator_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
