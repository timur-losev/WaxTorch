#include "waxcpp/vector_engine.hpp"

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace waxcpp {
namespace {

float Dot(std::span<const float> lhs, std::span<const float> rhs) {
  float dot = 0.0F;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    dot += lhs[i] * rhs[i];
  }
  return dot;
}

float Norm(std::span<const float> v) {
  const auto dot = Dot(v, v);
  return std::sqrt(std::max(dot, 0.0F));
}

float CosineSimilarity(std::span<const float> lhs, std::span<const float> rhs) {
  const auto lhs_norm = Norm(lhs);
  const auto rhs_norm = Norm(rhs);
  if (lhs_norm <= 0.0F || rhs_norm <= 0.0F) {
    return 0.0F;
  }
  return Dot(lhs, rhs) / (lhs_norm * rhs_norm);
}

}  // namespace

USearchVectorEngine::USearchVectorEngine(int dimensions) : dimensions_(dimensions) {
  if (dimensions_ <= 0) {
    throw std::runtime_error("USearchVectorEngine dimensions must be positive");
  }
}

int USearchVectorEngine::dimensions() const {
  return dimensions_;
}

std::vector<std::pair<std::uint64_t, float>> USearchVectorEngine::Search(const std::vector<float>& vector, int top_k) {
  if (vector.size() != static_cast<std::size_t>(dimensions_)) {
    throw std::runtime_error("USearchVectorEngine::Search dimension mismatch");
  }
  if (top_k <= 0 || vectors_.empty()) {
    return {};
  }

  std::vector<std::pair<std::uint64_t, float>> results{};
  results.reserve(vectors_.size());
  for (const auto& [frame_id, candidate] : vectors_) {
    results.emplace_back(frame_id,
                         CosineSimilarity(std::span<const float>(vector.data(), vector.size()),
                                          std::span<const float>(candidate.data(), candidate.size())));
  }

  std::sort(results.begin(), results.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.second != rhs.second) {
      return lhs.second > rhs.second;
    }
    return lhs.first < rhs.first;
  });

  const auto target_size = std::min<std::size_t>(results.size(), static_cast<std::size_t>(top_k));
  results.resize(target_size);
  return results;
}

void USearchVectorEngine::Add(std::uint64_t frame_id, const std::vector<float>& vector) {
  if (vector.size() != static_cast<std::size_t>(dimensions_)) {
    throw std::runtime_error("USearchVectorEngine::Add dimension mismatch");
  }
  vectors_[frame_id] = vector;
}

void USearchVectorEngine::AddBatch(const std::vector<std::uint64_t>& frame_ids,
                                   const std::vector<std::vector<float>>& vectors) {
  if (frame_ids.size() != vectors.size()) {
    throw std::runtime_error("USearchVectorEngine::AddBatch size mismatch");
  }
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    Add(frame_ids[i], vectors[i]);
  }
}

void USearchVectorEngine::Remove(std::uint64_t frame_id) {
  vectors_.erase(frame_id);
}

}  // namespace waxcpp
