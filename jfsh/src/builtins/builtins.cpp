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
  add({"logout",   builtin_logout,     BINF_NONE});
  add({"bye",      builtin_exit,       BINF_NONE});
  add({"export",   builtin_export,     BINF_SPECIAL | BINF_ASSIGN});
  add({"readonly", builtin_readonly,   BINF_SPECIAL | BINF_ASSIGN});
  add({"return",   builtin_return,     BINF_SPECIAL});
  add({"set",      builtin_set,        BINF_SPECIAL});
  add({"shift",    builtin_shift,      BINF_SPECIAL});
  add({"getopts",  builtin_getopts,    BINF_NONE});
  add({"times",    builtin_times,      BINF_SPECIAL});
  add({"trap",     builtin_trap,       BINF_SPECIAL});
  add({"unset",    builtin_unset,      BINF_SPECIAL});
  add({"unfunction", builtin_unfunction, BINF_NONE});
  add({"enable",   builtin_enable,     BINF_NONE});
  add({"disable",  builtin_disable,    BINF_NONE});

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
  add({"unhash",   builtin_unhash,     BINF_NONE});
  add({"builtin",  builtin_builtin,    BINF_NONE});
  add({"command",  builtin_command,    BINF_NONE});
  add({"source",   builtin_source,     BINF_NONE, 1});
  add({"hash",     builtin_hash,       BINF_NONE});
  add({"rehash",   builtin_rehash,     BINF_NONE});
  add({"true",     builtin_true,       BINF_NONE});
  add({"false",    builtin_false,      BINF_NONE});
  add({"[",        builtin_test,       BINF_NONE});
  add({"test",     builtin_test,       BINF_NONE});
  add({"umask",    builtin_umask,      BINF_NONE});
  add({"zmodload", builtin_zmodload,   BINF_NONE});
  add({"autoload", builtin_autoload,   BINF_NONE});
  add({"functions",builtin_functions,  BINF_NONE});
  add({"whence",   builtin_whence,     BINF_NONE});
  add({"where",    builtin_where,      BINF_NONE});
  add({"ttyctl",   builtin_ttyctl,     BINF_NONE});
  add({"which",    builtin_which,      BINF_NONE});
  add({"type",     builtin_type,       BINF_NONE});
  add({"compdef",  builtin_compdef,    BINF_NONE});
  add({"_files",   builtin__files,     BINF_NONE});
  add({"_directories", builtin__directories, BINF_NONE});
  add({"_command_names", builtin__command_names, BINF_NONE});
  add({"setopt",   builtin_setopt,     BINF_NONE});
  add({"unsetopt", builtin_unsetopt,   BINF_NONE});
  add({"let",      builtin_let,        BINF_NONE});
  add({"emulate",  builtin_emulate,    BINF_NONE});
  add({"noglob",   builtin_noglob,     BINF_NONE});
  add({"-",        builtin_dash,       BINF_NONE});
  add({"chdir",    builtin_cd,         BINF_NONE, 0, 2});
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
// enclosing loop.  N past the outermost loop means the outermost.  A bad N
// or no loop is an error that ends the command line, as zsh's zerr.
static int loop_control(vec<str>& args, ExecContext& ctx,
                        bool cont) {
  const char* name = args[0].c_str();
  auto fail = [&] {
    ctx.aborting = ctx.abort_line = true;
    return 1;
  };
  s::int64_t n = 1;
  if (args.size() > 1) {
    auto v = arith_eval(args[1], ctx.params, math_options(ctx),
                        &ctx.modules.math_funcs());
    if (not v) {
      s::fprintf(stderr, "jfsh: %s: %s\n", name, v.error().message.c_str());
      return fail();
    }
    if (*v < 1) {
      s::fprintf(stderr, "jfsh: %s: argument is not positive: %" PRId64 "\n", name, *v);
      return fail();
    }
    n = s::min<s::int64_t>(*v, ctx.loop_depth);
  }
  if (ctx.loop_depth == 0) {
    s::fprintf(stderr, "jfsh: %s: not in while, until, select, or repeat loop\n", name);
    return fail();
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


// `let expr…`: evaluates each expression; the status is 0 when the last
// one is non-zero, 1 when it is zero.  As in zsh an error is not fatal: it
// ends the evaluation with the value 0, and the message has no `let:`.
int builtin_let(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return builtin_error(args[0], "not enough arguments");
  MathNum last;
  for (s::size_t i = 1; i < args.size(); ++i) {
    auto v = math_eval(args[i], ctx.params, math_options(ctx),
                       &ctx.modules.math_funcs());
    // An autoloaded parameter that wasn't defined: status 1, and the line
    // goes on (zsh's bin_let clears errflag).
    if (ctx.take_lookup_error()) return 1;
    if (not v) {
      s::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
      return 1;
    }
    last = *v;
  }
  return last.truthy() ? 0 : 1;
}

}  // namespace jfsh
