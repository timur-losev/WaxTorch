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
    : runtime_models_(std::move(runtime_models)) {
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

} // namespace waxcpp::server
