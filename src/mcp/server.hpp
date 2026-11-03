#pragma once

#include "../inference/model.hpp"
#include <memory>
#include <string>

namespace jeff::mcp {

// JSON-RPC 2.0 over stdio — MCP protocol transport.
// Reads newline-delimited JSON from stdin, writes responses to stdout.
class Server {
public:
    explicit Server(std::shared_ptr<DecisionModel> model);

    // Blocks until stdin closes or a shutdown request is received.
    void run();

private:
    std::string handle_request(const std::string& json_request);

    std::shared_ptr<DecisionModel> model_;
};

} // namespace jeff::mcp
