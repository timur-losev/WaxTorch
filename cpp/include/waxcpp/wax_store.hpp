#pragma once

#include "waxcpp/types.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace waxcpp {

struct WaxStats {
  std::uint64_t frame_count = 0;
  std::uint64_t pending_frames = 0;
  std::uint64_t generation = 0;
};

class WaxStore {
 public:
  static WaxStore Create(const std::filesystem::path& path);
  static WaxStore Open(const std::filesystem::path& path);

  void Verify(bool deep);

  std::uint64_t Put(const std::vector<std::byte>& content, const Metadata& metadata = {});
  std::vector<std::uint64_t> PutBatch(const std::vector<std::vector<std::byte>>& contents,
                                      const std::vector<Metadata>& metadatas);
  void Delete(std::uint64_t frame_id);
  void Supersede(std::uint64_t superseded_id, std::uint64_t superseding_id);

  void Commit();
  void Close();

 [[nodiscard]] WaxStats Stats() const;

 private:
  void LoadState(bool deep_verify);
  explicit WaxStore(std::filesystem::path path);

  std::filesystem::path path_;
  std::uint64_t file_generation_ = 0;
  std::uint64_t wal_committed_seq_ = 0;
  std::uint64_t footer_offset_ = 0;
  bool is_open_ = false;
  WaxStats stats_{};
};

}  // namespace waxcpp
