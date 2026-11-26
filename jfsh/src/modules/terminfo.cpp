#include <jfsh/modules/terminfo.h>

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// <term.h> defines a macro for every capability (`lines`, `columns`,
// `tab`, …), so it comes last and this file avoids those names.
#ifdef JFSH_HAVE_CURSES
#define NCURSES_NOMACROS
#include <curses.h>
#include <term.h>
#endif

// zsh/terminfo (M11): `echoti` and `$terminfo`, as zsh's
// Src/Modules/terminfo.c.

namespace jfsh {

#ifdef JFSH_HAVE_CURSES

bool term_setup(const str& term) {
  static str loaded;
  static bool ok = false;
  if (term.empty()) return false;
  if (term == loaded) return ok;
  int status = 0;
  ok = ::setupterm(const_cast<char*>(term.c_str()), STDOUT_FILENO, &status) == OK;
  loaded = term;
  return ok;
}

int term_colors(const str& term) {
  if (not term_setup(term)) return -1;
  int n = ::tigetnum(const_cast<char*>("colors"));
  return n >= 0 ? n : -1;
}

namespace {

str term_of(ExecContext& ctx) { return param_or(ctx.params, "TERM"); }

// A capability's value as $terminfo shows it: a number, yes/no for a
// boolean, the raw string.
opt<str> capability(const str& name) {
  char* cap = const_cast<char*>(name.c_str());
  int num = ::tigetnum(cap);
  if (num >= 0) return s::to_string(num);
  int flag = ::tigetflag(cap);
  if (flag >= 0) return str(flag ? "yes" : "no");
  char* v = ::tigetstr(cap);
  if (v and v != reinterpret_cast<char*>(-1)) return str(v);
  return s::nullopt;
}

int put_byte(int c) { return s::putchar(c); }

int builtin_echoti(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return mod_error("echoti", "not enough arguments");
  if (not term_setup(term_of(ctx)))
    return mod_error("echoti", "terminal does not support necessary capabilities");
  char* cap = const_cast<char*>(args[1].c_str());
  if (int num = ::tigetnum(cap); num >= 0) {
    s::printf("%d\n", num);
    return 0;
  }
  switch (::tigetflag(cap)) {
    case -1: break;
    case 0:  s::puts("no");  return 0;
    default: s::puts("yes"); return 0;
  }
  char* v = ::tigetstr(cap);
  if (not v or v == reinterpret_cast<char*>(-1) or not *v)
    return mod_error("echoti", "no such terminfo capability: " + args[1]);
  if (args.size() > 11) return mod_error("echoti", "too many arguments");
  long p[9] = {};
  for (s::size_t k = 2; k < args.size(); ++k)
    p[k - 2] = s::strtol(args[k].c_str(), nullptr, 10);
  const char* out = args.size() > 2
      ? ::tparm(v, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8])
      : v;
  if (out) ::tputs(out, 1, put_byte);
  s::fflush(stdout);
  return 0;
}

}  // namespace

void register_terminfo_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/terminfo", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* ctxp = &ctx;
    out.builtins.push_back(BuiltinDef{"echoti", builtin_echoti, BINF_NONE, 1});
    Param& p = ctx.params.assign("terminfo", umap<str, str>{},
                                 PM_HASH | PM_SPECIAL | PM_READONLY);
    p.handlers = s::make_unique<ParamHandlers>();
    p.handlers->get_hash = [ctxp] {
      umap<str, str> h;
      if (not term_setup(term_of(*ctxp))) return h;
      for (auto* names : {boolnames, numnames, strnames})
        for (s::size_t k = 0; names[k]; ++k)
          if (auto v = capability(names[k])) h[names[k]] = *v;
      return h;
    };
    p.handlers->get_elem = [ctxp](const str& key) -> opt<str> {
      if (not term_setup(term_of(*ctxp))) return s::nullopt;
      return capability(key);
    };
    out.params.emplace_back("terminfo");
    out.on_unload = [ctxp] { drop_module_param(ctxp->params, "terminfo"); };
  });
}

#else  // no curses

bool term_setup(const str&) { return false; }
int  term_colors(const str&) { return -1; }

namespace {

int builtin_echoti(vec<str>&, ExecContext&) {
  return mod_error("echoti", "terminfo is not available (built without curses)");
}

}  // namespace

void register_terminfo_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/terminfo", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"echoti", builtin_echoti, BINF_NONE, 1});
  });
}

#endif

}  // namespace jfsh
