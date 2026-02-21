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

}  // namespace

int main() {
  try {
    waxcpp::tests::Log("structured_memory_store_test: start");
    ScenarioUpsertGetAndVersioning();
    ScenarioRemove();
    ScenarioQueryDeterminismAndLimit();
    ScenarioValidation();
    waxcpp::tests::Log("structured_memory_store_test: finished");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    waxcpp::tests::LogError(ex.what());
    return EXIT_FAILURE;
  }
}
