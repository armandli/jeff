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
  add({"setopt",   builtin_setopt,     BINF_NONE});
  add({"unsetopt", builtin_unsetopt,   BINF_NONE});
  add({"bindkey",  builtin_bindkey,    BINF_NONE});
  add({"vared",    builtin_vared,      BINF_NONE});
  add({"zle",      builtin_zle,        BINF_NONE});
  add({"let",      builtin_let,        BINF_NONE});
  add({"sched",    builtin_sched,      BINF_NONE});
}

optref<const BuiltinDef> BuiltinTable::find(strv name) const noexcept {
  auto it = mTable.find(str(name));
  if (it == mTable.end()) return s::nullopt;
  return s::cref(it->second);
}

void BuiltinTable::for_each(func<void(const BuiltinDef&)> fn) const {
  for (const auto& [name, def] : mTable) fn(def);
}

int builtin_colon     (vec<str>&, ExecContext&) { return 0; }
// `break [N]` / `continue [N]`: leave, or start the next pass of, the Nth
// enclosing loop.  N past the outermost loop means the outermost.
static int loop_control(vec<str>& args, ExecContext& ctx,
                        bool cont) {
  const char* name = args[0].c_str();
  s::int64_t n = 1;
  if (args.size() > 1) {
    auto v = arith_eval(args[1], ctx.params, math_options(ctx.opts));
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
int builtin_dot(vec<str>& args, ExecContext&) {
  // TODO(builtin/dot): Source args[1] into the current shell environment.
  //   Open the file, read it line by line (or as a CompiledProgram), execute
  //   with executor.exec_string() in the current scope (no fork).
  //   Search $PATH if the filename has no slash (POSIX requirement).
  (void)args; return 0;
}
int builtin_eval(vec<str>& args, ExecContext&) {
  // TODO(builtin/eval): Join args[1..] with spaces, re-lex and re-parse,
  //   then execute in the current scope via executor.exec_string().
  //   EVAL_LINENO option: line number attributed to eval'd code.
  (void)args; return 0;
}
int builtin_exec(vec<str>& args, ExecContext&) {
  // TODO(builtin/exec): With args, replace the current process via execvp().
  //   Flags: -a name (set argv[0]), -c (clear environment), -l (login shell).
  //   Without args, just apply any redirections to the shell itself.
  (void)args; return 0;
}
int builtin_exit(vec<str>& args, ExecContext& ctx) {
  ctx.exiting  = true;
  ctx.exit_val = args.size() > 1 ? s::atoi(args[1].c_str()) : ctx.last_exit;
  return ctx.exit_val;
}
static int typeset_common(vec<str>& args, ExecContext& ctx,
                          strv implied);

// `export` is `typeset -gx`; `-n` takes the export attribute away.
// TODO(builtin/export): `-p` and the bare `export` listing (M7).
int builtin_export(vec<str>& args, ExecContext& ctx) {
  for (auto& a : args)
    if (a == "-n") a = "+x";
  return typeset_common(args, ctx, "gx");
}
// `readonly` is `typeset -gr`.
int builtin_readonly(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, "gr");
}
int builtin_return(vec<str>& args, ExecContext& ctx) {
  ctx.returning  = true;
  ctx.return_val = args.size() > 1 ? s::atoi(args[1].c_str()) : ctx.last_exit;
  return ctx.return_val;
}
int builtin_set(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/set): Handle two roles:
  //   `set [±o option] …` — same as setopt/unsetopt; also single-letter flags
  //     (-e = errexit, -x = xtrace, -n = noexec, -u = nounset, etc.)
  //   `set -- word …` — replace positional parameters $1 $2 … with the words.
  //   `set` with no args — print all shell variables (like typeset -p).
  (void)args; (void)ctx; return 0;
}
int builtin_shift(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/shift): Remove the first N positional parameters (default 1).
  //   $1 becomes the old $2, etc.  Error if N > $#.
  //   Also supports named array: `shift arrayname N`.
  (void)args; (void)ctx; return 0;
}
int builtin_times(vec<str>&, ExecContext&) {
  s::printf("0m0.000s 0m0.000s\n0m0.000s 0m0.000s\n"); return 0;
}
int builtin_unset(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/unset): Remove named variables and/or functions.
  //   -v → unset variable (default); -f → unset function.
  //   Error if the variable is readonly.
  //   For PM_SPECIAL params, call the unset handler rather than removing.
  (void)args; (void)ctx; return 0;
}
int builtin_cd(vec<str>& args, ExecContext&) {
  const char* dir = args.size() > 1 ? args[1].c_str() : getenv("HOME");
  return dir ? chdir(dir) : 1;
}
int builtin_pushd(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/pushd): Push directory onto the stack (stored in $dirstack).
  //   `pushd dir` → chdir(dir) and push old PWD.
  //   `pushd +N/-N` → rotate the stack.
  //   AUTOPUSHD: pushd called implicitly on every cd when option is set.
  //   PUSHD_MINUS, PUSHD_SILENT, PUSHD_TO_HOME options.
  (void)args; (void)ctx; return 0;
}
int builtin_popd(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/popd): Pop the top directory and chdir to it.
  (void)args; (void)ctx; return 0;
}
int builtin_dirs(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/dirs): Print the directory stack.
  //   -v: numbered, one per line; -l: expand ~ to full path; -p: one per line.
  (void)args; (void)ctx; return 0;
}
int builtin_pwd(vec<str>&, ExecContext&) {
  char buf[4096]; if (getcwd(buf, sizeof buf)) s::printf("%s\n", buf); return 0;
}
// Attributes typeset can give a parameter, from its option letters.
struct TypesetAttrs {
  char        type   = 0;   // 'a', 'A', 'i' or 'F'; 0 for scalar
  bool        global = false;
  s::uint32_t on     = 0;   // PM_EXPORTED, PM_READONLY, PM_UPPER, PM_LOWER
  s::uint32_t off    = 0;
};

