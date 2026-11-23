#include <jfsh/modules/parameter.h>

#include <grp.h>
#include <pwd.h>

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
// `elem`, when given, reads one key without making the whole table
// (${+functions[f]} is asked often).
Param& hash_special(ParamTable& params, str name,
                    func<umap<str, str>()> getter,
                    func<opt<str>(const str&)> elem = {}) {
  Param& p = params.assign(s::move(name), umap<str, str>{},
                           PM_HASH | PM_SPECIAL | PM_READONLY);
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_hash = s::move(getter);
  p.handlers->get_elem = s::move(elem);
  return p;
}

void array_special(ParamTable& params, str name,
                   func<vec<str>()> getter) {
  Param& p = params.assign(s::move(name), vec<str>{},
                           PM_ARRAY | PM_SPECIAL | PM_READONLY);
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_array = s::move(getter);
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
        if (p.is_set()) h[p.name] = param_type_text(p);
      });
      // The specials the expander answers itself.
      for (const char* n : {"!", "?", "#", "$", "ARGC"})
        h.try_emplace(n, "integer-readonly-special");
      for (const char* n : {"@", "*"}) h.try_emplace(n, "array-readonly-special");
      h.try_emplace("-", "scalar-readonly-special");
      h.try_emplace("0", "scalar-special");
      return h;
    });

    // $commands: each command in $PATH → its path.
    hash_special(params, "commands", [ctxp]() {
      return umap<str, str>(ctxp->executor->full_command_hash());
    }, [ctxp](const str& name) -> opt<str> {
      auto& h = ctxp->executor->full_command_hash();
      if (auto it = h.find(name); it != h.end()) return it->second;
      return s::nullopt;
    });

    // $functions: each shell function → its body text; one marked for
    // autoload and not loaded yet, as zsh shows it.
    hash_special(params, "functions", [ctxp]() {
      umap<str, str> h;
      for (const auto& [name, body] : ctxp->functions)
        if (body) h[name] = function_text(name, *body);
      for (const auto& [name, _] : ctxp->autoloads)
        if (not h.contains(name)) h[name] = autoload_stub_body(*ctxp, name, false);
      return h;
    }, [ctxp](const str& name) -> opt<str> {
      if (auto it = ctxp->functions.find(name); it != ctxp->functions.end() and it->second)
        return function_text(name, *it->second);
      if (ctxp->autoloads.contains(name)) return autoload_stub_body(*ctxp, name, false);
      return s::nullopt;
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
      // Autoloaded ones not loaded yet, as zsh shows them.
      for (const auto& [b, mod] : ctxp->modules.autoloads())
        if (not h.contains(b)) h[b] = "undefined";
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

    // $userdirs: each user's home directory; $usergroups: each group's
    // id; $nameddirs: the named directories (none until hash -d exists).
    hash_special(params, "userdirs", []() {
      umap<str, str> h;
      ::setpwent();
      while (struct passwd* pw = ::getpwent())
        if (pw->pw_name and pw->pw_dir) h.emplace(pw->pw_name, pw->pw_dir);
      ::endpwent();
      return h;
    });
    hash_special(params, "usergroups", []() {
      umap<str, str> h;
      ::setgrent();
      while (struct group* gr = ::getgrent())
        if (gr->gr_name) h.emplace(gr->gr_name, s::to_string(gr->gr_gid));
      ::endgrent();
      return h;
    });
    hash_special(params, "nameddirs", []() { return umap<str, str>{}; });

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
                          "userdirs", "usergroups", "nameddirs",
                          "aliases", "galiases", "saliases", "dis_aliases",
                          "dis_galiases", "dis_saliases", "builtins",
                          "dis_builtins", "modules", "options", "reswords",
                          "dis_reswords", "patchars", "dis_patchars", "widgets"})
      out.params.emplace_back(n);
  });
}

}  // namespace jfsh
