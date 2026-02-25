// cpp/server/main.cpp
#include "wax_rag_handler.hpp"
#include "runtime_config.hpp"
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Util/ServerApplication.h>
#include <Poco/Logger.h>
#include <Poco/AutoPtr.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/PatternFormatter.h>
#include <iterator>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace Poco::Net;
using namespace Poco::Util;

namespace {

std::string EnvOrDefault(const char* name, const char* fallback) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return std::string(fallback);
    }
    std::string out(value);
    std::free(value);
    if (out.empty()) {
        return std::string(fallback);
    }
    return out;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::string(fallback);
    }
    return std::string(value);
#endif
}

}  // namespace

class RAGRequestHandler : public HTTPRequestHandler {
public:
    RAGRequestHandler(waxcpp::server::WaxRAGHandler& handler) 
        : handler_(handler) {}

    void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response) override {
        try {
            std::string body;
            std::istream& is = request.stream();
            std::copy(std::istreambuf_iterator<char>(is), 
                     std::istreambuf_iterator<char>(), 
                     std::back_inserter(body));

            // Парсим JSON-RPC
            auto json_request = waxcpp::server::parse_json_rpc(body);
            
            std::string result;
            if (json_request.method == "remember") {
                result = handler_.handle_remember(json_request.params);
            } else if (json_request.method == "recall") {
                result = handler_.handle_recall(json_request.params);
            } else if (json_request.method == "answer.generate") {
                result = handler_.handle_answer_generate(json_request.params);
            } else if (json_request.method == "flush") {
                result = handler_.handle_flush(json_request.params);
            } else if (json_request.method == "index.start") {
                result = handler_.handle_index_start(json_request.params);
            } else if (json_request.method == "index.status") {
                result = handler_.handle_index_status(json_request.params);
            } else if (json_request.method == "index.stop") {
                result = handler_.handle_index_stop(json_request.params);
            } else {
                result = "Unknown method: " + json_request.method;
            }

            // Отправляем JSON-RPC ответ
            response.setContentType("application/json");
            response.setStatus(HTTPResponse::HTTP_OK);
            response.send() << result;

        } catch (const std::exception& e) {
            response.setStatus(HTTPResponse::HTTP_INTERNAL_SERVER_ERROR);
            response.send() << "{\"error\": \"" << e.what() << "\"}";
        }
    }

private:
    waxcpp::server::WaxRAGHandler& handler_;
};

class RAGRequestHandlerFactory : public HTTPRequestHandlerFactory {
public:
    RAGRequestHandlerFactory(waxcpp::server::WaxRAGHandler& handler) 
        : handler_(handler) {}

    HTTPRequestHandler* createRequestHandler(const HTTPServerRequest&) override {
        return new RAGRequestHandler(handler_);
    }

private:
    waxcpp::server::WaxRAGHandler& handler_;
};

class RAGServer : public ServerApplication {
protected:
    int main(const std::vector<std::string>& args) override {
        (void)args;
        // Настройка логирования
        auto& logger = Poco::Logger::get("WaxRAGServer");
        Poco::AutoPtr<Poco::ConsoleChannel> channel = new Poco::ConsoleChannel;
        Poco::AutoPtr<Poco::PatternFormatter> formatter = new Poco::PatternFormatter("%Y-%m-%d %H:%M:%S [%p] %t");
        Poco::AutoPtr<Poco::FormattingChannel> logChannel = new Poco::FormattingChannel(formatter, channel);
        logger.setChannel(logChannel);
        logger.setLevel("information");

        // Параметры сервера
        unsigned short port = static_cast<unsigned short>(config().getUInt("port", 8080));
        int maxQueue = config().getInt("maxQueue", 64);
        int maxThreads = config().getInt("maxThreads", 8);

        // Создание сокета и сервера
        ServerSocket socket(port);
        Poco::Net::HTTPServerParams::Ptr params = new Poco::Net::HTTPServerParams;
        params->setMaxQueued(maxQueue);
        params->setMaxThreads(maxThreads);

        auto runtime_config_path = waxcpp::server::ResolveServerRuntimeConfigPathFromEnv();
        auto runtime_config = waxcpp::server::LoadServerRuntimeConfig(runtime_config_path);
        logger.information("Generation runtime: " + runtime_config.models.generation_model.runtime);
        logger.information("Generation model: " + runtime_config.models.generation_model.model_path);
        logger.information("llama.cpp generation endpoint: " +
                           EnvOrDefault("WAXCPP_LLAMA_GEN_ENDPOINT",
                                        "http://127.0.0.1:8081/completion (default)"));
        logger.information("llama.cpp generation timeout ms: " +
                           EnvOrDefault("WAXCPP_LLAMA_GEN_TIMEOUT_MS", "60000 (default)"));
        logger.information("llama.cpp generation max retries: " +
                           EnvOrDefault("WAXCPP_LLAMA_GEN_MAX_RETRIES", "2 (default)"));
        logger.information("llama.cpp generation retry backoff ms: " +
                           EnvOrDefault("WAXCPP_LLAMA_GEN_RETRY_BACKOFF_MS", "100 (default)"));
        logger.information("Embedding runtime: " + runtime_config.models.embedding_model.runtime);
        logger.information("Embedding model: " +
                           (runtime_config.models.embedding_model.model_path.empty()
                                ? std::string("(disabled)")
                                : runtime_config.models.embedding_model.model_path));
        logger.information("llama.cpp root: " +
                           (runtime_config.models.llama_cpp_root.empty()
                                ? std::string("(not set)")
                                : runtime_config.models.llama_cpp_root));
        logger.information("Vector search enabled: " +
                           std::string(runtime_config.models.enable_vector_search ? "true" : "false"));
        if (runtime_config.models.enable_vector_search) {
            logger.information("llama.cpp embedding endpoint: " +
                               EnvOrDefault("WAXCPP_LLAMA_EMBED_ENDPOINT",
                                            "http://127.0.0.1:8081/embedding (default)"));
            logger.information("llama.cpp embedding dimensions: " +
                               EnvOrDefault("WAXCPP_LLAMA_EMBED_DIMS", "1024 (default)"));
            logger.information("llama.cpp embedding max retries: " +
                               EnvOrDefault("WAXCPP_LLAMA_EMBED_MAX_RETRIES", "2 (default)"));
            logger.information("llama.cpp embedding retry backoff ms: " +
                               EnvOrDefault("WAXCPP_LLAMA_EMBED_RETRY_BACKOFF_MS", "100 (default)"));
            logger.information("llama.cpp embedding batch concurrency: " +
                               EnvOrDefault("WAXCPP_LLAMA_EMBED_MAX_BATCH_CONCURRENCY", "4 (default)"));
        }
        if (runtime_config_path.has_value()) {
            logger.information("Runtime config file: " + runtime_config_path->string());
        }

        // Инициализация WAX
        logger.information("Initializing WAX orchestrator...");
        waxcpp::server::WaxRAGHandler handler("wax-server.mv2s", runtime_config.models);
        logger.information("WAX orchestrator initialized");

        // Запуск сервера
        HTTPServer server(new RAGRequestHandlerFactory(handler), socket, params);
        server.start();

        logger.information("WAX RAG server started on port %hu", port);
        waitForTerminationRequest();

        logger.information("Shutting down WAX server...");
        server.stop();
        return Application::EXIT_OK;
    }
};

int main(int argc, char** argv) {
    RAGServer app;
    return app.run(argc, argv);
}
