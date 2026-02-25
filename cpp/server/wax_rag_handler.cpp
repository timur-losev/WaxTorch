// cpp/server/wax_rag_handler.cpp
#include "wax_rag_handler.hpp"
#include "runtime_config.hpp"

#include <Poco/Exception.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <iostream>
#include <chrono>

namespace waxcpp::server {

namespace {
constexpr std::uint64_t kIndexFlushEveryChunks = 128;
constexpr std::uint64_t kMaxIndexControlValue = 1'000'000;
constexpr const char* kDefaultLlamaEmbedEndpoint = "http://127.0.0.1:8081/embedding";
constexpr const char* kDefaultLlamaGenEndpoint = "http://127.0.0.1:8081/completion";
constexpr const char* kServerLogEnv = "WAXCPP_SERVER_LOG";

std::optional<std::string> EnvString(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out(value);
    std::free(value);
    if (out.empty()) {
        return std::nullopt;
    }
    return out;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

int ParsePositiveIntEnv(const char* name, int fallback) {
    const auto raw = EnvString(name);
    if (!raw.has_value()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(*raw, &consumed, 10);
        if (consumed != raw->size() || parsed <= 0) {
            throw std::runtime_error("");
        }
        return parsed;
    } catch (...) {
        throw std::runtime_error(std::string("invalid positive integer env value for ") + name + ": " + *raw);
    }
}

int ParseNonNegativeIntEnv(const char* name, int fallback) {
    const auto raw = EnvString(name);
    if (!raw.has_value()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(*raw, &consumed, 10);
        if (consumed != raw->size() || parsed < 0) {
            throw std::runtime_error("");
        }
        return parsed;
    } catch (...) {
        throw std::runtime_error(std::string("invalid non-negative integer env value for ") + name + ": " + *raw);
    }
}

float ParseFloatParam(const Poco::JSON::Object::Ptr& params,
                      const std::string& key,
                      float fallback) {
    if (params.isNull() || !params->has(key)) {
        return fallback;
    }
    try {
        return params->getValue<float>(key);
    } catch (const Poco::Exception&) {
        return fallback;
    }
}

int ParsePositiveIntParam(const Poco::JSON::Object::Ptr& params,
                          const std::string& key,
                          int fallback) {
    if (params.isNull() || !params->has(key)) {
        return fallback;
    }
    try {
        const auto value = params->getValue<int>(key);
        return value > 0 ? value : fallback;
    } catch (const Poco::Exception&) {
        return fallback;
    }
}

int ParseNonNegativeIntParam(const Poco::JSON::Object::Ptr& params,
                             const std::string& key,
                             int fallback) {
    if (params.isNull() || !params->has(key)) {
        return fallback;
    }
    try {
        const auto value = params->getValue<int>(key);
        return value >= 0 ? value : fallback;
    } catch (const Poco::Exception&) {
        return fallback;
    }
}

bool ServerLogEnabled() {
    const auto raw = EnvString(kServerLogEnv);
    if (!raw.has_value()) {
        return false;
    }
    const auto& value = *raw;
    return value == "1" || value == "true" || value == "TRUE" || value == "on" || value == "ON";
}

void ServerLog(std::string_view message) {
    if (!ServerLogEnabled()) {
        return;
    }
    std::cerr << "[waxcpp-server] " << message << "\n";
}

std::uint64_t NowMs() {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return static_cast<std::uint64_t>(now.time_since_epoch().count());
}

struct CitationInfo {
    std::uint64_t frame_id = 0;
    std::string relative_path{};
    std::optional<int> line_start{};
    std::optional<int> line_end{};
    std::string symbol{};
    float score = 0.0f;
    waxcpp::RAGItemKind kind = waxcpp::RAGItemKind::kSnippet;
};

struct PromptBuildResult {
    std::string prompt{};
    int context_items_used = 0;
    int context_tokens_used = 0;
};

std::optional<int> ParseOptionalInt(const std::unordered_map<std::string, std::string>& metadata,
                                    const char* key) {
    const auto it = metadata.find(key);
    if (it == metadata.end() || it->second.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t consumed = 0;
        const int value = std::stoi(it->second, &consumed, 10);
        if (consumed != it->second.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<CitationInfo> BuildCitations(waxcpp::MemoryOrchestrator& orchestrator,
                                         const waxcpp::RAGContext& context,
                                         int max_context_items) {
    std::unordered_map<std::uint64_t, CitationInfo> by_frame{};
    int seen_items = 0;
    for (const auto& item : context.items) {
        if (seen_items >= max_context_items) {
            break;
        }
        ++seen_items;
        auto [it, inserted] = by_frame.emplace(item.frame_id,
                                               CitationInfo{
                                                   .frame_id = item.frame_id,
                                                   .score = item.score,
                                                   .kind = item.kind,
                                               });
        if (!inserted) {
            continue;
        }
        const auto meta = orchestrator.FrameMeta(item.frame_id);
        if (!meta.has_value()) {
            continue;
        }
        const auto path_it = meta->metadata.find("relative_path");
        if (path_it != meta->metadata.end()) {
            it->second.relative_path = path_it->second;
        }
        it->second.line_start = ParseOptionalInt(meta->metadata, "line_start");
        it->second.line_end = ParseOptionalInt(meta->metadata, "line_end");
        const auto symbol_it = meta->metadata.find("symbol");
        if (symbol_it != meta->metadata.end()) {
            it->second.symbol = symbol_it->second;
        }
    }

    std::vector<CitationInfo> citations{};
    citations.reserve(by_frame.size());
    for (const auto& [_, citation] : by_frame) {
        citations.push_back(citation);
    }
    std::sort(citations.begin(), citations.end(), [](const CitationInfo& lhs, const CitationInfo& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.frame_id < rhs.frame_id;
    });
    return citations;
}

int CountApproxTokens(std::string_view text) {
    int tokens = 0;
    bool in_token = false;
    for (const char ch : text) {
        const bool ws = (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t' || ch == '\f' || ch == '\v');
        if (ws) {
            in_token = false;
            continue;
        }
        if (!in_token) {
            ++tokens;
            in_token = true;
        }
    }
    return tokens > 0 ? tokens : (text.empty() ? 0 : 1);
}

PromptBuildResult BuildAnswerPrompt(const std::string& query,
                                    const waxcpp::RAGContext& context,
                                    const std::vector<CitationInfo>& citations,
                                    int max_context_items,
                                    int max_context_tokens) {
    PromptBuildResult result{};
    std::ostringstream prompt;
    prompt << "You are a code assistant. Answer the query using only the provided context.\n"
           << "When you cite facts, include citation tags like [frame:<id>].\n\n"
           << "Query:\n" << query << "\n\n"
           << "Context:\n";

    const int safe_max_tokens = std::max(1, max_context_tokens);
    for (const auto& item : context.items) {
        if (result.context_items_used >= max_context_items) {
            break;
        }
        const int item_tokens = std::max(1, CountApproxTokens(item.text));
        if (result.context_items_used > 0 && (result.context_tokens_used + item_tokens) > safe_max_tokens) {
            break;
        }
        ++result.context_items_used;
        result.context_tokens_used += item_tokens;
        prompt << "- [frame:" << item.frame_id << "] " << item.text << "\n";
    }

    prompt << "\nCitation Map:\n";
    for (const auto& citation : citations) {
        prompt << "- [frame:" << citation.frame_id << "] ";
        if (!citation.relative_path.empty()) {
            prompt << citation.relative_path;
        } else {
            prompt << "(path unavailable)";
        }
        if (citation.line_start.has_value()) {
            prompt << ":" << *citation.line_start;
            if (citation.line_end.has_value()) {
                prompt << "-" << *citation.line_end;
            }
        }
        if (!citation.symbol.empty()) {
            prompt << " symbol=" << citation.symbol;
        }
        prompt << "\n";
    }
    prompt << "\nProvide concise technical answer with citations.";
    result.prompt = prompt.str();
    return result;
}

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path.string());
    }
    std::ostringstream out;
    out << in.rdbuf();
    if (!in.good() && !in.eof()) {
        throw std::runtime_error("failed to read file: " + path.string());
    }
    return out.str();
}

void WriteFileText(const std::filesystem::path& path,
                   std::string_view content,
                   std::string_view open_error,
                   std::string_view write_error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error(std::string(open_error));
    }
    out << content;
    if (!out) {
        throw std::runtime_error(std::string(write_error));
    }
}
}

WaxRAGHandler::WaxRAGHandler(const std::filesystem::path& store_path,
                             waxcpp::RuntimeModelsConfig runtime_models,
                             std::unique_ptr<LlamaCppGenerationClient> generation_client_override)
    : index_job_manager_(store_path.string() + ".index.checkpoint"),
      runtime_models_(std::move(runtime_models)) {
    if (runtime_models_.generation_model.runtime.empty() &&
        runtime_models_.generation_model.model_path.empty() &&
        runtime_models_.embedding_model.runtime.empty() &&
        runtime_models_.embedding_model.model_path.empty() &&
        runtime_models_.llama_cpp_root.empty() &&
        !runtime_models_.enable_vector_search) {
        runtime_models_ = DefaultServerRuntimeConfig().models;
    }
    waxcpp::ValidateRuntimeModelsConfig(runtime_models_);
    waxcpp::OrchestratorConfig config{};
    config.enable_vector_search = runtime_models_.enable_vector_search;
    config.require_on_device_providers = false;
    std::shared_ptr<waxcpp::EmbeddingProvider> embedder{};
    if (runtime_models_.enable_vector_search) {
        LlamaCppEmbeddingProviderConfig embedder_config{};
        embedder_config.endpoint = EnvString("WAXCPP_LLAMA_EMBED_ENDPOINT").value_or(kDefaultLlamaEmbedEndpoint);
        embedder_config.model_path = runtime_models_.embedding_model.model_path;
        embedder_config.dimensions = ParsePositiveIntEnv("WAXCPP_LLAMA_EMBED_DIMS", 1024);
        embedder_config.timeout_ms = ParsePositiveIntEnv("WAXCPP_LLAMA_EMBED_TIMEOUT_MS", 30000);
        embedder_config.max_retries = ParseNonNegativeIntEnv("WAXCPP_LLAMA_EMBED_MAX_RETRIES", 2);
        embedder_config.retry_backoff_ms = ParseNonNegativeIntEnv("WAXCPP_LLAMA_EMBED_RETRY_BACKOFF_MS", 100);
        embedder_config.max_batch_concurrency =
            ParsePositiveIntEnv("WAXCPP_LLAMA_EMBED_MAX_BATCH_CONCURRENCY", 4);
        embedder = std::make_shared<LlamaCppEmbeddingProvider>(std::move(embedder_config));
    }
    LlamaCppGenerationConfig generation_config{};
    generation_config.endpoint = EnvString("WAXCPP_LLAMA_GEN_ENDPOINT").value_or(kDefaultLlamaGenEndpoint);
    generation_config.model_path = runtime_models_.generation_model.model_path;
    generation_config.timeout_ms = ParsePositiveIntEnv("WAXCPP_LLAMA_GEN_TIMEOUT_MS", 60000);
    generation_config.max_retries = ParseNonNegativeIntEnv("WAXCPP_LLAMA_GEN_MAX_RETRIES", 2);
    generation_config.retry_backoff_ms = ParseNonNegativeIntEnv("WAXCPP_LLAMA_GEN_RETRY_BACKOFF_MS", 100);
    if (generation_client_override) {
        generation_client_ = std::move(generation_client_override);
    } else {
        generation_client_ = std::make_unique<LlamaCppGenerationClient>(std::move(generation_config));
    }

    orchestrator_ = std::make_unique<waxcpp::MemoryOrchestrator>(store_path, config, std::move(embedder));
}

WaxRAGHandler::~WaxRAGHandler() {
    std::thread worker_to_join{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index_cancel_flag_) {
            index_cancel_flag_->store(true, std::memory_order_relaxed);
        }
        if (index_job_manager_.status().state == IndexJobState::kRunning) {
            (void)index_job_manager_.Stop();
        }
        if (index_worker_.joinable()) {
            worker_to_join = std::move(index_worker_);
        }
        index_cancel_flag_.reset();
    }
    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }
}

