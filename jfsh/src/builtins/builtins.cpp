#include <jfsh/builtins/builtins.h>

#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

void BuiltinTable::register_all() {
  add({":",        builtin_colon,      BINF_SPECIAL});
  add({".",        builtin_dot,        BINF_SPECIAL, 1});
  add({"break",    builtin_break,      BINF_SPECIAL});
  add({"continue", builtin_continue,   BINF_SPECIAL});
  add({"eval",     builtin_eval,       BINF_SPECIAL});
  add({"exec",     builtin_exec,       BINF_SPECIAL});
  add({"exit",     builtin_exit,       BINF_SPECIAL});
  add({"export",   builtin_export,     BINF_SPECIAL | BINF_ASSIGN});
  add({"readonly", builtin_readonly,   BINF_SPECIAL | BINF_ASSIGN});
  add({"return",   builtin_return,     BINF_SPECIAL});
  add({"set",      builtin_set,        BINF_SPECIAL});
  add({"shift",    builtin_shift,      BINF_SPECIAL});
  add({"times",    builtin_times,      BINF_SPECIAL});
  add({"trap",     builtin_trap,       BINF_SPECIAL});
  add({"unset",    builtin_unset,      BINF_SPECIAL});

  add({"cd",       builtin_cd,         BINF_NONE, 0, 2});
  add({"pushd",    builtin_pushd,      BINF_NONE});
  add({"popd",     builtin_popd,       BINF_NONE});
  add({"dirs",     builtin_dirs,       BINF_NONE});
  add({"pwd",      builtin_pwd,        BINF_NONE});

  add({"bg",       builtin_bg,         BINF_NONE});
  add({"fg",       builtin_fg,         BINF_NONE});
  add({"jobs",     builtin_jobs,       BINF_NONE});
  add({"wait",     builtin_wait,       BINF_NONE});
  add({"disown",   builtin_disown,     BINF_NONE});
  add({"kill",     builtin_kill,       BINF_NONE});

  add({"typeset",  builtin_typeset,    BINF_TYPESET | BINF_ASSIGN});
  add({"local",    builtin_local,      BINF_TYPESET | BINF_ASSIGN});
  add({"declare",  builtin_declare,    BINF_TYPESET | BINF_ASSIGN});
  add({"integer",  builtin_integer,    BINF_TYPESET | BINF_ASSIGN});
  add({"float",    builtin_float,      BINF_TYPESET | BINF_ASSIGN});

  add({"echo",     builtin_echo,       BINF_PRINT | BINF_ECHOOPT});
  add({"print",    builtin_print,      BINF_PRINT});
  add({"printf",   builtin_printf,     BINF_PRINT});

  add({"read",     builtin_read,       BINF_NONE});

  add({"fc",       builtin_fc,         BINF_NONE});
  add({"history",  builtin_history,    BINF_NONE});
  add({"r",        builtin_r,          BINF_NONE});

  add({"alias",    builtin_alias,      BINF_NONE});
  add({"unalias",  builtin_unalias,    BINF_NONE});
  add({"builtin",  builtin_builtin,    BINF_NONE});
  add({"command",  builtin_command,    BINF_NONE});
  add({"source",   builtin_source,     BINF_NONE, 1});
  add({"hash",     builtin_hash,       BINF_NONE});
  add({"rehash",   builtin_rehash,     BINF_NONE});
  add({"true",     builtin_true,       BINF_NONE});
  add({"false",    builtin_false,      BINF_NONE});
  add({"[",        builtin_test,       BINF_NONE});
  add({"test",     builtin_test,       BINF_NONE});
  add({"ulimit",   builtin_ulimit,     BINF_NONE});
  add({"umask",    builtin_umask,      BINF_NONE});
  add({"zmodload", builtin_zmodload,   BINF_NONE});
  add({"autoload", builtin_autoload,   BINF_NONE});
  add({"functions",builtin_functions,  BINF_NONE});
  add({"whence",   builtin_whence,     BINF_NONE});
  add({"which",    builtin_which,      BINF_NONE});
  add({"type",     builtin_type,       BINF_NONE});
  add({"compinit", builtin_compinit,   BINF_NONE});
  add({"compdef",  builtin_compdef,    BINF_NONE});
  add({"compadd",  builtin_compadd,    BINF_NONE});
  add({"_files",   builtin__files,     BINF_NONE});
  add({"_directories", builtin__directories, BINF_NONE});
  add({"_command_names", builtin__command_names, BINF_NONE});
  add({"setopt",   builtin_setopt,     BINF_NONE});
  add({"unsetopt", builtin_unsetopt,   BINF_NONE});
  add({"bindkey",  builtin_bindkey,    BINF_NONE});
  add({"vared",    builtin_vared,      BINF_NONE});
  add({"zle",      builtin_zle,        BINF_NONE});
  add({"let",      builtin_let,        BINF_NONE});
  add({"sched",    builtin_sched,      BINF_NONE});
  add({"emulate",  builtin_emulate,    BINF_NONE});
  add({"noglob",   builtin_noglob,     BINF_NONE});
  add({"-",        builtin_dash,       BINF_NONE});
  add({"chdir",    builtin_cd,         BINF_NONE, 0, 2});
  add({"zpty",     builtin_zpty,       BINF_NONE});
}

