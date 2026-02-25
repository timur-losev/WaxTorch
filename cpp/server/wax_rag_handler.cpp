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
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace waxcpp::server {

namespace {
constexpr std::uint64_t kIndexFlushEveryChunks = 128;
constexpr const char* kDefaultLlamaEmbedEndpoint = "http://127.0.0.1:8081/embedding";

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
}

WaxRAGHandler::WaxRAGHandler(const std::filesystem::path& store_path,
                             waxcpp::RuntimeModelsConfig runtime_models)
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

    orchestrator_ = std::make_unique<waxcpp::MemoryOrchestrator>(store_path, config, std::move(embedder));
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

std::string WaxRAGHandler::handle_index_start(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    const std::string repo_root = (params.isNull() ? "" : params->optValue<std::string>("repo_root", ""));
    if (repo_root.empty()) {
        return "Missing required parameter 'repo_root'";
    }
    const bool resume_requested = (params.isNull() ? false : params->optValue<bool>("resume", false));

    try {
        const bool started = index_job_manager_.Start(std::filesystem::path(repo_root), resume_requested);
        if (!started) {
            return "Error: index job is already running";
        }
        const auto repo_root_path = std::filesystem::path(repo_root);
        const auto entries = ue5_scanner_.Scan(repo_root_path);
        const auto running_status = index_job_manager_.status();
        auto manifest_path = running_status.checkpoint_path;
        manifest_path += ".scan_manifest";
        auto chunk_manifest_path = running_status.checkpoint_path;
        chunk_manifest_path += ".chunk_manifest";
        auto file_manifest_path = running_status.checkpoint_path;
        file_manifest_path += ".file_manifest";

        std::vector<Ue5FileDigest> previous_file_digests{};
        if (resume_requested) {
            std::error_code ec;
            if (std::filesystem::exists(file_manifest_path, ec) && !ec) {
                previous_file_digests = Ue5ChunkManifestBuilder::ParseFileManifest(ReadFileText(file_manifest_path));
            }
        }

        std::vector<Ue5FileDigest> current_file_digests{};
        const auto chunk_records = ue5_chunk_builder_.Build(repo_root_path, entries, {}, &current_file_digests);
        const auto unchanged_paths = Ue5ChunkManifestBuilder::ComputeUnchangedPaths(
            previous_file_digests,
            current_file_digests);
        std::uint64_t indexed_chunks = 0;
        std::uint64_t committed_chunks = 0;
        (void)ue5_chunk_builder_.Build(
            repo_root_path,
            entries,
            [&](const Ue5ChunkRecord& chunk, std::string_view chunk_text) {
                if (resume_requested && unchanged_paths.contains(chunk.relative_path)) {
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
                orchestrator_->Remember(std::string(chunk_text), metadata);
                ++indexed_chunks;
                if (indexed_chunks % kIndexFlushEveryChunks == 0) {
                    orchestrator_->Flush();
                    committed_chunks = indexed_chunks;
                    const bool progress_updated = index_job_manager_.UpdateProgress(
                        static_cast<std::uint64_t>(entries.size()),
                        indexed_chunks,
                        committed_chunks);
                    (void)progress_updated;
                }
            });
        if (indexed_chunks > committed_chunks) {
            orchestrator_->Flush();
            committed_chunks = indexed_chunks;
        }
        {
            const bool progress_updated = index_job_manager_.UpdateProgress(
                static_cast<std::uint64_t>(entries.size()),
                indexed_chunks,
                committed_chunks);
            (void)progress_updated;
        }
        {
            std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                const bool marked_failed = index_job_manager_.Fail("failed to write scan manifest file");
                (void)marked_failed;
                return "Error: failed to open scan manifest file for write";
            }
            out << Ue5FilesystemScanner::SerializeManifest(entries);
            if (!out) {
                const bool marked_failed = index_job_manager_.Fail("failed to persist scan manifest file");
                (void)marked_failed;
                return "Error: failed to persist scan manifest file";
            }
        }
        {
            std::ofstream out(chunk_manifest_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                const bool marked_failed = index_job_manager_.Fail("failed to write chunk manifest file");
                (void)marked_failed;
                return "Error: failed to open chunk manifest file for write";
            }
            out << Ue5ChunkManifestBuilder::SerializeManifest(chunk_records);
            if (!out) {
                const bool marked_failed = index_job_manager_.Fail("failed to persist chunk manifest file");
                (void)marked_failed;
                return "Error: failed to persist chunk manifest file";
            }
        }
        {
            std::ofstream out(file_manifest_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                const bool marked_failed = index_job_manager_.Fail("failed to write file manifest file");
                (void)marked_failed;
                return "Error: failed to open file manifest file for write";
            }
            out << Ue5ChunkManifestBuilder::SerializeFileManifest(current_file_digests);
            if (!out) {
                const bool marked_failed = index_job_manager_.Fail("failed to persist file manifest file");
                (void)marked_failed;
                return "Error: failed to persist file manifest file";
            }
        }
        if (!index_job_manager_.Complete(static_cast<std::uint64_t>(entries.size()),
                                         indexed_chunks,
                                         committed_chunks)) {
            return "Error: failed to complete index job state transition";
        }
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        (void)index_job_manager_.Fail(e.what());
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_index_status(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)params;
    try {
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::handle_index_stop(const Poco::JSON::Object::Ptr& params) {
    std::lock_guard<std::mutex> lock(mutex_);
    (void)params;
    try {
        const bool stopped = index_job_manager_.Stop();
        if (!stopped) {
            return "Error: index job is not running";
        }
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
        return std::string("Error: ") + e.what();
    }
}

std::string WaxRAGHandler::make_index_status_json(const IndexJobStatus& status) const {
    Poco::JSON::Object response{};
    response.set("state", ToString(status.state));
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

    std::ostringstream out;
    response.stringify(out);
    return out.str();
}

} // namespace waxcpp::server
