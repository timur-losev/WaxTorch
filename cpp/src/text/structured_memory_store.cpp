#include "waxcpp/structured_memory.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace waxcpp {
namespace {

bool EntryLess(const StructuredMemoryEntry& lhs, const StructuredMemoryEntry& rhs) {
  if (lhs.entity != rhs.entity) {
    return lhs.entity < rhs.entity;
  }
  if (lhs.attribute != rhs.attribute) {
    return lhs.attribute < rhs.attribute;
  }
  return lhs.id < rhs.id;
}

}  // namespace

std::string StructuredMemoryStore::CompositeKey(const std::string& entity, const std::string& attribute) {
  return entity + '\x1F' + attribute;
}

std::uint64_t StructuredMemoryStore::Upsert(const std::string& entity,
                                            const std::string& attribute,
                                            const std::string& value,
                                            const Metadata& metadata) {
  if (entity.empty()) {
    throw std::runtime_error("StructuredMemoryStore::Upsert entity must be non-empty");
  }
  if (attribute.empty()) {
    throw std::runtime_error("StructuredMemoryStore::Upsert attribute must be non-empty");
  }

  const auto key = CompositeKey(entity, attribute);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    StructuredMemoryEntry entry{};
    entry.id = next_id_++;
    entry.entity = entity;
    entry.attribute = attribute;
    entry.value = value;
    entry.metadata = metadata;
    entry.version = 1;
    const auto id = entry.id;
    entries_.emplace(key, std::move(entry));
    return id;
  }

  it->second.value = value;
  it->second.metadata = metadata;
  it->second.version += 1;
  return it->second.id;
}

bool StructuredMemoryStore::Remove(const std::string& entity, const std::string& attribute) {
  const auto key = CompositeKey(entity, attribute);
  return entries_.erase(key) > 0;
}

std::optional<StructuredMemoryEntry> StructuredMemoryStore::Get(const std::string& entity,
                                                                const std::string& attribute) const {
  const auto key = CompositeKey(entity, attribute);
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<StructuredMemoryEntry> StructuredMemoryStore::QueryByEntityPrefix(const std::string& entity_prefix,
                                                                               int limit) const {
  if (limit == 0) {
    return {};
  }
  std::vector<StructuredMemoryEntry> out{};
  out.reserve(entries_.size());
  for (const auto& [_, entry] : entries_) {
    if (!entity_prefix.empty() && entry.entity.rfind(entity_prefix, 0) != 0) {
      continue;
    }
    out.push_back(entry);
  }
  std::sort(out.begin(), out.end(), EntryLess);
  if (limit > 0 && out.size() > static_cast<std::size_t>(limit)) {
    out.resize(static_cast<std::size_t>(limit));
  }
  return out;
}

std::vector<StructuredMemoryEntry> StructuredMemoryStore::All(int limit) const {
  return QueryByEntityPrefix("", limit);
}

}  // namespace waxcpp
