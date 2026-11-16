#ifndef JFSH_ROUTER_JEFF_H
#define JFSH_ROUTER_JEFF_H
// jfsh/router/jeff.h — System 1 router backed by the local jeff-server.
//
// Sends every candidate line to POST /classify on the running jeff-server
// binary (127.0.0.1:8765 by default). Falls back to HeuristicRouter on any
// connection error so the shell remains usable when the service is not running.
//
// Start the service:  jeff-server --model <checkpoint_dir>

#include <string>
#include <string_view>
#include <utility>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>
#include <jfsh/router/router.h>

namespace jfsh {

struct JeffRouter : RouterModel {
  explicit JeffRouter(JeffConfig cfg) : mCfg(s::move(cfg)) {}

  RouterDecision classify(strv line) override;
  strv           name() const noexcept override { return "jeff"; }

protected:
  str            build_body(strv line) const;
  Result<str>    http_post(strv url,
                           strv body) const;
  Result<RouterDecision> parse_response(strv json) const;

private:
  JeffConfig mCfg;
};

}  // namespace jfsh

#endif  // JFSH_ROUTER_JEFF_H
