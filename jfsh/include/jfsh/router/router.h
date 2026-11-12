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

#include <jfsh/core/types.h>

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
  virtual RouterDecision   classify(std::string_view line) = 0;
  virtual std::string_view name() const noexcept = 0;
};

struct HeuristicRouter : RouterModel {
  RouterDecision   classify(std::string_view line) override;
  std::string_view name() const noexcept override { return "heuristic"; }
};

// Configuration for the local jeff HTTP service (jeff-server binary).
struct JeffConfig {
  std::string   base_url   = "http://127.0.0.1:8765";
  std::uint32_t timeout_ms = 1000;  // local — fast timeout
};

// Configuration for the JEV cloud router (api.typesafe.ai).
struct JevConfig {
  std::string   api_key;
  std::string   model      = "jev-latest";
  std::string   base_url   = "https://api.typesafe.ai";
  std::uint32_t timeout_ms = 10000;

  static JevConfig from_env();  // reads TYPESAFE_API_KEY
};

struct Router {
  Router();

  RouterDecision classify(std::string_view line);

  void set_bypass(bool on)  noexcept { mBypass = on; }
  bool is_bypass()    const noexcept { return mBypass; }

  void load_jeff(JeffConfig cfg);
  void load_jev(JevConfig cfg);

  void set_threshold(float t) noexcept { mThreshold = t; }

private:
  std::unique_ptr<RouterModel> mModel;
  bool  mBypass    = false;
  float mThreshold = 0.5f;
};

}  // namespace jfsh

#endif  // JFSH_ROUTER_ROUTER_H
