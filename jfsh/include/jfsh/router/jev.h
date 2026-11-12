#ifndef JFSH_ROUTER_JEV_H
#define JFSH_ROUTER_JEV_H
// jfsh/router/jev.h — System 1 router backed by TypeSafe JEV cloud API.
//
// Sends every candidate line to POST /v1/systemone as a "choice" question
// with two options: "run" (execute as shell) or "agent" (forward to LLM).
// Falls back to HeuristicRouter on any network or parse error so the shell
// stays usable without a network connection.
//
// API: https://api.typesafe.ai  (Bearer auth via TYPESAFE_API_KEY)

#include <string>
#include <string_view>

#include <jfsh/core/types.h>
#include <jfsh/router/router.h>

namespace jfsh {

struct JevRouter : RouterModel {
  explicit JevRouter(JevConfig cfg);

  RouterDecision   classify(std::string_view line) override;
  std::string_view name() const noexcept override { return "jev"; }

private:
  JevConfig mCfg;

  std::string            build_body(std::string_view line) const;
  Result<std::string>    http_post(std::string_view url,
                                   std::string_view body) const;
  Result<RouterDecision> parse_response(std::string_view json) const;
};

}  // namespace jfsh

#endif  // JFSH_ROUTER_JEV_H
