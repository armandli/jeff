#include <cstdio>
#include <cstring>

#include <jfsh/router/jeff.h>
#include <jfsh/router/jev.h>
#include <jfsh/router/router.h>

namespace jfsh {

RouterDecision HeuristicRouter::classify(std::string_view line) {
  if (line.empty()) return {RouterVerdict::Run, 1.0f, "heuristic"};

  std::string cmd = "zsh -n -c '";
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

void Router::load_jeff(JeffConfig cfg) {
  mModel = std::make_unique<JeffRouter>(std::move(cfg));
}

void Router::load_jev(JevConfig cfg) {
  mModel = std::make_unique<JevRouter>(std::move(cfg));
}

}  // namespace jfsh
