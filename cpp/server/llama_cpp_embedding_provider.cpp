#include "llama_cpp_embedding_provider.hpp"

#include <Poco/Dynamic/Var.h>
#include <Poco/Exception.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/URI.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace waxcpp::server {

namespace {

void NormalizeL2(std::vector<float>& values) {
  double sum_sq = 0.0;
  for (const auto value : values) {
    sum_sq += static_cast<double>(value) * static_cast<double>(value);
  }
  if (sum_sq <= 0.0) {
    return;
  }
  const auto inv_norm = 1.0 / std::sqrt(sum_sq);
  for (auto& value : values) {
    value = static_cast<float>(static_cast<double>(value) * inv_norm);
  }
}

std::vector<float> ParseFloatArray(const Poco::JSON::Array::Ptr& array) {
  if (array.isNull() || array->empty()) {
    return {};
  }
  std::vector<float> values{};
  values.reserve(array->size());
  for (std::size_t i = 0; i < array->size(); ++i) {
    if (i > static_cast<std::size_t>(std::numeric_limits<unsigned int>::max())) {
      throw std::runtime_error("embedding array index overflow");
    }
    const auto v = array->get(static_cast<unsigned int>(i));
    if (v.isNumeric()) {
      values.push_back(v.convert<float>());
      continue;
    }
    throw std::runtime_error("embedding array contains non-numeric value");
  }
  return values;
}

std::vector<float> ParseEmbeddingObject(const Poco::JSON::Object::Ptr& object) {
  if (object.isNull()) {
    return {};
  }
  if (object->has("embedding")) {
    try {
      return ParseFloatArray(object->getArray("embedding"));
    } catch (const Poco::Exception& ex) {
      throw std::runtime_error(std::string("failed to parse embedding field: ") + ex.displayText());
    }
  }
  if (object->has("embeddings")) {
    try {
      const auto arr = object->getArray("embeddings");
      if (arr.isNull() || arr->empty()) {
        return {};
      }
      if (!arr->isArray(0)) {
        throw std::runtime_error("embeddings[0] is not an array");
      }
      return ParseFloatArray(arr->getArray(0));
    } catch (const Poco::Exception& ex) {
      throw std::runtime_error(std::string("failed to parse embeddings field: ") + ex.displayText());
    }
  }
  if (object->has("data")) {
    try {
      const auto arr = object->getArray("data");
      if (arr.isNull() || arr->empty()) {
        return {};
      }
      if (!arr->isObject(0)) {
        throw std::runtime_error("data[0] is not an object");
      }
      const auto first = arr->getObject(0);
      if (first.isNull() || !first->has("embedding")) {
        throw std::runtime_error("data[0].embedding is missing");
      }
      return ParseFloatArray(first->getArray("embedding"));
    } catch (const Poco::Exception& ex) {
      throw std::runtime_error(std::string("failed to parse data field: ") + ex.displayText());
    }
  }
  return {};
}

std::string JsonEscape(const std::string& value) {
  std::string out{};
  out.reserve(value.size() + 16);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

}  // namespace

LlamaCppEmbeddingProvider::LlamaCppEmbeddingProvider(LlamaCppEmbeddingProviderConfig config)
    : config_(std::move(config)) {
  if (config_.dimensions <= 0) {
    throw std::runtime_error("llama.cpp embedding provider requires positive dimensions");
  }
  if (config_.request_fn == nullptr && config_.endpoint.empty()) {
    throw std::runtime_error(
        "llama.cpp embedding provider requires endpoint or request_fn");
  }
}

int LlamaCppEmbeddingProvider::dimensions() const {
  return config_.dimensions;
}

bool LlamaCppEmbeddingProvider::normalize() const {
  return config_.normalize;
}

std::optional<waxcpp::EmbeddingIdentity> LlamaCppEmbeddingProvider::identity() const {
  return waxcpp::EmbeddingIdentity{
      .provider = std::string("llama.cpp"),
      .model = config_.model_path.empty() ? std::optional<std::string>{} : std::optional<std::string>{config_.model_path},
      .dimensions = config_.dimensions,
      .normalized = config_.normalize,
  };
}

std::vector<float> LlamaCppEmbeddingProvider::Embed(const std::string& text) {
  if (config_.memoization_capacity > 0) {
    std::lock_guard<std::mutex> lock(memoization_mutex_);
    const auto it = memoized_embeddings_.find(text);
    if (it != memoized_embeddings_.end()) {
      return it->second;
    }
  }

  auto embedding = FetchEmbedding(text);
  if (config_.normalize) {
    NormalizeL2(embedding);
  }

  if (config_.memoization_capacity > 0) {
    std::lock_guard<std::mutex> lock(memoization_mutex_);
    const auto cached = memoized_embeddings_.find(text);
    if (cached != memoized_embeddings_.end()) {
      return cached->second;
    }
    MemoizeLocked(text, embedding);
  }
  return embedding;
}

std::vector<std::vector<float>> LlamaCppEmbeddingProvider::EmbedBatch(const std::vector<std::string>& texts) {
  std::vector<std::vector<float>> out{};
  out.reserve(texts.size());
  for (const auto& text : texts) {
    out.push_back(Embed(text));
  }
  return out;
}

std::vector<float> LlamaCppEmbeddingProvider::ParseEmbeddingResponse(
    const std::string& payload,
    int expected_dimensions) {
  Poco::JSON::Parser parser{};
  Poco::Dynamic::Var parsed{};
  try {
    parsed = parser.parse(payload);
  } catch (const Poco::Exception& ex) {
    throw std::runtime_error(std::string("embedding response is not valid JSON: ") + ex.displayText());
  }

  Poco::JSON::Object::Ptr root{};
  try {
    root = parsed.extract<Poco::JSON::Object::Ptr>();
  } catch (const Poco::Exception&) {
    throw std::runtime_error("embedding response root must be a JSON object");
  }

  auto values = ParseEmbeddingObject(root);
  if (values.empty()) {
    throw std::runtime_error("embedding response does not contain a supported embedding field");
  }
  if (static_cast<int>(values.size()) != expected_dimensions) {
    throw std::runtime_error("embedding response dimension mismatch");
  }
  for (const auto value : values) {
    if (!std::isfinite(value)) {
      throw std::runtime_error("embedding response contains non-finite value");
    }
  }
  return values;
}

std::string LlamaCppEmbeddingProvider::RequestEmbeddingPayload(const std::string& text) const {
  std::ostringstream out;
  out << "{\"content\":\"" << JsonEscape(text) << "\"}";
  return out.str();
}

std::vector<float> LlamaCppEmbeddingProvider::FetchEmbedding(const std::string& text) const {
  const auto body = RequestEmbeddingPayload(text);
  if (config_.request_fn != nullptr) {
    return ParseEmbeddingResponse(config_.request_fn(body), config_.dimensions);
  }

  Poco::URI uri(config_.endpoint);
  auto path = uri.getPathEtc();
  if (path.empty()) {
    path = "/";
  }

  Poco::Net::HTTPClientSession session(uri.getHost(), uri.getPort());
  session.setTimeout(Poco::Timespan(0, config_.timeout_ms * 1000));

  Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST, path, Poco::Net::HTTPMessage::HTTP_1_1);
  request.setContentType("application/json");
  request.setContentLength(static_cast<int>(body.size()));
  request.set("Accept", "application/json");
  std::ostream& request_stream = session.sendRequest(request);
  request_stream.write(body.data(), static_cast<std::streamsize>(body.size()));

  Poco::Net::HTTPResponse response{};
  std::istream& response_stream = session.receiveResponse(response);
  std::ostringstream response_text{};
  response_text << response_stream.rdbuf();

  if (response.getStatus() >= 400) {
    throw std::runtime_error("llama.cpp embedding endpoint returned HTTP " + std::to_string(response.getStatus()));
  }
  return ParseEmbeddingResponse(response_text.str(), config_.dimensions);
}

void LlamaCppEmbeddingProvider::MemoizeLocked(const std::string& key, const std::vector<float>& embedding) {
  while (memoized_embeddings_.size() >= config_.memoization_capacity && !memoization_order_.empty()) {
    const auto evicted_key = memoization_order_.front();
    memoization_order_.pop_front();
    memoized_embeddings_.erase(evicted_key);
  }
  memoization_order_.push_back(key);
  memoized_embeddings_[key] = embedding;
}

}  // namespace waxcpp::server
