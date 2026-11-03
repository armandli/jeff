#include "server.hpp"
#include <iostream>
#include <string>

namespace jeff::mcp {

Server::Server(std::shared_ptr<DecisionModel> model) : model_(std::move(model)) {}

void Server::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        std::string response = handle_request(line);
        std::cout << response << "\n";
        std::cout.flush();
    }
}

std::string Server::handle_request(const std::string& /*json_request*/) {
    // TODO: parse JSON-RPC 2.0, dispatch to model_.decide(), serialize response
    return R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"not implemented"},"id":null})";
}

} // namespace jeff::mcp
