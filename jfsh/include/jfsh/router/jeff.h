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

#include <jfsh/core/types.h>
#include <jfsh/router/router.h>

namespace jfsh {

struct JeffRouter : RouterModel {
  explicit JeffRouter(JeffConfig cfg);

  RouterDecision   classify(std::string_view line) override;
  std::string_view name() const noexcept override { return "jeff"; }

private:
  JeffConfig mCfg;

  std::string            build_body(std::string_view line) const;
  Result<std::string>    http_post(std::string_view url,
                                   std::string_view body) const;
  Result<RouterDecision> parse_response(std::string_view json) const;
};

}  // namespace jfsh

#endif  // JFSH_ROUTER_JEFF_H
