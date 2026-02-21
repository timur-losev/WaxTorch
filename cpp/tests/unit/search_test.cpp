#include "waxcpp/search.hpp"

#include "../test_logger.hpp"

#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void ScenarioDeterministicOrderingAndTopK() {
  waxcpp::tests::Log("scenario: deterministic ordering and top_k");
  waxcpp::SearchRequest request{};
  request.query = "alpha";
  request.top_k = 2;
  request.preview_max_bytes = 100;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 5, .score = 1.0F, .preview_text = std::string("five"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("two"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 1, .score = 3.0F, .preview_text = std::string("one"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "top_k should clamp context item count");
  Require(context.items[0].frame_id == 1, "highest score should rank first");
  Require(context.items[1].frame_id == 2, "tie should break by lower frame_id");
}

void ScenarioPreviewClampAndTokenCount() {
  waxcpp::tests::Log("scenario: preview clamp and token count");
  waxcpp::SearchRequest request{};
  request.query = "query";
  request.top_k = 10;
  request.preview_max_bytes = 5;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 1.0F, .preview_text = std::string("alpha beta"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 0.5F, .preview_text = std::string(""), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 3, .score = 0.25F, .preview_text = std::nullopt, .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 3, "empty or missing previews should produce surrogate entries");
  Require(context.items[0].text == "alpha", "preview_max_bytes should truncate snippet");
  Require(context.items[1].kind == waxcpp::RAGItemKind::kSurrogate, "empty preview should map to surrogate");
  Require(context.items[2].kind == waxcpp::RAGItemKind::kSurrogate, "missing preview should map to surrogate");
  Require(context.total_tokens == 5, "token counting mismatch");
}

void ScenarioNaNScoreNormalization() {
  waxcpp::tests::Log("scenario: nan score normalization");
  waxcpp::SearchRequest request{};
  request.query = "nan";
  request.top_k = 10;
  request.preview_max_bytes = 100;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = std::numeric_limits<float>::quiet_NaN(), .preview_text = std::string("nan"), .sources = {}},
      {.frame_id = 2, .score = 0.1F, .preview_text = std::string("good"), .sources = {}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "expected two context items");
  Require(context.items[0].frame_id == 2, "numeric score should outrank NaN");
  Require(context.items[1].score == 0.0F, "NaN score should be normalized to zero");
}

void ScenarioUnifiedSearchModesAndHybridRrf() {
  waxcpp::tests::Log("scenario: unified search modes and hybrid rrf");
  const std::vector<waxcpp::SearchResult> text_results = {
      {.frame_id = 10, .score = 4.0F, .preview_text = std::string("t10"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 20, .score = 2.0F, .preview_text = std::string("t20"), .sources = {waxcpp::SearchSource::kText}},
  };
  const std::vector<waxcpp::SearchResult> vector_results = {
      {.frame_id = 20, .score = 3.0F, .preview_text = std::string("v20"), .sources = {waxcpp::SearchSource::kVector}},
      {.frame_id = 30, .score = 1.0F, .preview_text = std::string("v30"), .sources = {waxcpp::SearchSource::kVector}},
  };

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kTextOnly, 0.5F};
    request.top_k = 10;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(response.results.size() == 2, "text-only mode should use text channel only");
    Require(response.results[0].frame_id == 10, "text-only top result mismatch");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kVectorOnly, 0.5F};
    request.top_k = 10;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(response.results.size() == 2, "vector-only mode should use vector channel only");
    Require(response.results[0].frame_id == 20, "vector-only top result mismatch");
  }

  {
    waxcpp::SearchRequest request{};
    request.mode = {waxcpp::SearchModeKind::kHybrid, 0.5F};
    request.top_k = 10;
    request.rrf_k = 60;
    const auto response = waxcpp::UnifiedSearchWithCandidates(request, text_results, vector_results);
    Require(response.results.size() == 3, "hybrid mode should merge both channels");
    Require(response.results[0].frame_id == 20, "frame present in both channels should win hybrid RRF");
    Require(response.results[0].sources.size() == 2, "merged frame should carry both sources");
  }
}

void ScenarioContextTokenBudgetClamp() {
  waxcpp::tests::Log("scenario: context token budget clamp");
  waxcpp::SearchRequest request{};
  request.query = "budget";
  request.top_k = 10;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 2;
  request.snippet_max_tokens = 2;
  request.max_context_tokens = 3;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 2.0F, .preview_text = std::string("one two three"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("four five six"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "budget clamp should keep partial second item");
  Require(context.items[0].text == "one two", "snippet per-item token clamp mismatch");
  Require(context.items[1].text == "four", "remaining-budget truncation mismatch");
  Require(context.total_tokens == 3, "context token budget mismatch");
}

void ScenarioRagItemKindPolicy() {
  waxcpp::tests::Log("scenario: rag item kind policy");
  waxcpp::SearchRequest request{};
  request.query = "kinds";
  request.top_k = 10;
  request.preview_max_bytes = 256;
  request.expansion_max_tokens = 4;
  request.snippet_max_tokens = 2;
  request.max_context_tokens = 20;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 1, .score = 2.0F, .preview_text = std::string("a b c d e"), .sources = {waxcpp::SearchSource::kText}},
      {.frame_id = 2, .score = 1.0F, .preview_text = std::string("x y z"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 2, "expected two items");
  Require(context.items[0].kind == waxcpp::RAGItemKind::kExpanded, "first item should be expanded");
  Require(context.items[0].text == "a b c d", "expanded token clamp mismatch");
  Require(context.items[1].kind == waxcpp::RAGItemKind::kSnippet, "second item should be snippet");
  Require(context.items[1].text == "x y", "snippet token clamp mismatch");
}

void ScenarioSurrogateFallback() {
  waxcpp::tests::Log("scenario: surrogate fallback");
  waxcpp::SearchRequest request{};
  request.query = "surrogate";
  request.top_k = 10;
  request.preview_max_bytes = 0;  // force empty preview path
  request.expansion_max_tokens = 5;
  request.snippet_max_tokens = 2;
  request.max_context_tokens = 10;

  waxcpp::SearchResponse response{};
  response.results = {
      {.frame_id = 9, .score = 1.0F, .preview_text = std::string("unavailable"), .sources = {waxcpp::SearchSource::kText}},
  };

  const auto context = waxcpp::BuildFastRAGContext(request, response);
  Require(context.items.size() == 1, "surrogate fallback should emit one item");
  Require(context.items[0].kind == waxcpp::RAGItemKind::kSurrogate, "item kind should be surrogate");
  Require(context.items[0].text == "frame 9", "surrogate text mismatch");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("search_test: start");
    ScenarioDeterministicOrderingAndTopK();
    ScenarioPreviewClampAndTokenCount();
    ScenarioNaNScoreNormalization();
    ScenarioUnifiedSearchModesAndHybridRrf();
    ScenarioContextTokenBudgetClamp();
    ScenarioRagItemKindPolicy();
    ScenarioSurrogateFallback();
    waxcpp::tests::Log("search_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
