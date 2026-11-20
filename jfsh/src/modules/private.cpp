#include <jfsh/modules/private.h>

#include <jfsh/builtins/builtins.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/param/private (M11): `private`, a `local` that the functions it calls
// can't see, as zsh's Src/Modules/param_private.c.  The parameter table
// does the hiding (PM_PRIVATE, see ParamTable::lookup_raw); this builtin
// declares the parameters through `local` and marks them.

namespace jfsh {

namespace {

int builtin_private(vec<str>& args, ExecContext& ctx) {
  // The names, before `local` sees (and may rewrite) the arguments.
  vec<str> names;
  bool options = true;
  for (s::size_t i = 1; i < args.size(); ++i) {
    const str& a = args[i];
    if (options and a == "--") { options = false; continue; }
    if (options and not a.empty() and (a[0] == '-' or a[0] == '+')) continue;
    options = false;
    auto eq = a.find('=');
    str name = a.substr(0, eq);
    if (not name.empty() and name.back() == '+') name.pop_back();
    names.push_back(s::move(name));
  }
  vec<str> local_args = args;
  local_args[0] = "local";
  int rc = builtin_local(local_args, ctx);
  if (rc != 0) return rc;
  for (const auto& n : names)
    if (auto p = ctx.params.lookup_local(n)) p->get().flags |= PM_PRIVATE;
  return 0;
}

}  // namespace

void register_private_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/param/private", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"private", builtin_private, BINF_TYPESET, 0});
    out.feature_names = {"b:private"};
  });
  // jfsh's earlier name for it.
  mm.add_alias("zsh/private", "zsh/param/private");
}

}  // namespace jfsh
