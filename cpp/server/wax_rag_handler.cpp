// cpp/server/wax_rag_handler.cpp
#include "wax_rag_handler.hpp"
#include "runtime_config.hpp"

#include <Poco/Exception.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>

#include <sstream>
#include <stdexcept>
#include <utility>

namespace waxcpp::server {

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

    if (runtime_models_.enable_vector_search) {
        throw std::runtime_error(
            "vector search requested, but llama.cpp embedding provider wiring is not enabled yet");
    }

    orchestrator_ = std::make_unique<waxcpp::MemoryOrchestrator>(store_path, config, nullptr);
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
        return make_index_status_json(index_job_manager_.status());
    } catch (const std::exception& e) {
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
