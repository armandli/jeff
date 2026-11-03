#include "model.hpp"
#include "tokenizer.hpp"
#include <stdexcept>

namespace jeff {

DecisionModel::DecisionModel(const std::string& model_path) {
    try {
        module_ = torch::jit::load(model_path);
        module_.eval();
    } catch (const c10::Error& e) {
        throw std::runtime_error("Failed to load model from " + model_path + ": " + e.what());
    }
}

DecisionOutput DecisionModel::decide(const DecisionInput& input) const {
    // TODO: tokenize input and options, run forward pass with constrained decoding
    // Stub: always returns option 0 with uniform scores
    DecisionOutput out;
    out.chosen_index = 0;
    out.scores.assign(input.options.size(), 1.0f / static_cast<float>(input.options.size()));
    return out;
}

} // namespace jeff
