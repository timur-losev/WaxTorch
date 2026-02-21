#include "waxcpp/vector_engine.hpp"

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

void ScenarioCtorValidation() {
  waxcpp::tests::Log("scenario: constructor validation");
  bool threw = false;
  try {
    waxcpp::USearchVectorEngine invalid(0);
  } catch (const std::exception&) {
    threw = true;
  }
  Require(threw, "dimensions <= 0 must throw");
}

void ScenarioAddSearchAndTieBreak() {
  waxcpp::tests::Log("scenario: add/search/tie-break");
  waxcpp::USearchVectorEngine engine(3);
  engine.Add(10, {1.0F, 0.0F, 0.0F});
  engine.Add(2, {1.0F, 0.0F, 0.0F});
  engine.Add(7, {0.0F, 1.0F, 0.0F});

  const auto results = engine.Search({1.0F, 0.0F, 0.0F}, 3);
  Require(results.size() == 3, "expected three vector results");
  Require(results[0].first == 2, "tie-break should prefer lower frame_id");
  Require(results[1].first == 10, "second tie entry should be higher frame_id");
  Require(results[0].second == results[1].second, "tie entries should have equal score");
  Require(results[2].first == 7, "orthogonal vector should rank lower");
}

void ScenarioBatchAndRemove() {
  waxcpp::tests::Log("scenario: batch add and remove");
  waxcpp::USearchVectorEngine engine(2);
  engine.AddBatch({1, 2}, {{1.0F, 0.0F}, {0.0F, 1.0F}});
  engine.Remove(2);

  const auto results = engine.Search({0.0F, 1.0F}, 10);
  Require(results.size() == 1, "remove should drop frame from index");
  Require(results[0].first == 1, "remaining frame mismatch");
}

void ScenarioValidationErrors() {
  waxcpp::tests::Log("scenario: validation errors");
  waxcpp::USearchVectorEngine engine(2);

  bool add_threw = false;
  try {
    engine.Add(1, {1.0F});
  } catch (const std::exception&) {
    add_threw = true;
  }
  Require(add_threw, "Add must throw on dimension mismatch");

  bool batch_size_threw = false;
  try {
    engine.AddBatch({1, 2}, {{1.0F, 0.0F}});
  } catch (const std::exception&) {
    batch_size_threw = true;
  }
  Require(batch_size_threw, "AddBatch must throw on size mismatch");

  bool search_threw = false;
  try {
    (void)engine.Search({1.0F}, 10);
  } catch (const std::exception&) {
    search_threw = true;
  }
  Require(search_threw, "Search must throw on dimension mismatch");
}

void ScenarioTopKAndEmptyCases() {
  waxcpp::tests::Log("scenario: top_k and empty cases");
  waxcpp::USearchVectorEngine engine(2);
  engine.Add(1, {1.0F, 0.0F});
  engine.Add(2, {0.0F, 1.0F});

  Require(engine.Search({1.0F, 0.0F}, 0).empty(), "top_k=0 must return empty");
  Require(engine.Search({1.0F, 0.0F}, -1).empty(), "top_k<0 must return empty");

  const auto results = engine.Search({1.0F, 0.0F}, 1);
  Require(results.size() == 1, "top_k clamp must limit result count");
}

void ScenarioStagedMutationsRequireCommit() {
  waxcpp::tests::Log("scenario: staged mutations require commit");
  waxcpp::USearchVectorEngine engine(2);
  engine.StageAdd(1, {1.0F, 0.0F});
  Require(engine.PendingMutationCount() == 1, "pending mutation count mismatch after stage add");
  Require(engine.Search({1.0F, 0.0F}, 10).empty(), "staged vector add should not be visible before commit");

  engine.CommitStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after commit");
  Require(engine.Search({1.0F, 0.0F}, 10).size() == 1, "committed staged vector should be searchable");

  engine.StageRemove(1);
  Require(engine.Search({1.0F, 0.0F}, 10).size() == 1, "staged remove should stay invisible before commit");
  engine.CommitStaged();
  Require(engine.Search({1.0F, 0.0F}, 10).empty(), "committed staged remove should clear vector");
}

void ScenarioRollbackStagedMutations() {
  waxcpp::tests::Log("scenario: rollback staged mutations");
  waxcpp::USearchVectorEngine engine(2);
  engine.StageAddBatch({10, 11}, {{1.0F, 0.0F}, {0.0F, 1.0F}});
  Require(engine.PendingMutationCount() == 2, "pending mutation count mismatch after stage batch");
  engine.RollbackStaged();
  Require(engine.PendingMutationCount() == 0, "pending mutation count should reset after rollback");
  Require(engine.Search({1.0F, 0.0F}, 10).empty(), "rolled-back staged vectors should not be visible");
}

void ScenarioStagedOrderDeterminism() {
  waxcpp::tests::Log("scenario: staged order determinism");
  waxcpp::USearchVectorEngine engine(2);
  engine.StageAdd(7, {1.0F, 0.0F});
  engine.StageAdd(7, {0.0F, 1.0F});
  engine.StageRemove(7);
  engine.StageAdd(7, {1.0F, 0.0F});
  Require(engine.PendingMutationCount() == 4, "pending mutation count mismatch in order scenario");
  engine.CommitStaged();

  const auto results = engine.Search({1.0F, 0.0F}, 10);
  Require(results.size() == 1, "final staged state should leave one vector");
  Require(results[0].first == 7, "unexpected frame_id after staged mutation order apply");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("usearch_vector_engine_test: start");
    ScenarioCtorValidation();
    ScenarioAddSearchAndTieBreak();
    ScenarioBatchAndRemove();
    ScenarioValidationErrors();
    ScenarioTopKAndEmptyCases();
    ScenarioStagedMutationsRequireCommit();
    ScenarioRollbackStagedMutations();
    ScenarioStagedOrderDeterminism();
    waxcpp::tests::Log("usearch_vector_engine_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
