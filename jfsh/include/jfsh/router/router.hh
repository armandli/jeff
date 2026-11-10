#ifndef JFSH_ROUTER_ROUTER_HH
#define JFSH_ROUTER_ROUTER_HH
// jfsh/router/router.hh — System 1 decision router.
//
// The router intercepts every command line before execution and classifies it:
//   RUN   → execute normally as a shell command
//   AGENT → forward to the configured LLM agent
//
// When JFSH_USE_TORCH is not set, the router falls back to a heuristic
// (unparseable lines → AGENT, everything else → RUN).

#include <cstdint>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <jfsh/core/options.hh>
#include <jfsh/core/types.hh>

namespace jfsh {

enum class RouterVerdict : std::uint8_t {
  Run,
  Agent,
};

struct RouterDecision {
  RouterVerdict verdict    = RouterVerdict::Run;
  float         confidence = 1.0f;
  std::string   model_name;

  bool is_agent() const noexcept { return verdict == RouterVerdict::Agent; }
  bool is_run()   const noexcept { return verdict == RouterVerdict::Run; }
};

struct RouterModel {
  virtual ~RouterModel() = default;
  virtual RouterDecision  classify(std::string_view line) = 0;
  virtual std::string_view name() const noexcept = 0;
};

struct HeuristicRouter : RouterModel {
  RouterDecision  classify(std::string_view line) override;
  std::string_view name() const noexcept override { return "heuristic"; }
};

// Configuration for the JEV cloud router (api.typesafe.ai).
struct JevConfig {
  std::string   api_key;
  std::string   model      = "jev-latest";
  std::string   base_url   = "https://api.typesafe.ai";
  std::uint32_t timeout_ms = 10000;

  static JevConfig from_env();  // reads TYPESAFE_API_KEY
};

#ifdef JFSH_USE_TORCH
struct TorchRouter : RouterModel {
  static std::unique_ptr<TorchRouter> load(std::string model_dir);

  RouterDecision  classify(std::string_view line) override;
  std::string_view name() const noexcept override { return mName; }

protected:
  struct Impl;
  explicit TorchRouter(std::unique_ptr<Impl> impl, std::string name);

private:
  std::unique_ptr<Impl> mImpl;
  std::string           mName;
};
#endif

struct Router {
  Router();

  RouterDecision classify(std::string_view line);

  void set_bypass(bool on)  noexcept { mBypass = on; }
  bool is_bypass()    const noexcept { return mBypass; }

  void load_model(std::string checkpoint_dir);
  void load_jev(JevConfig cfg);

  void set_threshold(float t) noexcept { mThreshold = t; }

private:
  std::unique_ptr<RouterModel> mModel;
  bool  mBypass    = false;
  float mThreshold = 0.5f;
};

}  // namespace jfsh

#endif  // JFSH_ROUTER_ROUTER_HH
