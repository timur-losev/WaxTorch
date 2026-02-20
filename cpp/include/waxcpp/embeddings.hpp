#pragma once

#include "waxcpp/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace waxcpp {

struct EmbeddingIdentity {
  std::optional<std::string> provider;
  std::optional<std::string> model;
  std::optional<int> dimensions;
  std::optional<bool> normalized;
};

class EmbeddingProvider {
 public:
  virtual ~EmbeddingProvider() = default;

  virtual int dimensions() const = 0;
  virtual bool normalize() const = 0;
  virtual std::optional<EmbeddingIdentity> identity() const = 0;
  virtual std::vector<float> Embed(const std::string& text) = 0;
};

class BatchEmbeddingProvider : public EmbeddingProvider {
 public:
  ~BatchEmbeddingProvider() override = default;
  virtual std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) = 0;
};

class MiniLMEmbedderTorch final : public BatchEmbeddingProvider {
 public:
  MiniLMEmbedderTorch();

  int dimensions() const override;
  bool normalize() const override;
  std::optional<EmbeddingIdentity> identity() const override;
  std::vector<float> Embed(const std::string& text) override;
  std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts) override;
};

}  // namespace waxcpp
