#ifndef JFSH_ROUTER_ROUTER_H
#define JFSH_ROUTER_ROUTER_H
// jfsh/router/router.h — System 1 decision router.
//
// The router intercepts every command line before execution and classifies it:
//   RUN   → execute normally as a shell command
//   AGENT → forward to the configured LLM agent
//
// Three backends, selected at startup:
//   HeuristicRouter — zsh -n syntax check (default, no service required)
//   JeffRouter      — local jeff HTTP service (127.0.0.1:8765)
//   JevRouter       — TypeSafe JEV cloud API (api.typesafe.ai)

#include <cstdint>

#include <memory>
#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

enum class RouterVerdict : s::uint8_t {
  Run,
  Agent,
};

struct RouterDecision {
  RouterVerdict verdict    = RouterVerdict::Run;
  float         confidence = 1.0f;
  str           model_name;

  bool is_agent() const noexcept { return verdict == RouterVerdict::Agent; }
  bool is_run()   const noexcept { return verdict == RouterVerdict::Run; }
};

struct RouterModel {
  virtual ~RouterModel() = default;
  virtual RouterDecision   classify(strv line) = 0;
  virtual strv             name() const noexcept = 0;
};

struct HeuristicRouter : RouterModel {
  RouterDecision classify(strv line) override;
  strv           name() const noexcept override { return "heuristic"; }
};

// Configuration for the local jeff HTTP service (jeff-server binary).
struct JeffConfig {
  str         base_url   = "http://127.0.0.1:8765";
  s::uint32_t timeout_ms = 1000;  // local — fast timeout
};

// Configuration for the JEV cloud router (api.typesafe.ai).
struct JevConfig {
  str         api_key;
  str         model      = "jev-latest";
  str         base_url   = "https://api.typesafe.ai";
  s::uint32_t timeout_ms = 10000;

  static JevConfig from_env();  // reads TYPESAFE_API_KEY
};

struct Router {
  Router() : mModel(s::make_unique<HeuristicRouter>()) {}

  RouterDecision classify(strv line);

  void set_bypass(bool on)  noexcept { mBypass = on; }
  bool is_bypass()    const noexcept { return mBypass; }

  void load_jeff(JeffConfig cfg);
  void load_jev(JevConfig cfg);

  void set_threshold(float t) noexcept { mThreshold = t; }

private:
  uptr<RouterModel> mModel;
  bool  mBypass    = false;
  float mThreshold = 0.5f;
};

}  // namespace jfsh

#endif  // JFSH_ROUTER_ROUTER_H
