#include "waxcpp/fts5_search_engine.hpp"

#include "../test_logger.hpp"

#include <cstdlib>
#include <utility>
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

void ScenarioStagedMutationsRequireCommit() {
  waxcpp::tests::Log("scenario: staged mutations require commit");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndex(1, "alpha staged");
  Require(engine.PendingMutationCount() == 1, "pending mutation count mismatch after stage index");
  Require(engine.Search("alpha", 10).empty(), "staged index should not be visible before commit");

  engine.CommitStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after commit");
  Require(engine.Search("alpha", 10).size() == 1, "committed staged index should be searchable");

  engine.StageRemove(1);
  Require(engine.Search("alpha", 10).size() == 1, "staged remove should not be visible before commit");
  engine.CommitStaged();
  Require(engine.Search("alpha", 10).empty(), "committed staged remove should clear document");
}

void ScenarioRollbackStagedMutations() {
  waxcpp::tests::Log("scenario: rollback staged mutations");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndexBatch({10, 11}, {"alpha", "beta"});
  Require(engine.PendingMutationCount() == 2, "pending mutation count mismatch after stage batch");
  engine.RollbackStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after rollback");
  Require(engine.Search("alpha", 10).empty(), "rolled-back staged index should not be visible");
}

void ScenarioStagedOrderDeterminism() {
  waxcpp::tests::Log("scenario: staged order determinism");
  waxcpp::FTS5SearchEngine engine;
  engine.StageIndex(7, "old");
  engine.StageIndex(7, "new");
  engine.StageRemove(7);
  engine.StageIndex(7, "final");
  Require(engine.PendingMutationCount() == 4, "pending mutation count mismatch in order scenario");
  engine.CommitStaged();

  const auto results = engine.Search("final", 10);
  Require(results.size() == 1, "final staged state should leave one searchable document");
  Require(results[0].frame_id == 7, "unexpected frame_id after staged mutation order apply");
}

void ScenarioMoveSemanticsPreserveIndexState() {
  waxcpp::tests::Log("scenario: move semantics preserve index state");
  waxcpp::FTS5SearchEngine source;
  source.Index(101, "move alpha");
  source.Index(102, "move beta");

  waxcpp::FTS5SearchEngine moved = std::move(source);
  const auto moved_results = moved.Search("alpha", 10);
  Require(moved_results.size() == 1, "moved engine should preserve indexed documents");
  Require(moved_results[0].frame_id == 101, "moved engine returned unexpected frame_id");

  waxcpp::FTS5SearchEngine reassigned;
  reassigned = std::move(moved);
  const auto reassigned_results = reassigned.Search("beta", 10);
  Require(reassigned_results.size() == 1, "move-assigned engine should preserve indexed documents");
  Require(reassigned_results[0].frame_id == 102, "move-assigned engine returned unexpected frame_id");
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
    ScenarioStagedMutationsRequireCommit();
    ScenarioRollbackStagedMutations();
    ScenarioStagedOrderDeterminism();
    ScenarioMoveSemanticsPreserveIndexState();
    waxcpp::tests::Log("fts5_search_engine_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
