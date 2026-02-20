#include "waxcpp/vector_engine.hpp"

#include <stdexcept>

namespace waxcpp {

USearchVectorEngine::USearchVectorEngine(int dimensions) : dimensions_(dimensions) {}

int USearchVectorEngine::dimensions() const {
  return dimensions_;
}

std::vector<std::pair<std::uint64_t, float>> USearchVectorEngine::Search(
    const std::vector<float>& /*vector*/, int /*top_k*/) {
  throw std::runtime_error("USearchVectorEngine::Search not implemented");
}

void USearchVectorEngine::Add(std::uint64_t /*frame_id*/, const std::vector<float>& /*vector*/) {
  throw std::runtime_error("USearchVectorEngine::Add not implemented");
}

void USearchVectorEngine::AddBatch(const std::vector<std::uint64_t>& /*frame_ids*/,
                                   const std::vector<std::vector<float>>& /*vectors*/) {
  throw std::runtime_error("USearchVectorEngine::AddBatch not implemented");
}

void USearchVectorEngine::Remove(std::uint64_t /*frame_id*/) {
  throw std::runtime_error("USearchVectorEngine::Remove not implemented");
}

}  // namespace waxcpp