static bool is_identifier(strv s) noexcept {
  if (s.empty() or s::isdigit(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s)
    if (not s::isalnum(static_cast<unsigned char>(c)) and c != '_') return false;
  return true;
}

// Declares or updates one `name` / `name=value` argument of typeset.
static int typeset_one(strv cmd, strv arg,
                       const TypesetAttrs& attrs, ExecContext& ctx) {
  s::size_t eq = arg.find('=');
  bool        has_value = eq != strv::npos;
  bool        append    = has_value and eq > 0 and arg[eq - 1] == '+';
  str name(arg.substr(0, has_value ? eq - (append ? 1 : 0) : arg.size()));
  str value = has_value ? str(arg.substr(eq + 1)) : str{};
  if (not is_identifier(name)) {
    s::fprintf(stderr, "jfsh: %.*s: not valid in this context: %s\n",
               static_cast<int>(cmd.size()), cmd.data(), name.c_str());
    return 1;
  }
  auto existing = attrs.global ? ctx.params.lookup(name)
                               : ctx.params.lookup_local(name);
  if (existing and existing->get().is_readonly()
      and (has_value or attrs.type or attrs.off)) {
    s::fprintf(stderr, "jfsh: read-only variable: %s\n", name.c_str());
    return 1;
  }
  Param* p = existing ? &existing->get() : nullptr;
  if (not p) {
    // A new local keeps the export attribute of the parameter it hides.
    auto outer = ctx.params.lookup(name);
    bool exported = outer and outer->get().is_exported();
    if (attrs.global) {
      (void)ctx.params.set_scalar(name, "");
      p = &ctx.params.lookup(name)->get();
    } else {
      p = &ctx.params.declare_local(name, PM_SCALAR);
      p->set(str{});
    }
    if (exported) p->flags |= PM_EXPORTED;
    if (attrs.type == 'i' and not has_value) p->set(s::int64_t{0});
    if (attrs.type == 'F' and not has_value) p->set(0.0);
  }
  if (append and has_value) value = p->scalar() + value;
  s::uint32_t case_flags = (p->flags | attrs.on) & ~attrs.off;
  if (case_flags & PM_UPPER)
    for (char& c : value) c = static_cast<char>(s::toupper(static_cast<unsigned char>(c)));
  if (case_flags & PM_LOWER)
    for (char& c : value) c = static_cast<char>(s::tolower(static_cast<unsigned char>(c)));
  switch (attrs.type) {
    case 'a':
      if (has_value) p->set(vec<str>{value});
      else if (not p->is_array()) p->set(vec<str>{});
      break; case 'A':
      if (not p->is_hash()) p->set(umap<str, str>{});
      break; case 'i': case 'F':
      if (has_value or (attrs.type == 'i' ? not p->is_integer() : not p->is_float())) {
        auto v = math_eval(has_value ? value : p->scalar(), ctx.params, math_options(ctx.opts));
        if (not v) {
          s::fprintf(stderr, "jfsh: %.*s: %s\n", static_cast<int>(cmd.size()),
                     cmd.data(), v.error().message.c_str());
          return 1;
        }
        if (attrs.type == 'i') p->set(v->as_int());
        else                   p->set(v->as_float());
      }
      break; default:
      if (has_value) {
        if (p->is_integer() or p->is_float()) {
          auto v = math_eval(value, ctx.params, math_options(ctx.opts));
          if (not v) {
            s::fprintf(stderr, "jfsh: %.*s: %s\n", static_cast<int>(cmd.size()),
                       cmd.data(), v.error().message.c_str());
            return 1;
          }
          if (p->is_integer()) p->set(v->as_int());
          else                 p->set(v->as_float());
        } else {
          p->set(s::move(value));
        }
      }
  }
  p->flags = (p->flags | attrs.on) & ~attrs.off;
  return 0;
}

// typeset [±aAFgilrux] [name[=value] …].  Without -g a name is declared in
// the innermost scope, which inside a function is local to it.  A new
// parameter is set and empty (0 for -i/-F, () for -a/-A).  `implied` holds
// the options the variant stands for (export is `typeset -gx`).
// TODO(builtin/typeset): listing (`typeset`, `-p`, `-m`), `-f`, `-h`,
//   `-T`, `-U`, `-Z`/`-L`/`-R` padding, and keeping -u/-l/-i in force for
//   later assignments (M7).
static int typeset_common(vec<str>& args, ExecContext& ctx,
                          strv implied) {
  strv cmd = args[0];
  TypesetAttrs attrs;
  auto apply = [&](char c, bool on) {
    switch (c) {
      case 'a': case 'A': case 'i': attrs.type = on ? c : 0; return true;
      case 'F': case 'E':
        attrs.type = on ? 'F' : 0;
        if (on) (c == 'E' ? attrs.on : attrs.off) |= PM_EFLOAT;
        return true;
      case 'g': attrs.global = on; return true;
      case 'x': (on ? attrs.on : attrs.off) |= PM_EXPORTED; return true;
      case 'r': (on ? attrs.on : attrs.off) |= PM_READONLY; return true;
      case 'u': (on ? attrs.on : attrs.off) |= PM_UPPER;    return true;
      case 'l': (on ? attrs.on : attrs.off) |= PM_LOWER;    return true;
      default:  return false;
    }
  };
  for (char c : implied) apply(c, true);
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; break; }
    if (a.size() < 2 or (a[0] != '-' and a[0] != '+')) break;
    for (s::size_t k = 1; k < a.size(); ++k) {
      if (not apply(a[k], a[0] == '-')) {
        s::fprintf(stderr, "jfsh: %.*s: bad option: %c%c\n",
                   static_cast<int>(cmd.size()), cmd.data(), a[0], a[k]);
        return 1;
      }
    }
  }
  if (cmd == "local") attrs.global = false;
  int rc = 0;
  for (; i < args.size(); ++i)
    if (typeset_one(cmd, args[i], attrs, ctx) != 0) rc = 1;
  return rc;
}