optref<const BuiltinDef> BuiltinTable::find(strv name) const noexcept {
  auto it = mTable.find(str(name));
  if (it == mTable.end()) return s::nullopt;
  return s::cref(it->second);
}

void BuiltinTable::for_each(func<void(const BuiltinDef&)> fn) const {
  for (const auto& [name, def] : mTable) fn(def);
}

int builtin_colon(vec<str>&, ExecContext&) { return 0; }
int builtin_true (vec<str>&, ExecContext&) { return 0; }
int builtin_false(vec<str>&, ExecContext&) { return 1; }
// builtin_test (`test` / `[`) is in cond.cpp, the job-control builtins and
// trap in jobctl.cpp; the rest are by group in vars.cpp, setopt.cpp,
// dirs.cpp, print.cpp, read.cpp, eval.cpp, lookup.cpp, limits.cpp and
// hist.cpp.

// `break [N]` / `continue [N]`: leave, or start the next pass of, the Nth
// enclosing loop.  N past the outermost loop means the outermost.
static int loop_control(vec<str>& args, ExecContext& ctx,
                        bool cont) {
  const char* name = args[0].c_str();
  s::int64_t n = 1;
  if (args.size() > 1) {
    auto v = arith_eval(args[1], ctx.params, math_options(ctx.opts),
                        &ctx.modules.math_funcs());
    if (not v) {
      s::fprintf(stderr, "jfsh: %s: %s\n", name, v.error().message.c_str());
      return 1;
    }
    if (*v < 1) {
      s::fprintf(stderr, "jfsh: %s: argument is not positive: %" PRId64 "\n", name, *v);
      return 1;
    }
    n = s::min<s::int64_t>(*v, ctx.loop_depth);
  }
  if (ctx.loop_depth == 0) {
    s::fprintf(stderr, "jfsh: %s: not in while, until, select, or repeat loop\n", name);
    return 1;
  }
  ctx.break_level = static_cast<int>(s::min<s::int64_t>(n, ctx.loop_depth));
  ctx.continuing  = cont;
  return 0;
}
int builtin_break(vec<str>& args, ExecContext& ctx) {
  return loop_control(args, ctx, false);
}
int builtin_continue(vec<str>& args, ExecContext& ctx) {
  return loop_control(args, ctx, true);
}
// `noglob` and `-` are precommand modifiers the executor strips before it
// looks the command up; these run only as `builtin noglob cmd …`.
static int run_words(vec<str>& args, ExecContext& ctx, strv prefix) {
  if (args.size() < 2) return 0;
  str text(prefix);
  for (s::size_t i = 1; i < args.size(); ++i) text += " " + shell_quote(args[i]);
  return ctx.executor->exec_string(text);
}
int builtin_noglob(vec<str>& args, ExecContext& ctx) { return run_words(args, ctx, "noglob"); }
int builtin_dash(vec<str>& args, ExecContext& ctx)   { return run_words(args, ctx, "-"); }

