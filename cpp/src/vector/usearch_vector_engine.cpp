#include "waxcpp/vector_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace waxcpp {
namespace {

std::atomic<std::uint32_t> g_test_commit_fail_countdown{0};

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

void MaybeInjectCommitFailure() {
  auto remaining = g_test_commit_fail_countdown.load(std::memory_order_relaxed);
  while (remaining > 0) {
    if (g_test_commit_fail_countdown.compare_exchange_weak(remaining,
                                                           remaining - 1,
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
      throw std::runtime_error("USearchVectorEngine::CommitStaged injected failure");
    }
  }
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

std::vector<std::pair<std::uint64_t, float>> USearchVectorEngine::Search(const std::vector<float>& vector,
                                                                          int top_k) const {
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

void USearchVectorEngine::StageAdd(std::uint64_t frame_id, const std::vector<float>& vector) {
  if (vector.size() != static_cast<std::size_t>(dimensions_)) {
    throw std::runtime_error("USearchVectorEngine::StageAdd dimension mismatch");
  }
  pending_mutations_.push_back(PendingMutation{PendingMutationType::kAdd, frame_id, vector});
}

void USearchVectorEngine::StageAddBatch(const std::vector<std::uint64_t>& frame_ids,
                                        const std::vector<std::vector<float>>& vectors) {
  if (frame_ids.size() != vectors.size()) {
    throw std::runtime_error("USearchVectorEngine::StageAddBatch size mismatch");
  }
  pending_mutations_.reserve(pending_mutations_.size() + frame_ids.size());
  for (std::size_t i = 0; i < frame_ids.size(); ++i) {
    StageAdd(frame_ids[i], vectors[i]);
  }
}

void USearchVectorEngine::StageRemove(std::uint64_t frame_id) {
  pending_mutations_.push_back(PendingMutation{PendingMutationType::kRemove, frame_id, {}});
}

void USearchVectorEngine::CommitStaged() {
  MaybeInjectCommitFailure();
  for (auto& mutation : pending_mutations_) {
    if (mutation.type == PendingMutationType::kAdd) {
      vectors_[mutation.frame_id] = std::move(mutation.vector);
      continue;
    }
    vectors_.erase(mutation.frame_id);
  }
  pending_mutations_.clear();
}

void USearchVectorEngine::RollbackStaged() {
  pending_mutations_.clear();
}

std::size_t USearchVectorEngine::PendingMutationCount() const {
  return pending_mutations_.size();
}

void USearchVectorEngine::Add(std::uint64_t frame_id, const std::vector<float>& vector) {
  StageAdd(frame_id, vector);
  CommitStaged();
}

void USearchVectorEngine::AddBatch(const std::vector<std::uint64_t>& frame_ids,
                                   const std::vector<std::vector<float>>& vectors) {
  StageAddBatch(frame_ids, vectors);
  CommitStaged();
}

void USearchVectorEngine::Remove(std::uint64_t frame_id) {
  StageRemove(frame_id);
  CommitStaged();
}

namespace vector::testing {

void SetCommitFailCountdown(std::uint32_t countdown) {
  g_test_commit_fail_countdown.store(countdown, std::memory_order_relaxed);
}

void ClearCommitFailCountdown() {
  g_test_commit_fail_countdown.store(0, std::memory_order_relaxed);
}

}  // namespace vector::testing

}  // namespace waxcpp
