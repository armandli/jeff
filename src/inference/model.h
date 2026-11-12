#pragma once

#include <string>
#include <vector>
#include <torch/script.h>

namespace jeff {

struct DecisionInput {
    std::string context;
    std::vector<std::string> options;
};

struct DecisionOutput {
    int chosen_index;
    std::vector<float> scores;
};

class DecisionModel {
public:
    explicit DecisionModel(const std::string& model_path);

    DecisionOutput decide(const DecisionInput& input) const;

private:
    torch::jit::script::Module module_;
};

} // namespace jeff