int builtin_typeset(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, "");
}
int builtin_local(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, "");
}
int builtin_declare(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, "");
}
int builtin_integer(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, "i");
}
int builtin_float(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, "F");
}
int builtin_fc(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/fc): History editor and re-execution.
  //   `fc -l [first [last]]` → list history entries.
  //   `fc -e editor [first [last]]` → open entries in $editor and re-execute.
  //   `fc -s [pat=rep] [event]` → re-execute with optional substitution.
  //   `fc -p [file [size [savesize]]]` → push history file (LOCAL_HISTORY).
  //   `fc -P` → pop pushed history.
  (void)args; (void)ctx; return 0;
}
int builtin_history(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/history): Alias for `fc -l`; prints numbered history list.
  (void)args; (void)ctx; return 0;
}
int builtin_r(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/r): Alias for `fc -s`; re-execute the last command.
  (void)args; (void)ctx; return 0;
}
int builtin_alias(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/alias): Manage the alias table (a HashTable<std::string>
  //   that needs to be added to ExecContext or a global AliasTable).
  //   `alias name=value` → register; `alias name` → print; `alias` → print all.
  //   -g: global alias (expanded anywhere in line, not just command position).
  //   -s: suffix alias (expanded when a file with that suffix is run directly).
  //   The lexer must expand aliases at token-read time (alias_expand_ flag).
  (void)args; (void)ctx; return 0;
}
int builtin_unalias(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/unalias): Remove named entries from the alias table.
  //   -a: remove all aliases; -m pattern: remove matching aliases.
  (void)args; (void)ctx; return 0;
}
int builtin_builtin   (vec<str>&, ExecContext&) { return 0; }
int builtin_command   (vec<str>&, ExecContext&) { return 0; }
int builtin_echo(vec<str>& args, ExecContext&) {
  for (s::size_t i = 1; i < args.size(); ++i) {
    if (i > 1) s::putchar(' ');
    s::fputs(args[i].c_str(), stdout);
  }
  s::putchar('\n'); return 0;
}
int builtin_print(vec<str>& args, ExecContext& ctx) {
  return builtin_echo(args, ctx);
}
int builtin_printf    (vec<str>&, ExecContext&) { return 0; }
// `read [-rA] [name …]`: reads one line from fd 0, a byte at a time so
// nothing past it is consumed.  The line is split on $IFS: each name but
// the last takes a field and the last takes the rest; with no name the
// whole line goes to $REPLY, and with -A the fields fill the array.
// Without -r a backslash quotes the next character, and before a newline
// joins the next line.  Status 1 at end of input.
// TODO(builtin/read): -s, -t, -n/-k, -p, -u, -d, -q, -e/-E, -z (M7).
int builtin_read(vec<str>& args, ExecContext& ctx) {
  bool raw = false;
  bool array = false;
  s::size_t i = 1;
  for (; i < args.size() and args[i].size() > 1 and args[i][0] == '-'; ++i) {
    for (char c : strv(args[i]).substr(1)) {
      if      (c == 'r') raw = true;
      else if (c == 'A') array = true;
      else {
        s::fprintf(stderr, "jfsh: read: bad option: -%c\n", c);
        return 1;
      }
    }
  }
  vec<str> names(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());

  // Characters quoted by a backslash are marked so they don't split.
  str line;
  vec<bool> quoted;
  bool eof = false;
  for (;;) {
    char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) { eof = true; break; }
    if (c == '\n') break;
    if (c == '\\' and not raw) {
      n = ::read(STDIN_FILENO, &c, 1);
      if (n <= 0) { eof = true; break; }
      if (c == '\n') continue;
      line += c;
      quoted.push_back(true);
      continue;
    }
    line += c;
    quoted.push_back(false);
  }
  if (eof and line.empty()) return 1;

  str ifs = " \t\n";
  if (auto p = ctx.params.lookup("IFS"); p and p->get().is_set())
    ifs = p->get().scalar();
  auto is_sep = [&](s::size_t k) {
    return not quoted[k] and ifs.find(line[k]) != str::npos;
  };
  auto is_white = [&](s::size_t k) {
    return is_sep(k) and (line[k] == ' ' or line[k] == '\t' or line[k] == '\n');
  };
  // Fields, plus where each starts so the last name can take the rest.
  vec<str> fields;
  vec<s::size_t> starts;
  s::size_t k = 0;
  while (k < line.size() and is_white(k)) ++k;
  while (k < line.size()) {
    starts.push_back(k);
    str field;
    while (k < line.size() and not is_sep(k)) field += line[k++];
    fields.push_back(s::move(field));
    while (k < line.size() and is_white(k)) ++k;
    if (k < line.size() and is_sep(k)) {
      ++k;
      while (k < line.size() and is_white(k)) ++k;
    }
  }

  auto set = [&](const str& name, str value) {
    if (auto r = ctx.params.set_scalar(name, s::move(value)); not r) {
      s::fprintf(stderr, "jfsh: read: %s\n", r.error().message.c_str());
      return false;
    }
    return true;
  };
  if (array) {
    str name = names.empty() ? "reply" : names[0];
    if (auto r = ctx.params.set_array(name, fields); not r) {
      s::fprintf(stderr, "jfsh: read: %s\n", r.error().message.c_str());
      return 1;
    }
    return eof ? 1 : 0;
  }
  if (names.empty()) names.push_back("REPLY");
  if (names.size() == 1 and names[0] == "REPLY" and i == args.size()) {
    if (not set("REPLY", line)) return 1;
    return eof ? 1 : 0;
  }
  for (s::size_t n = 0; n < names.size(); ++n) {
    str value;
    if (n + 1 == names.size() and n < fields.size()) {
      // The rest of the line, without trailing IFS whitespace.
      s::size_t end = line.size();
      while (end > starts[n] and is_white(end - 1)) --end;
      value = line.substr(starts[n], end - starts[n]);
    } else if (n < fields.size()) {
      value = fields[n];
    }
    if (not set(names[n], s::move(value))) return 1;
  }
  return eof ? 1 : 0;
}
int builtin_source(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/source): Same as builtin_dot.  Source the file; pass any
  //   extra arguments as $1 $2 … to the sourced script.
  (void)args; (void)ctx; return 0;
}
int builtin_hash(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/hash): Manage the command hash table (PATH lookup cache).
  //   `hash` → print table; `hash cmd=path` → add entry; `hash -d` → dir hash.
  //   The hash table needs to be added to ExecContext as HashTable<std::string>.
  (void)args; (void)ctx; return 0;
}
int builtin_rehash(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/rehash): Rebuild the command hash table by walking $PATH.
  //   HASH_CMDS option: auto-hash on first use rather than on startup.
  (void)args; (void)ctx; return 0;
}
int builtin_true      (vec<str>&, ExecContext&) { return 0; }
int builtin_false     (vec<str>&, ExecContext&) { return 1; }
// builtin_test (`test` / `[`) is in cond.cpp.
int builtin_ulimit(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/ulimit): Wrapper around getrlimit/setrlimit.
  //   -a: print all; -c -d -f -n -s -t -u -v -x: individual resource limits.
  //   Values: N (set hard+soft), `unlimited`, or Nh/Ns for hard/soft only.
  (void)args; (void)ctx; return 0;
}
int builtin_umask(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/umask): Get/set the file creation mask via umask(2).
  //   Accept octal or symbolic (u=rwx,g=rx,o=) notation.
  //   -S: print in symbolic form.
  (void)args; (void)ctx; return 0;
}
int builtin_zmodload(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/zmodload): Load/unload a module via ModuleManager::load().
  //   -a: autoload (load on first use); -u: unload; -l: list loaded modules.
  //   Pass the module name, search ModuleManager::search_paths().
  (void)args; (void)ctx; return 0;
}
int builtin_autoload(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/autoload): Mark a function name for lazy loading from $fpath.
  //   When the function is first called, search $fpath for a file named after
  //   it, source it, then invoke the now-defined function.
  //   -U: suppress alias expansion; -z/-k: zsh/ksh autoload style.
  (void)args; (void)ctx; return 0;
}
int builtin_functions(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/functions): Print function definitions using text.cpp decompiler.
  //   `functions name` → print one function; `functions` → print all.
  //   -u: print only undefined (autoload) functions; -m pattern: match.
  (void)args; (void)ctx; return 0;
}
int builtin_whence(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/whence): Report where a name is defined:
  //   alias, function, builtin, hashed command, or PATH search result.
  //   -c: print like `type`; -f: print function body; -v: verbose; -a: all.
  (void)args; (void)ctx; return 0;
}
int builtin_which(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/which): Like whence -c (command path only).
  //   Search PATH for each arg; print full path or "not found".
  (void)args; (void)ctx; return 0;
}
int builtin_type(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/type): POSIX-compat alias for `whence -v`.
  (void)args; (void)ctx; return 0;
}
int builtin_compinit(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/compinit): Bootstrap the completion system.
  //   Walk $fpath for files named _command, source them to populate the
  //   compdef table in CompletionEngine.
  //   -d dumpfile: cache compiled completions; -C: use cache without check.
  (void)args; (void)ctx; return 0;
}
int builtin_setopt(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/setopt): Set shell options by name.
  //   `setopt extendedglob` → opts.set(OPT_EXTENDEDGLOB)
  //   `setopt no_extendedglob` → opts.unset(OPT_EXTENDEDGLOB)
  //   `setopt` → print all currently set options.
  //   Use OptionState::find() for name lookup.
  (void)args; (void)ctx; return 0;
}
int builtin_unsetopt(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/unsetopt): Unset a shell option by name (inverse of setopt).
  //   `unsetopt globdots` → opts.unset(OPT_GLOBDOTS)
  (void)args; (void)ctx; return 0;
}
int builtin_bindkey(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/bindkey): Manage ZLE key bindings.
  //   `bindkey seq widget` → editor.bind_key(keymap, seq, widget)
  //   `bindkey -M keymap seq widget` → bind in named keymap
  //   `bindkey` → list all bindings; `bindkey seq` → show binding for seq.
  //   Keymap names: main, emacs, viins, vicmd, viopp, visual, menuselect.
  (void)args; (void)ctx; return 0;
}
int builtin_vared(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/vared): Edit a shell variable interactively with ZLE.
  //   Load the variable's current value into the line buffer, run the editor,
  //   then write the result back to the variable.
  (void)args; (void)ctx; return 0;
}
int builtin_zle(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/zle): ZLE widget management from within a widget function.
  //   `zle -N name [function]` → register a user-defined widget.
  //   `zle widget-name` → call a widget from within another widget.
  //   `zle -R` → trigger a display refresh.
  //   `zle -M msg` → display a message below the prompt.
  (void)args; (void)ctx; return 0;
}
int builtin_zpty(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/zpty): Run a command in a pseudo-terminal.
  //   `zpty name cmd` → fork cmd in a pty, store name for later I/O.
  //   `zpty -w name data` → write to the pty.
  //   `zpty -r name var` → read from the pty into var.
  //   `zpty -d name` → close the pty.
  (void)args; (void)ctx; return 0;
}
int builtin_sched(vec<str>& args, ExecContext& ctx) {
  // TODO(builtin/sched): Schedule a command to run after a delay.
  //   `sched +N:M cmd` → run cmd after N minutes M seconds.
  //   Implemented via SIGALRM or a checked event queue in the REPL loop.
  (void)args; (void)ctx; return 0;
}
// `let expr…`: evaluates each expression; the status is 0 when the last
// one is non-zero, 1 when it is zero, and 2 on an error.
int builtin_let(vec<str>& args, ExecContext& ctx) {
  MathNum last;
  for (s::size_t i = 1; i < args.size(); ++i) {
    auto v = math_eval(args[i], ctx.params, math_options(ctx.opts));
    if (not v) {
      s::fprintf(stderr, "jfsh: let: %s\n", v.error().message.c_str());
      return 2;
    }
    last = *v;
  }
  return last.truthy() ? 0 : 1;
}

}  // namespace jfsh