// Builtins that need the module system say so rather than pretending to
// work (B19).  The line editor's (bindkey, zle, vared, compinit, …) are in
// zle.cpp.
static int not_implemented(const vec<str>& args) {
  s::fprintf(stderr, "jfsh: %s: not implemented\n", args[0].c_str());
  return 1;
}
int builtin_zpty    (vec<str>& args, ExecContext&) { return not_implemented(args); }

// `zmodload` (M10): loads, unloads, lists, and aliases modules.  Flags
// accepted:
//   -L           list in a sourceable form (used by `zmodload` alone too)
//   -l           list as `zmodload -L` does, long form
//   -i           ignore "already loaded" / "not loaded" errors
//   -u           unload the module
//   -A alias=target  alias chain (`zmodload -A new=zsh/old`)
//   -R alias     remove an alias
//   -a NAME [BLTN]  autoload: the first use of BLTN loads NAME
//   -d           show module dependencies (treated as "list", none kept)
//   -e NAME…     exit status 0 iff every NAME is loaded
//   -F NAME ±ft  features (accepted; no fine-grained toggles yet)
// Unsupported combinations print `jfsh: zmodload: …` and return 1.
int builtin_zmodload(vec<str>& args, ExecContext& ctx) {
  bool list_short = false;    // -L
  bool list_long  = false;    // -l
  bool ignore     = false;    // -i
  bool unload     = false;    // -u
  bool alias_set  = false;    // -A
  bool alias_del  = false;    // -R
  bool autoload   = false;    // -a
  bool check      = false;    // -e
  bool features   = false;    // -F

  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a.size() < 2 or a[0] != '-' or a == "--") { if (a == "--") ++i; break; }
    for (s::size_t k = 1; k < a.size(); ++k) {
      switch (a[k]) {
        case 'L': list_short = true; break;
        case 'l': list_long  = true; break;
        case 'i': ignore     = true; break;
        case 'u': unload     = true; break;
        case 'A': alias_set  = true; break;
        case 'R': alias_del  = true; break;
        case 'a': autoload   = true; break;
        case 'e': check      = true; break;
        case 'F': features   = true; break;
        case 'd': /* dependency listing: tracked but unused */ break;
        case 'b': case 'c': case 'p': case 'f':
          // -b/-c/-p/-f fine-grained autoload categories: parsed, treated
          // the same as -a.
          autoload = true; break;
        case 'm': case 'U': case 'I':
          // -m pattern matching, -U unloadable, -I ignore if listed:
          // accepted, no-op.
          break;
        default:
          s::fprintf(stderr, "jfsh: zmodload: bad option: -%c\n", a[k]);
          return 1;
      }
    }
  }

  // Alias set: zmodload -A alias=target …
  if (alias_set) {
    bool any = false;
    for (; i < args.size(); ++i) {
      auto eq = args[i].find('=');
      if (eq == str::npos) {
        if (not any) {
          // zmodload -A alone lists aliases.
          for (const auto& [k, v] : ctx.modules.aliases())
            s::printf("%s -> %s\n", k.c_str(), v.c_str());
          return 0;
        }
        s::fprintf(stderr, "jfsh: zmodload: -A arguments must be of the form name=module\n");
        return 1;
      }
      ctx.modules.add_alias(args[i].substr(0, eq), args[i].substr(eq + 1));
      any = true;
    }
    if (not any) {
      for (const auto& [k, v] : ctx.modules.aliases())
        s::printf("%s -> %s\n", k.c_str(), v.c_str());
    }
    return 0;
  }

  if (alias_del) {
    if (i >= args.size()) {
      s::fprintf(stderr, "jfsh: zmodload: -R requires an alias name\n");
      return 1;
    }
    for (; i < args.size(); ++i) ctx.modules.remove_alias(args[i]);
    return 0;
  }

  if (check) {
    for (; i < args.size(); ++i)
      if (not ctx.modules.is_loaded(args[i])) return 1;
    return 0;
  }

  if (autoload) {
    if (i >= args.size()) {
      s::fprintf(stderr, "jfsh: zmodload: -a requires a module name\n");
      return 1;
    }
    str mod = args[i++];
    if (i >= args.size()) {
      ctx.modules.mark_autoload(mod);
    } else {
      for (; i < args.size(); ++i) ctx.modules.mark_autoload(mod, args[i]);
    }
    return 0;
  }

  // No module arguments → list.  zsh's `-L` is a sourceable form that
  // includes autoloads and aliases; `-l` lists loaded modules only; no
  // flag lists loaded modules by name.
  if (i >= args.size()) {
    auto entries = list_short ? ctx.modules.list_all() : ctx.modules.list();
    for (const auto& e : entries) {
      if (list_short or list_long) {
        if (e.loaded)       s::printf("zmodload %s\n", e.name.c_str());
        else if (e.auto_)   s::printf("zmodload -a %s\n", e.name.c_str());
      } else {
        s::printf("%s\n", e.name.c_str());
      }
    }
    if (list_short) {
      for (const auto& [a, t] : ctx.modules.aliases())
        s::printf("zmodload -A %s=%s\n", a.c_str(), t.c_str());
    }
    return 0;
  }

  // Unload one or more modules.
  if (unload) {
    int rc = 0;
    for (; i < args.size(); ++i) {
      if (not ctx.modules.is_loaded(args[i])) {
        if (not ignore) {
          s::fprintf(stderr, "jfsh: zmodload: no such module: %s\n", args[i].c_str());
          rc = 1;
        }
        continue;
      }
      auto r = ctx.modules.unload(args[i]);
      if (not r) {
        s::fprintf(stderr, "jfsh: zmodload: %s\n", r.error().message.c_str());
        rc = 1;
      }
    }
    return rc;
  }

  // Load one or more modules.  `-F NAME +feat / -feat / feat`: a leading
  // `+feat` or bare `feat` is a no-op (the feature is on by default); a
  // leading `-feat` records the feature as disabled, so the module's
  // next load leaves it out.  Matches zsh's behaviour when combined with
  // the re-register path below.
  int rc = 0;
  if (features) {
    if (i >= args.size()) {
      s::fprintf(stderr, "jfsh: zmodload: -F requires a module name\n");
      return 1;
    }
    str mod = args[i++];
    uset<str> off;
    // Preserve any previously-disabled features so successive -F calls
    // add to the set rather than replacing it.
    if (auto* prev = ctx.modules.disabled_features(mod)) off = *prev;
    for (; i < args.size(); ++i) {
      const str& a = args[i];
      if (a.empty()) continue;
      if (a[0] == '-')       off.insert(a.substr(1));
      else if (a[0] == '+')  off.erase (a.substr(1));
      else                   off.erase (a);      // bare feature = enable
    }
    bool was_loaded = ctx.modules.is_loaded(mod);
    if (was_loaded) (void)ctx.modules.unload(mod);
    ctx.modules.set_disabled_features(mod, s::move(off));
    if (was_loaded) {
      auto r = ctx.modules.load(mod, &ctx);
      if (not r) {
        s::fprintf(stderr, "jfsh: zmodload: %s\n", r.error().message.c_str());
        return 1;
      }
    }
    return 0;
  }

  for (; i < args.size(); ++i) {
    if (ctx.modules.is_loaded(args[i])) {
      if (not ignore) {
        // zsh is silent on re-load; stay silent too.
      }
      continue;
    }
    auto r = ctx.modules.load(args[i], &ctx);
    if (not r) {
      s::fprintf(stderr, "jfsh: zmodload: %s\n", r.error().message.c_str());
      rc = 1;
    }
  }
  return rc;
}

// `let expr…`: evaluates each expression; the status is 0 when the last
// one is non-zero, 1 when it is zero, and 2 on an error.
int builtin_let(vec<str>& args, ExecContext& ctx) {
  MathNum last;
  for (s::size_t i = 1; i < args.size(); ++i) {
    auto v = math_eval(args[i], ctx.params, math_options(ctx.opts),
                       &ctx.modules.math_funcs());
    if (not v) {
      s::fprintf(stderr, "jfsh: let: %s\n", v.error().message.c_str());
      return 2;
    }
    last = *v;
  }
  return last.truthy() ? 0 : 1;
}

}  // namespace jfsh
