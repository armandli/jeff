#include "../inference/model.hpp"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: jeff <model_path> [--context <ctx>] [--option <opt>]...\n";
        return 1;
    }

    const std::string model_path = argv[1];

    jeff::DecisionModel model(model_path);

    // TODO: parse --context and --option flags, call model.decide(), print result
    jeff::DecisionInput input;
    input.context = "placeholder context";
    input.options = {"option A", "option B"};

    auto output = model.decide(input);
    std::cout << "chosen: " << output.chosen_index
              << " (" << input.options[output.chosen_index] << ")\n";
    return 0;
}
