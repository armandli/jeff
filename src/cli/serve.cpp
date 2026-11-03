#include "../inference/model.hpp"
#include "../mcp/server.hpp"
#include <iostream>
#include <memory>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: jeff-serve <model_path>\n";
        return 1;
    }

    const std::string model_path = argv[1];

    auto model = std::make_shared<jeff::DecisionModel>(model_path);
    jeff::mcp::Server server(model);
    server.run();
    return 0;
}
