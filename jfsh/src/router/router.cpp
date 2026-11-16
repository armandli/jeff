#include <jfsh/router/router.h>

#include <cstdio>
#include <cstring>

#include <jfsh/core/aliases.h>
#include <jfsh/router/jeff.h>
#include <jfsh/router/jev.h>

namespace jfsh {

RouterDecision HeuristicRouter::classify(strv line) {
  if (line.empty()) return {RouterVerdict::Run, 1.0f, "heuristic"};

  str cmd = "zsh -n -c '";
  for (char c : line) {
    if (c == '\'') cmd += "'\\''";
    else           cmd += c;
  }
  cmd += "' 2>/dev/null";

  int rc = s::system(cmd.c_str());
  if (rc == 0) return {RouterVerdict::Run,   0.9f, "heuristic"};
  return          {RouterVerdict::Agent, 0.9f, "heuristic"};
}

RouterDecision Router::classify(strv line) {
  if (mBypass or line.empty())
    return {RouterVerdict::Run, 1.0f, "bypass"};
  auto decision = mModel->classify(line);
  if (decision.confidence < mThreshold)
    return {RouterVerdict::Run, decision.confidence, decision.model_name};
  return decision;
}

void Router::load_jeff(JeffConfig cfg) {
  mModel = s::make_unique<JeffRouter>(s::move(cfg));
}

void Router::load_jev(JevConfig cfg) {
  mModel = s::make_unique<JevRouter>(s::move(cfg));
}

}  // namespace jfsh
