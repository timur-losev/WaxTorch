#include "waxcpp/structured_memory.hpp"

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

void ScenarioUpsertGetAndVersioning() {
  waxcpp::tests::Log("scenario: upsert/get/versioning");
  waxcpp::StructuredMemoryStore store;

  const auto id0 = store.Upsert("user:1", "name", "Alice", {{"src", "ingest"}});
  const auto first = store.Get("user:1", "name");
  Require(first.has_value(), "entry should exist after upsert");
  Require(first->id == id0, "id mismatch on first insert");
  Require(first->version == 1, "version should start at 1");
  Require(first->value == "Alice", "value mismatch after insert");
  Require(first->metadata.at("src") == "ingest", "metadata mismatch");

  const auto id1 = store.Upsert("user:1", "name", "Alice B", {{"src", "edit"}});
  Require(id1 == id0, "upsert should preserve id for same (entity,attribute)");
  const auto second = store.Get("user:1", "name");
  Require(second.has_value(), "entry should still exist after update");
  Require(second->version == 2, "version should increment on update");
  Require(second->value == "Alice B", "value mismatch after update");
  Require(second->metadata.at("src") == "edit", "metadata should be replaced on update");
}

void ScenarioRemove() {
  waxcpp::tests::Log("scenario: remove");
  waxcpp::StructuredMemoryStore store;
  (void)store.Upsert("user:1", "city", "Paris");
  Require(store.Remove("user:1", "city"), "remove should return true for existing entry");
  Require(!store.Remove("user:1", "city"), "remove should return false for missing entry");
  Require(!store.Get("user:1", "city").has_value(), "removed entry should not be found");
}

void ScenarioQueryDeterminismAndLimit() {
  waxcpp::tests::Log("scenario: query determinism and limit");
  waxcpp::StructuredMemoryStore store;
  (void)store.Upsert("user:2", "name", "Bob");
  (void)store.Upsert("user:1", "city", "Paris");
  (void)store.Upsert("user:1", "name", "Alice");
  (void)store.Upsert("order:1", "state", "paid");

  const auto user_entries = store.QueryByEntityPrefix("user:", -1);
  Require(user_entries.size() == 3, "prefix query count mismatch");
  Require(user_entries[0].entity == "user:1" && user_entries[0].attribute == "city",
          "deterministic ordering mismatch [0]");
  Require(user_entries[1].entity == "user:1" && user_entries[1].attribute == "name",
          "deterministic ordering mismatch [1]");
  Require(user_entries[2].entity == "user:2" && user_entries[2].attribute == "name",
          "deterministic ordering mismatch [2]");

  const auto limited = store.QueryByEntityPrefix("user:", 2);
  Require(limited.size() == 2, "limit should clamp query results");

  const auto none = store.QueryByEntityPrefix("missing:", -1);
  Require(none.empty(), "missing prefix should return empty result");
}

void ScenarioValidation() {
  waxcpp::tests::Log("scenario: validation");
  waxcpp::StructuredMemoryStore store;
  bool entity_throw = false;
  try {
    (void)store.Upsert("", "name", "x");
  } catch (const std::exception&) {
    entity_throw = true;
  }
  Require(entity_throw, "empty entity should throw");

  bool attribute_throw = false;
  try {
    (void)store.Upsert("user:1", "", "x");
  } catch (const std::exception&) {
    attribute_throw = true;
  }
  Require(attribute_throw, "empty attribute should throw");
}

void ScenarioStagedMutationVisibilityAndRollback() {
  waxcpp::tests::Log("scenario: staged mutation visibility and rollback");
  waxcpp::StructuredMemoryStore store;

  const auto staged_id = store.StageUpsert("user:1", "city", "Rome", {{"src", "stage"}});
  Require(store.PendingMutationCount() == 1, "stage upsert must increase pending mutation count");
  Require(!store.Get("user:1", "city").has_value(), "staged upsert must stay invisible before commit");
  Require(store.QueryByEntityPrefix("user:", -1).empty(),
          "staged upsert must not appear in committed query view");

  store.RollbackStaged();
  Require(store.PendingMutationCount() == 0, "rollback must clear pending mutation count");
  Require(!store.Get("user:1", "city").has_value(), "rollback must discard staged upsert");

  const auto committed_id = store.StageUpsert("user:1", "city", "Rome", {});
  Require(committed_id == staged_id, "id allocation should be deterministic across rollback retries");
  store.CommitStaged();
  Require(store.PendingMutationCount() == 0, "commit must clear pending mutation count");
  const auto committed = store.Get("user:1", "city");
  Require(committed.has_value(), "commit must publish staged upsert");
  Require(committed->id == committed_id, "committed staged id mismatch");
  Require(committed->value == "Rome", "committed staged value mismatch");

  (void)store.StageUpsert("user:1", "city", "Milan", {});
  Require(store.Get("user:1", "city")->value == "Rome",
          "staged update must stay invisible before commit");
  store.RollbackStaged();
  Require(store.Get("user:1", "city")->value == "Rome",
          "rollback must preserve last committed value");

  const auto removed_id = store.StageRemove("user:1", "city");
  Require(removed_id.has_value() && *removed_id == committed_id, "staged remove should expose removed id");
  Require(store.Get("user:1", "city").has_value(), "staged remove must stay invisible before commit");
  store.CommitStaged();
  Require(!store.Get("user:1", "city").has_value(), "commit should apply staged remove");
}

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("structured_memory_store_test: start");
    ScenarioUpsertGetAndVersioning();
    ScenarioRemove();
    ScenarioQueryDeterminismAndLimit();
    ScenarioValidation();
    ScenarioStagedMutationVisibilityAndRollback();
    waxcpp::tests::Log("structured_memory_store_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
