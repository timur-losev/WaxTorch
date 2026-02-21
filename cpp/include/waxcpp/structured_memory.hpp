#pragma once

#include "waxcpp/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace waxcpp {

struct StructuredMemoryEntry {
  std::uint64_t id = 0;
  std::string entity;
  std::string attribute;
  std::string value;
  Metadata metadata;
  std::uint64_t version = 0;
};

class StructuredMemoryStore {
 public:
  StructuredMemoryStore() = default;

  std::uint64_t Upsert(const std::string& entity,
                       const std::string& attribute,
                       const std::string& value,
                       const Metadata& metadata = {});
  bool Remove(const std::string& entity, const std::string& attribute);

  [[nodiscard]] std::optional<StructuredMemoryEntry> Get(const std::string& entity,
                                                         const std::string& attribute) const;
  [[nodiscard]] std::vector<StructuredMemoryEntry> QueryByEntityPrefix(const std::string& entity_prefix,
                                                                       int limit) const;
  [[nodiscard]] std::vector<StructuredMemoryEntry> All(int limit = -1) const;

 private:
  static std::string CompositeKey(const std::string& entity, const std::string& attribute);

  std::uint64_t next_id_ = 0;
  std::unordered_map<std::string, StructuredMemoryEntry> entries_;
};

}  // namespace waxcpp
