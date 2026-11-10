#include <jfsh/router/router.hh>
#include <cstdio>
#include <cstring>

namespace jfsh {

// ---------------------------------------------------------------------------
// HeuristicRouter
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------

Router::Router() : model_(std::make_unique<HeuristicRouter>()) {}

RouterDecision Router::classify(std::string_view line) {
    if (bypass_ || line.empty()) return {RouterVerdict::Run, 1.0f, "bypass"};
    auto decision = model_->classify(line);
    if (decision.confidence < threshold_) return {RouterVerdict::Run, decision.confidence, decision.model_name};
    return decision;
}

void Router::load_model(std::string /*checkpoint_dir*/) {
#ifdef JFSH_USE_TORCH
    // TorchRouter::load(checkpoint_dir) — TBD
#endif
    // Fall through to heuristic
}

} // namespace jfsh
