#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace waxcpp {

class VectorSearchEngine {
 public:
  virtual ~VectorSearchEngine() = default;

  virtual int dimensions() const = 0;
  virtual std::vector<std::pair<std::uint64_t, float>> Search(const std::vector<float>& vector, int top_k) = 0;
  virtual void Add(std::uint64_t frame_id, const std::vector<float>& vector) = 0;
  virtual void AddBatch(const std::vector<std::uint64_t>& frame_ids, const std::vector<std::vector<float>>& vectors) = 0;
  virtual void Remove(std::uint64_t frame_id) = 0;
};

class USearchVectorEngine final : public VectorSearchEngine {
 public:
  explicit USearchVectorEngine(int dimensions);

  int dimensions() const override;
  std::vector<std::pair<std::uint64_t, float>> Search(const std::vector<float>& vector, int top_k) override;
  void Add(std::uint64_t frame_id, const std::vector<float>& vector) override;
  void AddBatch(const std::vector<std::uint64_t>& frame_ids, const std::vector<std::vector<float>>& vectors) override;
  void Remove(std::uint64_t frame_id) override;

 private:
  int dimensions_;
  std::unordered_map<std::uint64_t, std::vector<float>> vectors_;
};

}  // namespace waxcpp
