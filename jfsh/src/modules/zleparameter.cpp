#include <jfsh/modules/zleparameter.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/executor.h>

// zsh/zleparameter (M11 Phase 11a): $widgets and $keymaps live views of
// the ZLE state (keymaps, builtin widgets, user widgets, zle -A aliases).

namespace jfsh {

void register_zleparameter_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/zleparameter", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* ctxp = &ctx;

    // $widgets — associative: widget name → kind string.
    Param& pw = ctx.params.assign("widgets", umap<str, str>{},
                                  PM_HASH | PM_SPECIAL | PM_READONLY);
    pw.handlers = s::make_unique<ParamHandlers>();
    pw.handlers->get_hash = [ctxp]() {
      umap<str, str> h;
      if (not ctxp->zle) return h;
      const Zle& zle = *ctxp->zle;
      for (const auto& name : Zle::builtin_widgets()) {
        h["." + name] = "builtin";
        if (not zle.deleted_widgets.contains(name)) h[name] = "builtin";
      }
      for (const auto& [name, _] : zle.module_widgets) h[name] = "builtin";
      for (const auto& [name, target] : zle.widget_aliases) h[name] = "builtin";
      for (const auto& [name, fn] : zle.user_widgets) {
        auto c = zle.comp_widgets.find(name);
        h[name] = c == zle.comp_widgets.end() ? "user:" + fn : "completion:" + c->second + ":" + fn;
      }
      return h;
    };

    // $keymaps — array of keymap names.
    Param& pk = ctx.params.assign("keymaps", vec<str>{},
                                  PM_ARRAY | PM_SPECIAL | PM_READONLY);
    pk.handlers = s::make_unique<ParamHandlers>();
    pk.handlers->get_array = [ctxp]() {
      vec<str> v;
      if (not ctxp->zle) return v;
      for (const auto& [name, _] : ctxp->zle->keymaps) v.push_back(name);
      s::sort(v.begin(), v.end());
      return v;
    };

    out.params.emplace_back("widgets");
    out.params.emplace_back("keymaps");
    out.feature_names = {"p:widgets", "p:keymaps"};
  });
}

}  // namespace jfsh
