// cpp/server/llm_fact_enricher.cpp
#include "llm_fact_enricher.hpp"
#include "server_utils.hpp"

#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#include <chrono>
#include <iostream>
#include <sstream>

namespace waxcpp::server {

namespace {

bool EnrichLlmLogEnabled() {
    static const bool enabled = []() {
        const auto raw = EnvString("WAXCPP_ENRICH_LLM_LOG");
        if (!raw.has_value()) return false;
        const auto& v = *raw;
        return v == "1" || v == "true" || v == "TRUE" || v == "on" || v == "ON";
    }();
    return enabled;
}

}  // namespace

LlmFactEnricher::LlmFactEnricher(
    LlamaCppGenerationClient* client,
    LlmFactEnricherConfig config)
    : client_(client), config_(config) {}

// ── Prompts ──────────────────────────────────────────────────

std::string LlmFactEnricher::BuildSystemPrompt() {
    return
        "You are a code analysis assistant. Extract structured facts from the given C++ code chunk.\n"
        "Return a JSON array of objects with exactly these fields:\n"
        "- \"entity\": The class, function, or component name (e.g. \"AMyActor\", \"AMyActor::TakeDamage\")\n"
        "- \"attribute\": The relationship type (e.g. \"inherits\", \"returns\", \"calls\", \"depends_on\", \"purpose\")\n"
        "- \"value\": The value of the relationship\n"
        "\n"
        "Focus on: inheritance, function signatures, dependencies, design patterns, semantic purpose.\n"
        "Only extract facts you are confident about. Return [] for trivial or boilerplate code.\n"
        "Respond ONLY with the JSON array, no other text.";
}

std::string LlmFactEnricher::BuildUserPrompt(
    const Ue5ChunkRecord& record,
    std::string_view chunk_text) {
    std::ostringstream out;
    out << "File: " << record.relative_path
        << " (lines " << record.line_start << "-" << record.line_end
        << ", language: " << record.language << ")\n";
    if (!record.symbol.empty()) {
        out << "Symbol context: " << record.symbol << "\n";
    }
    out << "\n```cpp\n" << chunk_text << "\n```\n\n"
        << "Extract facts as JSON:\n/no_think";
    return out.str();
}

// ── JSON response parsing ────────────────────────────────────

std::string LlmFactEnricher::ExtractJsonArray(const std::string& text) {
    // Strip markdown code fences if present
    auto start = text.find('[');
    if (start == std::string::npos) return "[]";

    // Find matching closing bracket
    int depth = 0;
    for (std::size_t i = start; i < text.size(); ++i) {
        if (text[i] == '[') ++depth;
        else if (text[i] == ']') {
            --depth;
            if (depth == 0) {
                return text.substr(start, i - start + 1);
            }
        }
    }
    return "[]";
}

FactBatch LlmFactEnricher::ParseJsonResponse(
    const std::string& response,
    const Ue5ChunkRecord& record) {

    FactBatch out;
    const auto json_str = ExtractJsonArray(response);

    Poco::JSON::Parser parser;
    const auto parsed = parser.parse(json_str);
    const auto arr = parsed.extract<Poco::JSON::Array::Ptr>();
    if (arr.isNull()) return out;

    const waxcpp::Metadata meta = {
        {"enricher_kind", "llm"},
        {"source_path", record.relative_path},
        {"source_lines", std::to_string(record.line_start) + "-" + std::to_string(record.line_end)},
        {"chunk_id", record.chunk_id},
    };

    for (std::size_t i = 0; i < arr->size(); ++i) {
        const auto obj = arr->getObject(static_cast<unsigned int>(i));
        if (obj.isNull()) continue;

        const auto entity = obj->optValue<std::string>("entity", "");
        const auto attribute = obj->optValue<std::string>("attribute", "");
        const auto value = obj->optValue<std::string>("value", "");

        if (entity.empty() || attribute.empty()) continue;

        // Add cpp: prefix if not already present
        std::string prefixed_entity = entity;
        if (!entity.starts_with("cpp:") && !entity.starts_with("file:")) {
            prefixed_entity = "cpp:" + entity;
        }

        out.push_back({std::move(prefixed_entity), attribute, value, meta});
    }

    return out;
}

// ── Main Enrich method ───────────────────────────────────────

FactBatch LlmFactEnricher::Enrich(
    const Ue5ChunkRecord& record,
    std::string_view chunk_text) {

    if (!client_) return {};
    if (chunk_text.empty()) return {};

    const bool verbose = EnrichLlmLogEnabled();

    try {
        const auto user_prompt = BuildUserPrompt(record, chunk_text);
        const auto system_prompt = BuildSystemPrompt();

        if (verbose) {
            std::cerr << "\n[ENRICH-LLM] ── REQUEST ──────────────────────\n"
                      << "[ENRICH-LLM] file: " << record.relative_path
                      << " lines " << record.line_start << "-" << record.line_end
                      << " symbol: " << record.symbol << "\n"
                      << "[ENRICH-LLM] chunk (" << chunk_text.size() << " chars):\n"
                      << chunk_text.substr(0, 500);
            if (chunk_text.size() > 500) {
                std::cerr << "\n... (" << (chunk_text.size() - 500) << " more chars)";
            }
            std::cerr << "\n[ENRICH-LLM] ─────────────────────────────────\n"
                      << std::flush;
        }

        const auto t0 = std::chrono::steady_clock::now();

        const auto response = client_->Generate(LlamaCppGenerationRequest{
            .prompt = user_prompt,
            .system_prompt = system_prompt,
            .max_tokens = config_.max_tokens,
            .temperature = config_.temperature,
        });

        const auto t1 = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        auto facts = ParseJsonResponse(response, record);

        if (verbose) {
            std::cerr << "[ENRICH-LLM] ── RESPONSE (" << elapsed_ms << "ms) ────────────\n"
                      << "[ENRICH-LLM] raw (" << response.size() << " chars): "
                      << response.substr(0, 800);
            if (response.size() > 800) {
                std::cerr << "\n... (" << (response.size() - 800) << " more chars)";
            }
            std::cerr << "\n[ENRICH-LLM] facts extracted: " << facts.size() << "\n";
            for (std::size_t i = 0; i < facts.size(); ++i) {
                std::cerr << "[ENRICH-LLM]   [" << i << "] "
                          << facts[i].entity << " | "
                          << facts[i].attribute << " | "
                          << facts[i].value << "\n";
            }
            std::cerr << "[ENRICH-LLM] ─────────────────────────────────\n"
                      << std::flush;
        } else {
            // Compact one-liner even without verbose
            std::cerr << "[ENRICH-LLM] " << record.relative_path
                      << ":" << record.line_start << "-" << record.line_end
                      << " -> " << facts.size() << " facts (" << elapsed_ms << "ms)\n";
        }

        return facts;

    } catch (const std::exception& e) {
        std::cerr << "[ENRICH-LLM] error: " << e.what()
                  << " file=" << record.relative_path << "\n";
        if (config_.skip_on_error) return {};
        throw;
    }
}

}  // namespace waxcpp::server
