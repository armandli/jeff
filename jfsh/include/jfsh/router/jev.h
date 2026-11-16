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
#include <utility>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>
#include <jfsh/router/router.h>

namespace jfsh {

struct JevRouter : RouterModel {
  explicit JevRouter(JevConfig cfg) : mCfg(s::move(cfg)) {}

  RouterDecision classify(strv line) override;
  strv           name() const noexcept override { return "jev"; }

protected:
  str            build_body(strv line) const;
  Result<str>    http_post(strv url,
                           strv body) const;
  Result<RouterDecision> parse_response(strv json) const;

private:
  JevConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_ROUTER_JEV_H
