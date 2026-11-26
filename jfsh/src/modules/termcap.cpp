#include <jfsh/modules/termcap.h>

#include <cstdio>
#include <cstdlib>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>
#include <jfsh/modules/terminfo.h>

// <term.h> defines a macro per capability; it comes last (see terminfo.cpp).
#ifdef JFSH_HAVE_CURSES
#define NCURSES_NOMACROS
#include <curses.h>
#include <term.h>
#endif

// zsh/termcap (M11): `echotc` and `$termcap`, as zsh's
// Src/Modules/termcap.c, over curses' termcap emulation.

namespace jfsh {

namespace {

#ifdef JFSH_HAVE_CURSES

str term_of(ExecContext& ctx) { return param_or(ctx.params, "TERM"); }

opt<str> capability(const str& name) {
  char* cap = const_cast<char*>(name.c_str());
  int num = ::tgetnum(cap);
  if (num >= 0) return s::to_string(num);
  if (::tgetflag(cap) > 0) return str("yes");
  char  buf[2048];
  char* area = buf;
  char* v = ::tgetstr(cap, &area);
  if (v and v != reinterpret_cast<char*>(-1) and *v) return str(v);
  return s::nullopt;
}

int put_byte(int c) { return s::putchar(c); }

int builtin_echotc(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return mod_error("echotc", "not enough arguments");
  if (not term_setup(term_of(ctx)))
    return mod_error("echotc", "terminal does not support necessary capabilities");
  char* cap = const_cast<char*>(args[1].c_str());
  if (int num = ::tgetnum(cap); num >= 0) {
    s::printf("%d\n", num);
    return 0;
  }
  if (::tgetflag(cap) > 0) {
    s::puts("yes");
    return 0;
  }
  char  buf[2048];
  char* area = buf;
  char* v = ::tgetstr(cap, &area);
  if (not v or v == reinterpret_cast<char*>(-1) or not *v)
    return mod_error("echotc", "no such capability: " + args[1]);
  if (args.size() > 4) return mod_error("echotc", "too many arguments");
  if (args.size() == 2) {
    ::tputs(v, 1, put_byte);
  } else {
    // termcap's argument order is "column row".
    int a = s::atoi(args[2].c_str());
    int b = args.size() > 3 ? s::atoi(args[3].c_str()) : a;
    ::tputs(::tgoto(v, a, b), 1, put_byte);
  }
  s::fflush(stdout);
  return 0;
}

#else

int builtin_echotc(vec<str>&, ExecContext&) {
  return mod_error("echotc", "termcap is not available (built without curses)");
}

#endif

}  // namespace

void register_termcap_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/termcap", [](ModuleFeatures& out, ExecContext& ctx) {
    out.builtins.push_back(BuiltinDef{"echotc", builtin_echotc, BINF_NONE, 1});
#ifdef JFSH_HAVE_CURSES
    ExecContext* ctxp = &ctx;
    Param& p = ctx.params.assign("termcap", umap<str, str>{},
                                 PM_HASH | PM_SPECIAL | PM_READONLY);
    p.handlers = s::make_unique<ParamHandlers>();
    p.handlers->get_hash = [ctxp] {
      umap<str, str> h;
      if (not term_setup(term_of(*ctxp))) return h;
      for (auto* codes : {boolcodes, numcodes, strcodes})
        for (s::size_t k = 0; codes[k]; ++k)
          if (auto v = capability(codes[k])) h[codes[k]] = *v;
      return h;
    };
    p.handlers->get_elem = [ctxp](const str& key) -> opt<str> {
      if (not term_setup(term_of(*ctxp))) return s::nullopt;
      return capability(key);
    };
    out.params.emplace_back("termcap");
    out.on_unload = [ctxp] { drop_module_param(ctxp->params, "termcap"); };
#else
    (void)ctx;
#endif
  });
}

}  // namespace jfsh
