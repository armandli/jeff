#ifndef JFSH_MODULES_SUPPORT_H
#define JFSH_MODULES_SUPPORT_H
// jfsh/modules/support.h — small helpers shared by the M11 modules.

#include <cstdio>

#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>

namespace jfsh {

// `jfsh: cmd: msg` on stderr; returns 1 for `return mod_error(…)`.
inline int mod_error(strv cmd, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n",
             static_cast<int>(cmd.size()), cmd.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

// Removes a parameter a module installed (readonly or not), for the
// module's on_unload.
inline void drop_module_param(ParamTable& params, strv name) {
  if (auto p = params.lookup_raw(name)) {
    p->get().flags &= ~PM_READONLY;
    (void)params.unset(name);
  }
}

// A parameter's scalar value, or `def` when it is unset.
inline str param_or(ParamTable& params, strv name, str def = {}) {
  auto p = params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : def;
}

}  // namespace jfsh

#endif  // JFSH_MODULES_SUPPORT_H
