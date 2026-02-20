#include "waxcpp/embeddings.hpp"

#include <stdexcept>

namespace waxcpp {

MiniLMEmbedderTorch::MiniLMEmbedderTorch() = default;

int MiniLMEmbedderTorch::dimensions() const {
  return 384;
}

bool MiniLMEmbedderTorch::normalize() const {
  return true;
}

std::optional<EmbeddingIdentity> MiniLMEmbedderTorch::identity() const {
  return EmbeddingIdentity{
      .provider = std::string("WaxCpp"),
      .model = std::string("MiniLM-Torch"),
      .dimensions = 384,
      .normalized = true,
  };
}

std::vector<float> MiniLMEmbedderTorch::Embed(const std::string& /*text*/) {
  throw std::runtime_error("MiniLMEmbedderTorch::Embed not implemented");
}

std::vector<std::vector<float>> MiniLMEmbedderTorch::EmbedBatch(const std::vector<std::string>& /*texts*/) {
  throw std::runtime_error("MiniLMEmbedderTorch::EmbedBatch not implemented");
}

}  // namespace waxcpp
