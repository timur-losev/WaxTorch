#pragma once

#include "ue5_filesystem_scanner.hpp"
#include "waxcpp/types.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace waxcpp::server {

struct Ue5ChunkRecord {
  std::string chunk_id{};
  std::string relative_path{};
  std::string language{};
  std::string symbol{};
  std::uint32_t line_start = 0;
  std::uint32_t line_end = 0;
  std::uint32_t token_estimate = 0;
  std::string content_hash{};
  std::uint64_t size_bytes = 0;
};

struct Ue5ChunkingConfig {
  waxcpp::ChunkingStrategy strategy{};
  bool include_symbol_metadata = true;
};

class Ue5ChunkManifestBuilder {
 public:
  using ChunkVisitor = std::function<void(const Ue5ChunkRecord&, std::string_view chunk_text)>;

  explicit Ue5ChunkManifestBuilder(Ue5ChunkingConfig config = {});

  [[nodiscard]] std::vector<Ue5ChunkRecord> Build(
      const std::filesystem::path& repo_root,
      const std::vector<Ue5ScanEntry>& entries,
      const ChunkVisitor& on_chunk = {}) const;

  [[nodiscard]] static std::string SerializeManifest(const std::vector<Ue5ChunkRecord>& records);

 private:
  [[nodiscard]] static std::string DetectLanguage(const std::string& relative_path);

  Ue5ChunkingConfig config_{};
};

}  // namespace waxcpp::server
