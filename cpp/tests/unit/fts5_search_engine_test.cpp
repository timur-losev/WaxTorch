#include "waxcpp/fts5_search_engine.hpp"

#include "../test_logger.hpp"

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

void ScenarioBasicRanking() {
  waxcpp::tests::Log("scenario: basic ranking");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(10, "apple banana");
  engine.Index(11, "apple apple");
  engine.Index(12, "banana");

  const auto results = engine.Search("apple banana", 10);
  Require(results.size() == 3, "expected 3 search results");
  Require(results[0].frame_id == 10, "frame 10 should rank first for mixed query");
  Require(results[1].frame_id == 11, "frame 11 should rank second in mixed query");
  Require(results[0].score >= results[1].score, "top score must be greater or equal");
  Require(results[0].preview_text.has_value(), "preview text must be present");
}

void ScenarioCaseInsensitiveTokenization() {
  waxcpp::tests::Log("scenario: case-insensitive tokenization");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(1, "Hello, WORLD!!!");

  const auto results = engine.Search("world", 5);
  Require(results.size() == 1, "expected one result for case-insensitive match");
  Require(results[0].frame_id == 1, "matched frame_id mismatch");
}

void ScenarioDeterministicTieBreak() {
  waxcpp::tests::Log("scenario: deterministic tie-break by frame_id");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(42, "equal score token");
  engine.Index(7, "equal score token");

  const auto results = engine.Search("token", 10);
  Require(results.size() == 2, "expected two results in tie-break scenario");
  Require(results[0].score == results[1].score, "scores should be equal in tie-break scenario");
  Require(results[0].frame_id == 7, "lower frame_id must win tie-break");
  Require(results[1].frame_id == 42, "higher frame_id must come second on tie");
}

void ScenarioRemoveAndTopK() {
  waxcpp::tests::Log("scenario: remove and top_k clamp");
  waxcpp::FTS5SearchEngine engine;
  engine.IndexBatch({1, 2, 3}, {"alpha beta", "alpha", "beta"});
  engine.Remove(2);

  const auto results = engine.Search("alpha beta", 1);
  Require(results.size() == 1, "top_k=1 must clamp output to single result");
  Require(results[0].frame_id != 2, "removed frame must not appear in results");
}

void ScenarioIndexBatchMismatchThrows() {
  waxcpp::tests::Log("scenario: batch size mismatch throws");
  waxcpp::FTS5SearchEngine engine;
  bool threw = false;
  try {
    engine.IndexBatch({1, 2}, {"only-one"});
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "IndexBatch must throw on size mismatch");
}

void ScenarioEmptyInputs() {
  waxcpp::tests::Log("scenario: empty query/top_k");
  waxcpp::FTS5SearchEngine engine;
  engine.Index(1, "content");

  Require(engine.Search("", 10).empty(), "empty query must return no results");
  Require(engine.Search("content", 0).empty(), "top_k=0 must return no results");
  Require(engine.Search("content", -1).empty(), "negative top_k must return no results");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("fts5_search_engine_test: start");
    ScenarioBasicRanking();
    ScenarioCaseInsensitiveTokenization();
    ScenarioDeterministicTieBreak();
    ScenarioRemoveAndTopK();
    ScenarioIndexBatchMismatchThrows();
    ScenarioEmptyInputs();
    waxcpp::tests::Log("fts5_search_engine_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
