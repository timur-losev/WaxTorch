// cpp/server/main.cpp
#include "wax_rag_handler.hpp"
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
#include <iostream>

using namespace Poco::Net;
using namespace Poco::Util;

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
            } else if (json_request.method == "flush") {
                result = handler_.handle_flush(json_request.params);
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

        // Инициализация WAX
        logger.information("Initializing WAX orchestrator...");
        waxcpp::server::WaxRAGHandler handler;
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
