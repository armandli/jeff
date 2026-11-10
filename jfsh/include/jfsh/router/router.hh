#pragma once
// jfsh/router/router.hh — System 1 decision router.
//
// The router intercepts every command line before execution and classifies it:
//   RUN   → execute normally as a shell command
//   AGENT → forward to the configured LLM agent
//
// The router uses a fast encoder model (ModernBERT or ELECTRA fine-tuned on
// exp_006_zsh_router data) for sub-millisecond latency decisions.
// When JFSH_USE_TORCH is not set, the router falls back to a heuristic
// (unparseable lines → AGENT, everything else → RUN).

#include <jfsh/core/types.hh>
#include <jfsh/core/options.hh>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace jfsh {

// ---------------------------------------------------------------------------
// RouterDecision — the router's verdict for a command line
// ---------------------------------------------------------------------------
enum class RouterVerdict : std::uint8_t {
    Run,    // treat as a shell command
    Agent,  // forward to LLM
};

struct RouterDecision {
    RouterVerdict verdict    = RouterVerdict::Run;
    float         confidence = 1.0f;  // [0, 1] — higher = more certain
    std::string   model_name;         // which model produced this

    bool is_agent() const noexcept { return verdict == RouterVerdict::Agent; }
    bool is_run()   const noexcept { return verdict == RouterVerdict::Run;   }
};

// ---------------------------------------------------------------------------
// RouterModel — abstract interface over inference backends
// ---------------------------------------------------------------------------
struct RouterModel {
    virtual ~RouterModel() = default;
    virtual RouterDecision classify(std::string_view line) = 0;
    virtual std::string_view name() const noexcept = 0;
};

// ---------------------------------------------------------------------------
// HeuristicRouter — no-torch fallback: tries zsh -n, returns RUN if parses
// ---------------------------------------------------------------------------
class HeuristicRouter : public RouterModel {
public:
    RouterDecision classify(std::string_view line) override;
    std::string_view name() const noexcept override { return "heuristic"; }
};

#ifdef JFSH_USE_TORCH
// ---------------------------------------------------------------------------
// TorchRouter — uses a libtorch encoder model for inference
// ---------------------------------------------------------------------------
class TorchRouter : public RouterModel {
public:
    // Load the best checkpoint from `model_dir`.
    static std::unique_ptr<TorchRouter> load(std::string model_dir);

    RouterDecision classify(std::string_view line) override;
    std::string_view name() const noexcept override { return name_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string           name_;

    explicit TorchRouter(std::unique_ptr<Impl> impl, std::string name);
};
#endif

// ---------------------------------------------------------------------------
// Router — the shell's router subsystem
// ---------------------------------------------------------------------------
class Router {
public:
    Router();

    // Classify a raw command line.
    RouterDecision classify(std::string_view line);

    // Bypass the model: always RUN.  Set by --norouter flag.
    void set_bypass(bool on) noexcept { bypass_ = on; }
    bool is_bypass()   const noexcept { return bypass_; }

    // Load a torch model from a checkpoint directory.
    // Falls back to heuristic if loading fails or torch is unavailable.
    void load_model(std::string checkpoint_dir);

    // Confidence threshold below which to default to RUN (conservative).
    void set_threshold(float t) noexcept { threshold_ = t; }

private:
    std::unique_ptr<RouterModel> model_;
    bool  bypass_    = false;
    float threshold_ = 0.5f;
};

} // namespace jfsh
