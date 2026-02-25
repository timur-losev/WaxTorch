// cpp/server/wax_rag_handler.hpp
#pragma once

#include "../include/waxcpp/memory_orchestrator.hpp"
#include "../include/waxcpp/runtime_model_config.hpp"
#include "index_job_manager.hpp"
#include "json_rpc.hpp"
#include "llama_cpp_generation_client.hpp"
#include "llama_cpp_embedding_provider.hpp"
#include "ue5_chunk_manifest.hpp"
#include "ue5_filesystem_scanner.hpp"

#include <Poco/JSON/Object.h>

#include <filesystem>
#include <memory>
#include <mutex>

namespace waxcpp::server {

class WaxRAGHandler {
public:
    explicit WaxRAGHandler(
        const std::filesystem::path& store_path = "wax-server.mv2s",
        waxcpp::RuntimeModelsConfig runtime_models = {});
    
    // Обработчики JSON-RPC методов
    std::string handle_remember(const Poco::JSON::Object::Ptr& params);
    std::string handle_recall(const Poco::JSON::Object::Ptr& params);
    std::string handle_answer_generate(const Poco::JSON::Object::Ptr& params);
    std::string handle_flush(const Poco::JSON::Object::Ptr& params);
    std::string handle_index_start(const Poco::JSON::Object::Ptr& params);
    std::string handle_index_status(const Poco::JSON::Object::Ptr& params);
    std::string handle_index_stop(const Poco::JSON::Object::Ptr& params);

private:
    std::string make_index_status_json(const IndexJobStatus& status) const;

    std::unique_ptr<waxcpp::MemoryOrchestrator> orchestrator_;
    std::unique_ptr<LlamaCppGenerationClient> generation_client_;
    IndexJobManager index_job_manager_;
    Ue5FilesystemScanner ue5_scanner_{};
    Ue5ChunkManifestBuilder ue5_chunk_builder_{};
    waxcpp::RuntimeModelsConfig runtime_models_{};
    std::mutex mutex_;
};

} // namespace waxcpp::server
