// cpp/server/wax_rag_handler.hpp
#pragma once

#include "../include/waxcpp/memory_orchestrator.hpp"
#include "json_rpc.hpp"

#include <Poco/JSON/Object.h>

#include <filesystem>
#include <memory>
#include <mutex>

namespace waxcpp::server {

class WaxRAGHandler {
public:
    explicit WaxRAGHandler(const std::filesystem::path& store_path = "wax-server.mv2s");
    
    // Обработчики JSON-RPC методов
    std::string handle_remember(const Poco::JSON::Object::Ptr& params);
    std::string handle_recall(const Poco::JSON::Object::Ptr& params);
    std::string handle_flush(const Poco::JSON::Object::Ptr& params);

private:
    std::unique_ptr<waxcpp::MemoryOrchestrator> orchestrator_;
    std::mutex mutex_;
};

} // namespace waxcpp::server
