#include <cstdio>
#include <cstring>

#include <jfsh/router/jev.hh>
#include <jfsh/router/router.hh>

namespace jfsh {

RouterDecision HeuristicRouter::classify(std::string_view line) {
  if (line.empty()) return {RouterVerdict::Run, 1.0f, "heuristic"};

  // Try `zsh -n` syntax check
  std::string cmd = "zsh -n -c ";
  // Simple single-quote escape
  cmd += '\'';
  for (char c : line) {
    if (c == '\'') cmd += "'\\''";
    else           cmd += c;
  }
  cmd += "' 2>/dev/null";

  int rc = std::system(cmd.c_str());
  if (rc == 0) return {RouterVerdict::Run,   0.9f, "heuristic"};
  return          {RouterVerdict::Agent, 0.9f, "heuristic"};
}

Router::Router() : mModel(std::make_unique<HeuristicRouter>()) {}

RouterDecision Router::classify(std::string_view line) {
  if (mBypass or line.empty())
    return {RouterVerdict::Run, 1.0f, "bypass"};
  auto decision = mModel->classify(line);
  if (decision.confidence < mThreshold)
    return {RouterVerdict::Run, decision.confidence, decision.model_name};
  return decision;
}

void Router::load_jev(JevConfig cfg) {
  mModel = std::make_unique<JevRouter>(std::move(cfg));
}

void Router::load_model(std::string checkpoint_dir) {
#ifdef JFSH_USE_TORCH
  // TODO(router/torch): Implement TorchRouter::load(checkpoint_dir).
  //   Steps:
  //   1. Load tokenizer vocab from checkpoint_dir/tokenizer.json (or use
  //      a FastTokenizer from HuggingFace tokenizers C++ port).
  //   2. torch::load(model, checkpoint_dir + "/decision_head.pt")
  //   3. Load ModernBERT/ELECTRA encoder via torch::jit::load() if the
  //      checkpoint contains a TorchScript-traced encoder.
  //   4. Set model_->eval() and move to CPU (or CUDA if available).
  //   5. The classify() method should:
  //      a. Build the System 1 context string (same as make_context in infer.py).
  //      b. Tokenize to input_ids / attention_mask tensors.
  //      c. Run the encoder forward pass.
  //      d. Apply the linear decision head.
  //      e. Softmax → return RouterDecision{verdict, confidence, model_name}.
  //   See python/experiments/exp_006_zsh_router/infer.py for the reference impl.
  (void)checkpoint_dir;
#else
  (void)checkpoint_dir;
#endif
  // Falls back to heuristic (zsh -n) if torch is unavailable or load fails.
}

}  // namespace jfsh
