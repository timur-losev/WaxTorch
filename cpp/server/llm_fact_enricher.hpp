// cpp/server/llm_fact_enricher.hpp
// LLM-based enricher: sends chunk text to llama-server (Qwen3)
// and parses JSON array of {entity, attribute, value} facts.
#pragma once

#include "chunk_enricher.hpp"
#include "llama_cpp_generation_client.hpp"

namespace waxcpp::server {

struct LlmFactEnricherConfig {
    int max_tokens = 1024;
    float temperature = 0.1f;
    bool skip_on_error = true;  // continue indexing if LLM fails
};

class LlmFactEnricher : public ChunkEnricher {
 public:
    explicit LlmFactEnricher(
        LlamaCppGenerationClient* client,  // non-owning
        LlmFactEnricherConfig config = {});

    [[nodiscard]] std::string Name() const override { return "llm"; }

    [[nodiscard]] FactBatch Enrich(
        const Ue5ChunkRecord& record,
        std::string_view chunk_text) override;

 private:
    [[nodiscard]] static std::string BuildSystemPrompt();
    [[nodiscard]] static std::string BuildUserPrompt(
        const Ue5ChunkRecord& record,
        std::string_view chunk_text);
    [[nodiscard]] static FactBatch ParseJsonResponse(
        const std::string& response,
        const Ue5ChunkRecord& record);
    [[nodiscard]] static std::string ExtractJsonArray(const std::string& text);

    LlamaCppGenerationClient* client_;
    LlmFactEnricherConfig config_;
};

}  // namespace waxcpp::server
