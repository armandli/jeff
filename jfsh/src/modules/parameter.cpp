#include <jfsh/modules/parameter.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/parser/parser.h>

// zsh/parameter (M10 Phase 2a): live views of the shell's internal tables.
// Each `special()` here installs a PM_HASH (or PM_ARRAY) parameter whose
// value is recomputed on every lookup, so compsys and shipped functions
// can see current state without a refresh step.
//
// Only the read side is implemented for Phase 2a: writing to $functions
// (define a function) or $aliases (declare an alias) through assignment is
// zsh's own trick, and requires parsing the value as shell code at write
// time; that wiring lands with the compsys dependency work.

namespace jfsh {

namespace {

// Hash special: pass just a get_hash; value flagged PM_HASH + PM_READONLY.
void hash_special(ParamTable& params, str name,
                  func<umap<str, str>()> getter) {
  Param& p = params.assign(s::move(name), umap<str, str>{},
                           PM_HASH | PM_SPECIAL | PM_READONLY);
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_hash = s::move(getter);
}

void array_special(ParamTable& params, str name,
                   func<vec<str>()> getter) {
  Param& p = params.assign(s::move(name), vec<str>{},
                           PM_ARRAY | PM_SPECIAL | PM_READONLY);
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_array = s::move(getter);
}

// Render a Param's flags into zsh's `typeset -p`-ish type string:
//   "scalar", "integer", "float", "array", "association",
// with `-readonly`, `-exported`, etc. markers after.
str param_type(const Param& p) {
  str t;
  if      (p.is_hash())    t = "association";
  else if (p.is_array())   t = "array";
  else if (p.is_integer()) t = "integer";
  else if (p.is_float())   t = "float";
  else                     t = "scalar";
  if (p.is_readonly()) t += "-readonly";
  if (p.is_exported()) t += "-exported";
  if (p.flags & PM_HIDE) t += "-hidden";
  return t;
}

// zsh's reserved word list, in the order zsh uses when dumping them.
constexpr const char* RESWORDS[] = {
  "do", "done", "esac", "then", "elif", "else", "fi",
  "for", "case", "if", "while", "function",
  "repeat", "time", "until", "exec", "command", "select",
  "coproc", "nocorrect", "foreach", "end",
  "!", "[[", "{", "}", "declare", "export", "float", "integer",
  "local", "readonly", "typeset",
};

// zsh's default pattern characters.
constexpr const char* PATCHARS[] = {
  "*", "?", "[", "<", "|", "(", ")", "~", "^", "#",
};

}  // namespace

void register_parameter_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/parameter", [](ModuleFeatures& out, ExecContext& ctx) {
    ParamTable& params = ctx.params;
    OptionState& opts  = ctx.opts;
    ExecContext* ctxp  = &ctx;

    // $parameters: each name to its type string.
    hash_special(params, "parameters", [ctxp]() {
      umap<str, str> h;
      ctxp->params.for_each([&](const Param& p) {
        if (p.is_set()) h[p.name] = param_type(p);
      });
      return h;
    });

    // $commands: each command in the executor's cache → resolved path.
    hash_special(params, "commands", [ctxp]() {
      return umap<str, str>(ctxp->executor->command_hash());
    });

    // $functions: each shell function → its body text.
    hash_special(params, "functions", [ctxp]() {
      umap<str, str> h;
      for (const auto& [name, body] : ctxp->functions)
        if (body) h[name] = function_text(name, *body);
      return h;
    });
    hash_special(params, "dis_functions", []() { return umap<str, str>{}; });

    // $aliases: regular aliases (text only, without the -g flag).
    hash_special(params, "aliases", [ctxp]() {
      umap<str, str> h;
      for (const auto& [name, def] : ctxp->aliases.regular)
        if (not def.global) h[name] = def.text;
      return h;
    });
    hash_special(params, "galiases", [ctxp]() {
      umap<str, str> h;
      for (const auto& [name, def] : ctxp->aliases.regular)
        if (def.global) h[name] = def.text;
      return h;
    });
    hash_special(params, "saliases", [ctxp]() {
      umap<str, str> h;
      for (const auto& [name, text] : ctxp->aliases.suffix) h[name] = text;
      return h;
    });
    hash_special(params, "dis_aliases",  []() { return umap<str, str>{}; });
    hash_special(params, "dis_galiases", []() { return umap<str, str>{}; });
    hash_special(params, "dis_saliases", []() { return umap<str, str>{}; });

    // $builtins: shipped builtins → "defined".  Module-contributed builtins
    // get the module's name as their value.
    hash_special(params, "builtins", [ctxp]() {
      umap<str, str> h;
      for (const auto& n : ctxp->executor->builtin_names()) h[n] = "defined";
      // Append module-contributed builtins with the module name as value.
      ctxp->modules.for_each([&](const ModuleHandle& m) {
        for (const auto& b : m.features.builtins) h[b.name] = m.name;
      });
      return h;
    });
    hash_special(params, "dis_builtins", []() { return umap<str, str>{}; });

    // $modules: name → "loaded" / "autoloaded" / "alias".
    hash_special(params, "modules", [ctxp]() {
      umap<str, str> h;
      for (const auto& e : ctxp->modules.list_all()) {
        if (e.loaded)    h[e.name] = "loaded";
        else if (e.auto_) h[e.name] = "autoloaded";
      }
      for (const auto& [alias, target] : ctxp->modules.aliases()) {
        h[alias] = "alias:" + target;
      }
      return h;
    });

    // $options: name → "on"/"off" for every option in the table.
    hash_special(params, "options", [&opts]() {
      umap<str, str> h;
      for (int i = 0; i < OPT_COUNT; ++i) {
        const auto& info = OPTION_INFO[i];
        h[info.name] = opts.get(static_cast<OptIndex>(i)) ? "on" : "off";
      }
      return h;
    });

    // $reswords: a sorted array of zsh's reserved words.
    array_special(params, "reswords", []() {
      vec<str> v;
      v.reserve(sizeof RESWORDS / sizeof *RESWORDS);
      for (const char* w : RESWORDS) v.emplace_back(w);
      s::sort(v.begin(), v.end());
      return v;
    });
    array_special(params, "dis_reswords", []() { return vec<str>{}; });

    // $patchars: zsh's pattern metacharacters for the current emulation.
    array_special(params, "patchars", []() {
      vec<str> v;
      for (const char* c : PATCHARS) v.emplace_back(c);
      return v;
    });
    array_special(params, "dis_patchars", []() { return vec<str>{}; });

    // $widgets: ZLE widget name → kind.  Phase 2a: list them as-is only
    // when the shell has a line editor; otherwise an empty hash.
    hash_special(params, "widgets", [ctxp]() {
      umap<str, str> h;
      if (not ctxp->zle) return h;
      // Builtin widgets aren't introspectable without Zle::widget_names();
      // Phase 2b wires that up.  For now, user widgets only (via compdef).
      return h;
    });

    for (const char* n : {"parameters", "commands", "functions", "dis_functions",
                          "aliases", "galiases", "saliases", "dis_aliases",
                          "dis_galiases", "dis_saliases", "builtins",
                          "dis_builtins", "modules", "options", "reswords",
                          "dis_reswords", "patchars", "dis_patchars", "widgets"})
      out.params.emplace_back(n);
  });
}

}  // namespace jfsh