std::string WaxRAGHandler::handle_remember(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string content = (params.isNull() ? "" : params->optValue<std::string>("content", ""));
    if (content.empty()) {
        return "Missing required parameter 'content'";
    }

    waxcpp::Metadata metadata_map{};
    if (!params.isNull() && params->has("metadata")) {
        Poco::JSON::Object::Ptr metadata;
        try {
            metadata = params->getObject("metadata");
        } catch (const Poco::Exception&) {
            metadata = nullptr;
        }

        if (!metadata.isNull()) {
            for (const auto& [key, value] : *metadata) {
                try {
                    metadata_map[key] = value.convert<std::string>();
                } catch (const Poco::Exception&) {
                    // Ignore non-string metadata values for deterministic behavior.
                }
            }
        }
    }

    try {
        orchestrator_->Remember(content, metadata_map);
        return "OK";
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_recall(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string query = (params.isNull() ? "" : params->optValue<std::string>("query", ""));
    if (query.empty()) {
        return "Missing required parameter 'query'";
    }

    try {
        const auto context = orchestrator_->Recall(query);

        Poco::JSON::Array response;
        for (const auto& item : context.items) {
            Poco::JSON::Object::Ptr row = new Poco::JSON::Object();
            row->set("kind", static_cast<int>(item.kind));
            row->set("text", item.text);
            row->set("score", item.score);
            response.add(row);
        }

        std::ostringstream out;
        response.stringify(out);
        return out.str();
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_answer_generate(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string query = (params.isNull() ? "" : params->optValue<std::string>("query", ""));
    if (query.empty()) {
        return "Missing required parameter 'query'";
    }
    const int max_context_items = ParsePositiveIntParam(params, "max_context_items", 10);
    const int max_context_tokens = ParsePositiveIntParam(params, "max_context_tokens", 4000);
    const int max_output_tokens = ParsePositiveIntParam(params, "max_output_tokens", 768);
    const float temperature = ParseFloatParam(params, "temperature", 0.1F);
    const float top_p = ParseFloatParam(params, "top_p", 0.95F);

    try {
        const auto context = orchestrator_->Recall(query);
        const auto citations = BuildCitations(*orchestrator_, context, max_context_items);
        const auto prompt = BuildAnswerPrompt(query, context, citations, max_context_items, max_context_tokens);
        const auto answer = generation_client_->Generate(
            LlamaCppGenerationRequest{
                .prompt = prompt.prompt,
                .max_tokens = max_output_tokens,
                .temperature = temperature,
                .top_p = top_p,
            });

        Poco::JSON::Object response{};
        response.set("query", query);
        response.set("answer", answer);
        response.set("model", runtime_models_.generation_model.model_path);
        response.set("total_context_tokens", context.total_tokens);
        response.set("context_items_used", prompt.context_items_used);
        response.set("context_tokens_used", prompt.context_tokens_used);

        Poco::JSON::Array citations_json{};
        for (const auto& citation : citations) {
            Poco::JSON::Object citation_json{};
            citation_json.set("frame_id", citation.frame_id);
            citation_json.set("relative_path", citation.relative_path);
            citation_json.set("line_start", citation.line_start.value_or(0));
            citation_json.set("line_end", citation.line_end.value_or(0));
            citation_json.set("symbol", citation.symbol);
            citation_json.set("score", citation.score);
            citations_json.add(citation_json);
        }
        response.set("citations", citations_json);

        std::ostringstream out;
        response.stringify(out);
        return out.str();
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_flush(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)params;

    try {
        orchestrator_->Flush();
        return "OK";
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

void WaxRAGHandler::run_index_job(std::string repo_root,
                                  bool resume_requested,
                                  IndexRunOptions options,
                                  std::shared_ptr<std::atomic<bool>> cancel_flag) {
    auto is_cancelled = [&cancel_flag]() noexcept {
        return cancel_flag && cancel_flag->load(std::memory_order_relaxed);
    };

    try {
        {
            std::ostringstream msg;
            msg << "index job started repo_root=" << repo_root
                << " resume=" << (resume_requested ? "true" : "false")
                << " flush_every_chunks=" << options.flush_every_chunks
                << " max_files=" << options.max_files
                << " max_chunks=" << options.max_chunks;
            ServerLog(msg.str());
        }
        (void)index_job_manager_.SetPhase("scanning");
        const auto repo_root_path = std::filesystem::path(repo_root);
        auto entries = ue5_scanner_.Scan(repo_root_path, is_cancelled);
        if (options.max_files > 0 && entries.size() > options.max_files) {
            entries.resize(static_cast<std::size_t>(options.max_files));
        }
        if (is_cancelled()) {
            ServerLog("index job cancelled during scan");
            return;
        }
        {
            std::ostringstream msg;
            msg << "scan completed files=" << entries.size();
            ServerLog(msg.str());
        }

        const auto running_status = index_job_manager_.status();
        auto manifest_path = running_status.checkpoint_path;
        manifest_path += ".scan_manifest";
        auto chunk_manifest_path = running_status.checkpoint_path;
        chunk_manifest_path += ".chunk_manifest";
        auto file_manifest_path = running_status.checkpoint_path;
        file_manifest_path += ".file_manifest";

        std::vector<Ue5FileDigest> previous_file_digests{};
        bool loaded_previous_file_manifest = false;
        if (resume_requested) {
            std::error_code ec;
            if (std::filesystem::exists(file_manifest_path, ec) && !ec) {
                loaded_previous_file_manifest = true;
                previous_file_digests = Ue5ChunkManifestBuilder::ParseFileManifest(ReadFileText(file_manifest_path));
            }
        }

        std::vector<Ue5FileDigest> current_file_digests{};
        const auto chunk_records = ue5_chunk_builder_.Build(repo_root_path, entries, {}, &current_file_digests);
        const auto unchanged_paths =
            Ue5ChunkManifestBuilder::ComputeUnchangedPaths(previous_file_digests, current_file_digests);
        {
            std::ostringstream msg;
            msg << "chunk manifest prepared chunks=" << chunk_records.size()
                << " unchanged_files=" << unchanged_paths.size();
            ServerLog(msg.str());
        }
        const std::uint64_t resume_committed_watermark =
            (resume_requested && !loaded_previous_file_manifest) ? running_status.committed_chunks : 0;
        std::uint64_t remaining_resume_skip_chunks = resume_committed_watermark;
        if (resume_committed_watermark > 0) {
            std::ostringstream msg;
            msg << "resume committed watermark active committed_chunks=" << resume_committed_watermark;
            ServerLog(msg.str());
        }
        (void)index_job_manager_.SetPhase("ingesting");

        std::uint64_t indexed_chunks = 0;
        std::uint64_t committed_chunks = 0;
        bool reached_chunk_limit = false;
        (void)ue5_chunk_builder_.Build(
            repo_root_path,
            entries,
            [&](const Ue5ChunkRecord& chunk, std::string_view chunk_text) {
                if (reached_chunk_limit) {
                    return;
                }
                if (is_cancelled()) {
                    return;
                }
                if (resume_requested && unchanged_paths.contains(chunk.relative_path)) {
                    return;
                }
                if (remaining_resume_skip_chunks > 0) {
                    --remaining_resume_skip_chunks;
                    return;
                }

                waxcpp::Metadata metadata{};
                metadata["source_kind"] = "ue5_chunk";
                metadata["repo_root"] = repo_root;
                metadata["relative_path"] = chunk.relative_path;
                metadata["language"] = chunk.language;
                metadata["symbol"] = chunk.symbol;
                metadata["line_start"] = std::to_string(chunk.line_start);
                metadata["line_end"] = std::to_string(chunk.line_end);
                metadata["chunk_id"] = chunk.chunk_id;
                metadata["chunk_hash"] = chunk.content_hash;
                metadata["token_estimate"] = std::to_string(chunk.token_estimate);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    orchestrator_->Remember(std::string(chunk_text), metadata);
                }
                ++indexed_chunks;
                if (options.max_chunks > 0 && indexed_chunks >= options.max_chunks) {
                    reached_chunk_limit = true;
                }

                if (indexed_chunks % options.flush_every_chunks == 0) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        orchestrator_->Flush();
                    }
                    committed_chunks = indexed_chunks;
                    (void)index_job_manager_.UpdateProgress(static_cast<std::uint64_t>(entries.size()),
                                                            indexed_chunks,
                                                            committed_chunks);
                    std::ostringstream msg;
                    msg << "index progress indexed_chunks=" << indexed_chunks
                        << " committed_chunks=" << committed_chunks;
                    ServerLog(msg.str());
                }
            });
        if (reached_chunk_limit) {
            ServerLog("index job reached max_chunks cap");
        }
        if (is_cancelled()) {
            ServerLog("index job cancelled during ingest");
            return;
        }

        if (indexed_chunks > committed_chunks) {
            std::lock_guard<std::mutex> lock(mutex_);
            orchestrator_->Flush();
            committed_chunks = indexed_chunks;
        }
        (void)index_job_manager_.UpdateProgress(static_cast<std::uint64_t>(entries.size()),
                                                indexed_chunks,
                                                committed_chunks);
        if (is_cancelled()) {
            ServerLog("index job cancelled before manifest write");
            return;
        }

        (void)index_job_manager_.SetPhase("persisting_manifests");
        WriteFileText(manifest_path,
                      Ue5FilesystemScanner::SerializeManifest(entries),
                      "failed to open scan manifest file for write",
                      "failed to persist scan manifest file");
        WriteFileText(chunk_manifest_path,
                      Ue5ChunkManifestBuilder::SerializeManifest(chunk_records),
                      "failed to open chunk manifest file for write",
                      "failed to persist chunk manifest file");
        WriteFileText(file_manifest_path,
                      Ue5ChunkManifestBuilder::SerializeFileManifest(current_file_digests),
                      "failed to open file manifest file for write",
                      "failed to persist file manifest file");
        if (is_cancelled()) {
            ServerLog("index job cancelled after manifest write");
            return;
        }

        (void)index_job_manager_.Complete(static_cast<std::uint64_t>(entries.size()), indexed_chunks, committed_chunks);
        {
            std::ostringstream msg;
            msg << "index job completed scanned_files=" << entries.size()
                << " indexed_chunks=" << indexed_chunks
                << " committed_chunks=" << committed_chunks;
            ServerLog(msg.str());
        }
    } catch (const std::exception& e) {
        if (!is_cancelled()) {
            (void)index_job_manager_.Fail(e.what());
            std::ostringstream msg;
            msg << "index job failed: " << e.what();
            ServerLog(msg.str());
        }
    }
}

void WaxRAGHandler::reap_index_worker_if_finished_locked() {
    std::thread worker_to_join{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!index_worker_.joinable()) {
            if (index_cancel_flag_ && index_job_manager_.status().state != IndexJobState::kRunning) {
                index_cancel_flag_.reset();
            }
            return;
        }
        if (index_job_manager_.status().state == IndexJobState::kRunning) {
            return;
        }
        worker_to_join = std::move(index_worker_);
        index_cancel_flag_.reset();
    }
    if (worker_to_join.joinable()) {
        worker_to_join.join();
    }
}

std::string WaxRAGHandler::handle_index_start(const Poco::JSON::Object::Ptr& params) {
    const std::string repo_root = (params.isNull() ? "" : params->optValue<std::string>("repo_root", ""));
    if (repo_root.empty()) {
        return "Missing required parameter 'repo_root'";
    }
    const bool resume_requested = (params.isNull() ? false : params->optValue<bool>("resume", false));
    const int flush_every_chunks_param =
        ParsePositiveIntParam(params, "flush_every_chunks", static_cast<int>(kIndexFlushEveryChunks));
    const int max_files_param = ParseNonNegativeIntParam(params, "max_files", 0);
    const int max_chunks_param = ParseNonNegativeIntParam(params, "max_chunks", 0);
    if (flush_every_chunks_param <= 0 ||
        static_cast<std::uint64_t>(flush_every_chunks_param) > kMaxIndexControlValue) {
        return "Error: flush_every_chunks must be within [1, 1000000]";
    }
    if (max_files_param < 0 || static_cast<std::uint64_t>(max_files_param) > kMaxIndexControlValue) {
        return "Error: max_files must be within [0, 1000000]";
    }
    if (max_chunks_param < 0 || static_cast<std::uint64_t>(max_chunks_param) > kMaxIndexControlValue) {
        return "Error: max_chunks must be within [0, 1000000]";
    }
    const IndexRunOptions options{
        .flush_every_chunks = static_cast<std::uint64_t>(flush_every_chunks_param),
        .max_files = static_cast<std::uint64_t>(max_files_param),
        .max_chunks = static_cast<std::uint64_t>(max_chunks_param),
    };

    try {
        reap_index_worker_if_finished_locked();

        std::shared_ptr<std::atomic<bool>> cancel_flag = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const bool started = index_job_manager_.Start(std::filesystem::path(repo_root), resume_requested);
            if (!started) {
                return "Error: index job is already running";
            }
            index_cancel_flag_ = cancel_flag;
            try {
                index_worker_ =
                    std::thread(&WaxRAGHandler::run_index_job, this, repo_root, resume_requested, options, cancel_flag);
            } catch (const std::exception& e) {
                index_cancel_flag_.reset();
                (void)index_job_manager_.Fail(e.what());
                return std::string("Error: failed to start index worker: ") + e.what();
            }
        }
        ServerLog("index.start accepted");
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_index_status(const Poco::JSON::Object::Ptr& params) {
    (void)params;
    try {
        reap_index_worker_if_finished_locked();
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_index_stop(const Poco::JSON::Object::Ptr& params) {
    (void)params;
    try {
        std::thread worker_to_join{};
        bool had_running_worker = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            had_running_worker = index_worker_.joinable();
            if (index_cancel_flag_) {
                index_cancel_flag_->store(true, std::memory_order_relaxed);
            }
            const bool stopped = index_job_manager_.Stop();
            if (!stopped && !had_running_worker) {
                return "Error: index job is not running";
            }
            if (index_worker_.joinable()) {
                worker_to_join = std::move(index_worker_);
            }
            index_cancel_flag_.reset();
        }
        if (worker_to_join.joinable()) {
            worker_to_join.join();
        }
        ServerLog("index.stop completed");
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::make_index_status_json(const IndexJobStatus& status) const {
    Poco::JSON::Object response{};
    response.set("state", ToString(status.state));
    response.set("phase", status.phase);
    response.set("generation", status.generation);
    response.set("job_id", status.job_id.value_or(""));
    response.set("repo_root", status.repo_root.value_or(""));
    response.set("checkpoint_path", status.checkpoint_path.string());
    response.set("started_at_ms", status.started_at_ms);
    response.set("updated_at_ms", status.updated_at_ms);
    response.set("scanned_files", status.scanned_files);
    response.set("indexed_chunks", status.indexed_chunks);
    response.set("committed_chunks", status.committed_chunks);
    response.set("resume_requested", status.resume_requested);
    response.set("last_error", status.last_error.value_or(""));
    const std::uint64_t now_ms = NowMs();
    const std::uint64_t elapsed_ms = (status.started_at_ms > 0 && now_ms >= status.started_at_ms)
                                         ? (now_ms - status.started_at_ms)
                                         : 0;
    response.set("elapsed_ms", elapsed_ms);
    if (elapsed_ms > 0) {
        const double seconds = static_cast<double>(elapsed_ms) / 1000.0;
        response.set("indexed_chunks_per_sec", static_cast<double>(status.indexed_chunks) / seconds);
        response.set("committed_chunks_per_sec", static_cast<double>(status.committed_chunks) / seconds);
    } else {
        response.set("indexed_chunks_per_sec", 0.0);
        response.set("committed_chunks_per_sec", 0.0);
    }

    std::ostringstream out;
    response.stringify(out);
    return out.str();
}

} // namespace waxcpp::server
