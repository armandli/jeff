// Builtins that run code or end it: `.`, source, eval, exit, return,
// autoload and functions.

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

namespace {

// A file `.` can read: anything that exists but a directory (zsh's
// stat and !S_ISDIR), so a fifo or /dev/null too.  A path with a slash
// may even be a directory, which reads as empty.
bool can_source(const str& path, bool dir_ok) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 and (dir_ok or not S_ISDIR(st.st_mode));
}

// Where `.` (and `source`, which tries the current directory first) finds
// `name`: as given when it has a slash, otherwise along $PATH.
opt<str> find_sourced(const str& name, bool cwd_first, ExecContext& ctx) {
  if (name.find('/') != str::npos)
    return can_source(name, true) ? opt<str>{name} : s::nullopt;
  if (cwd_first and can_source(name, false)) return name;
  for (const auto& dir : path_dirs(ctx.params)) {
    str candidate = dir.empty() ? name : dir + "/" + name;
    if (can_source(candidate, false)) return candidate;
  }
  return s::nullopt;
}

int source_common(vec<str>& args, ExecContext& ctx, bool cwd_first) {
  strv cmd = args[0];
  if (args.size() < 2) return builtin_error(cmd, "not enough arguments");
  auto path = find_sourced(args[1], cwd_first, ctx);
  if (not path) {
    builtin_error(cmd, "no such file or directory: " + args[1]);
    return 127;
  }
  vec<str> rest(args.begin() + 2, args.end());
  return ctx.executor->source_file(*path, rest, args[1]);
}

// `exit`/`return`'s argument, an arithmetic expression; the last status
// without one.
opt<int> status_arg(vec<str>& args, ExecContext& ctx) {
  if (args.size() > 2) {
    builtin_error(args[0], "too many arguments");
    return s::nullopt;
  }
  if (args.size() < 2) return ctx.last_exit;
  auto v = arith_eval(args[1], ctx.params, math_options(ctx),
                      &ctx.modules.math_funcs());
  if (not v) {
    builtin_error(args[0], v.error().message);
    return s::nullopt;
  }
  return static_cast<int>(*v);
}

// What `functions` prints for an autoload function not yet loaded.
str autoload_stub(const ExecContext& ctx, const str& name) {
  return name + " () {\n\t# undefined\n\t" + autoload_stub_body(ctx, name) + "\n}";
}

}  // namespace

int builtin_dot(vec<str>& args, ExecContext& ctx)    { return source_common(args, ctx, false); }
int builtin_source(vec<str>& args, ExecContext& ctx) { return source_common(args, ctx, true); }

// eval arg …: the arguments joined with spaces, run in this shell.
int builtin_eval(vec<str>& args, ExecContext& ctx) {
  str text;
  // A first `--` is dropped, as zsh's BINF_DASHDASHVALID.
  s::size_t first = args.size() > 1 and args[1] == "--" ? 2 : 1;
  for (s::size_t i = first; i < args.size(); ++i) {
    if (i > first) text += ' ';
    text += args[i];
  }
  if (text.find_first_not_of(" \t\n") == str::npos) return 0;
  FuncFrame frame{ctx, "(eval)"};
  ++ctx.code_runs;
  bool hard = s::exchange(ctx.hard_error, false);
  int rc = ctx.executor->exec_string(text);
  // As zsh's eval, an error ends just the evaluated text; an interrupt,
  // an exit and `${x?}` go on.  A TRAPxxx's failure is an error too, but
  // its return (zsh's retflag) stays: the function the eval is in returns
  // 1, and at the top of a script just the line ends.
  if (ctx.aborting and ctx.trap_failed) {
    ctx.aborting = ctx.abort_line = false;
    ctx.end_trap_abort();
    rc = 1;
    ctx.returning   = true;
    ctx.return_val  = rc;
    ctx.return_line = true;
  } else if (ctx.aborting and not ctx.interrupt and not ctx.exiting and not ctx.hard_error) {
    ctx.aborting = ctx.abort_line = false;
    ctx.end_trap_abort();
    if (rc == 0) rc = 1;
  }
  ctx.hard_error = ctx.hard_error or hard;
  return rc;
}

// `exec` is handled by the executor as a precommand modifier; this runs
// only for `builtin exec`, which zsh treats the same way.
int builtin_exec(vec<str>& args, ExecContext& ctx) {
  vec<str> rest(args.begin() + 1, args.end());
  if (rest.empty()) return 0;
  str text = "exec";
  for (const auto& a : rest) text += " " + shell_quote(a);
  return ctx.executor->exec_string(text);
}

// exit [n]: the status is n (an arithmetic expression), taken mod 256 by
// the system, or $? without one.  The EXIT trap runs as the shell exits.
int builtin_exit(vec<str>& args, ExecContext& ctx) {
  auto n = status_arg(args, ctx);
  if (not n) return 1;
  ctx.exiting  = true;
  ctx.exit_val = *n & 0xff;
  // Outside a function zsh exits at once; in one, the exit unwinds it.
  ctx.exit_now = ctx.func_depth == 0;
  return ctx.exit_val;
}

// return [n]: leaves the function or sourced file (outside both, the
// script).
int builtin_return(vec<str>& args, ExecContext& ctx) {
  auto n = status_arg(args, ctx);
  if (not n) return 1;
  ctx.returning  = true;
  ctx.return_val = *n;
  return ctx.return_val;
}

// autoload [-UXkmtz] [+X] [name …]: marks functions to be loaded from
// $fpath when first called.  `autoload /dir/name` loads from that file.
// With no names, lists the functions still to be loaded.  +X loads them
// now; -X, in a function, loads that function and runs it with its
// arguments, as zsh's eval_autoload.
int builtin_autoload(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  if (not parse_options(args, "UXdkmrRtTwz", "", o, true)) return 1;
  // zsh's bin_functions refuses -k with -z.
  if ((o.has('k') or o.has_plus('k')) and (o.has('z') or o.has_plus('z')))
    return builtin_error(args[0], "invalid option(s)");
  // As zsh's bin_functions: the flags given are added to (or with `+`
  // taken from) those a name already has, -z and -k each clearing the
  // other.
  str on, off;
  for (char c : strv("UtTkz")) {
    if (o.has(c)) on += c;
    if (o.has_plus(c)) off += c;
  }
  if (o.has('z')) off += 'k';
  if (o.has('k')) off += 'z';
  // -d (zsh's PM_CUR_FPATH) shows as `c`.
  if (o.has('d')) on += 'c';
  if (o.has_plus('d')) off += 'c';
  if (o.has('X')) {
    // As zsh's bin_functions: one argument is the directory to load from.
    if (args.size() > o.next + 1) return builtin_error(args[0], "-X: too many arguments");
    // The innermost function being run (not an eval or a sourced file).
    str name;
    for (auto it = ctx.funcstack.rbegin(); it != ctx.funcstack.rend(); ++it)
      if (ctx.functions.contains(it->name)) {
        name = it->name;
        break;
      }
    if (name.empty()) {
      // zsh's zerrnam: the command line ends.
      ctx.aborting = true;
      return builtin_error(args[0], "bad autoload");
    }
    // Its file is looked for along $fpath (or in the directory given), not
    // where it was defined.
    if (o.next < args.size()) ctx.function_files[name] = args[o.next] + "/" + name;
    else                      ctx.function_files.erase(name);
    // The flags are the ones given here (zsh: `shf->node.flags = on`).
    ctx.autoloads[name] = on;
    vec<str> fargs{name};
    fargs.insert(fargs.end(), ctx.positional.begin(), ctx.positional.end());
    if (not ctx.executor->load_autoload(name)) {
      // As zsh's: the function is left undefined, to be looked for again.
      ctx.functions.erase(name);
      return 1;
    }
    ProgRef body = ctx.functions[name];
    return ctx.executor->call_function(body, fargs);
  }
  if (o.next >= args.size()) {
    vec<str> names;
    for (const auto& [n, _] : ctx.autoloads) names.push_back(n);
    s::sort(names.begin(), names.end());
    for (const auto& n : names) s::printf("%s\n", autoload_stub(ctx, n).c_str());
    return 0;
  }
  int rc = 0;
  for (s::size_t i = o.next; i < args.size(); ++i) {
    str name = args[i];
    str path;
    if (auto slash = name.rfind('/'); slash != str::npos) {
      path = name;
      name = name.substr(slash + 1);
    }
    if (name.empty() or ctx.functions.contains(name)) continue;
    str& flags = ctx.autoloads[name];
    str merged;
    for (char c : strv("UtTkzc"))
      if ((flags.find(c) != str::npos or on.find(c) != str::npos) and off.find(c) == str::npos)
        merged += c;
    flags = s::move(merged);
    if (not path.empty()) ctx.function_files[name] = path;
    if (o.has_plus('X') and not ctx.executor->load_autoload(name)) rc = 1;
  }
  return rc;
}

// functions [-mu+] [name …]: prints function definitions (all, the named
// ones, or with -m those matching patterns); `+` prints only names; -u is
// autoload.
int builtin_functions(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  bool names_only = false;
  vec<str> a2;
  for (const auto& a : args) {
    if (a == "+") names_only = true;
    else          a2.push_back(a);
  }
  if (not parse_options(a2, "mukzU", "x", o, true)) return 1;
  // -x N: N spaces for each level of indentation (0: none), as zsh's
  // printshfuncexpand.
  opt<s::size_t> indent;
  if (auto* x = o.arg('x')) {
    char* end = nullptr;
    long n = s::strtol(x->c_str(), &end, 10);
    if (x->empty() or *end != '\0' or n < 0)
      return builtin_error(a2[0], "number expected after -x");
    indent = static_cast<s::size_t>(n);
  }
  auto expand = [&](str text) {
    if (not indent) return text;
    str out;
    bool start = true;
    for (char c : text) {
      if (start and c == '\t') {
        out.append(*indent, ' ');
        continue;
      }
      start = c == '\n';
      out += c;
    }
    return out;
  };
  // -u (and -U, zsh's PM_UNALIASED|PM_UNDEFINED) is autoload, with the
  // flags given.
  if (o.has('u') or o.has('U')) {
    vec<str> al{"autoload"};
    for (char c : strv("Ukz"))
      if (o.has(c)) al.push_back(str("-") + c);
    al.insert(al.end(), a2.begin() + static_cast<s::ptrdiff_t>(o.next), a2.end());
    return builtin_autoload(al, ctx);
  }
  vec<str> requested(a2.begin() + static_cast<s::ptrdiff_t>(o.next), a2.end());
  // Disabled ones aren't listed, nor found (zsh's DISABLED nodes).
  vec<str> all;
  for (const auto& [name, body] : ctx.functions)
    if (not ctx.disabled_functions.contains(name)) all.push_back(name);
  for (const auto& [name, _] : ctx.autoloads)
    if (not ctx.disabled_functions.contains(name)) all.push_back(name);
  s::sort(all.begin(), all.end());
  all.erase(s::unique(all.begin(), all.end()), all.end());
  auto print = [&](const str& name) {
    if (names_only) {
      s::printf("%s\n", name.c_str());
      return;
    }
    if (auto fn = ctx.functions.find(name); fn != ctx.functions.end())
      s::printf("%s\n", expand(function_text(name, *fn->second)).c_str());
    else
      s::printf("%s\n", expand(autoload_stub(ctx, name)).c_str());
  };
  if (requested.empty()) {
    for (const auto& n : all) print(n);
    return 0;
  }
  int rc = 0;
  for (const auto& r : requested) {
    if (o.has('m')) {
      auto pat = CompiledPattern::compile(r);
      bool any = false;
      for (const auto& n : all)
        if (pat.matches(n)) { print(n); any = true; }
      if (not any) rc = 1;
      continue;
    }
    if (s::binary_search(all.begin(), all.end(), r)) print(r);
    else rc = 1;
  }
  return rc;
}

// enable/disable [-afmprs] [name …]: builtins (the default) and, with -f,
// shell functions are skipped by command lookup while disabled; -m takes
// patterns.  With no names, the disabled (enabled) ones are listed.
// Pattern characters (-p) are always on in jfsh, so `enable -p` does
// nothing and `disable -p` with names fails; aliases (-a, -s) and
// reserved words (-r) can't be disabled yet.
static int enable_common(vec<str>& args, ExecContext& ctx, bool on) {
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "afmprs", "", o)) return 1;
  vec<str> names(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  if (o.has('p') or o.has('a') or o.has('s') or o.has('r')) {
    if (on or names.empty()) return 0;
    return builtin_error(cmd, "only builtins and functions can be disabled");
  }
  bool funcs = o.has('f');
  auto& set = funcs ? ctx.disabled_functions : ctx.disabled_builtins;
  vec<str> all;
  if (funcs) {
    for (const auto& [name, body] : ctx.functions) all.push_back(name);
    for (const auto& [name, _] : ctx.autoloads) all.push_back(name);
  } else {
    all = ctx.executor->builtin_names();
  }
  s::sort(all.begin(), all.end());
  if (names.empty()) {
    for (const auto& n : all)
      if (set.contains(n) != on) s::printf("%s\n", n.c_str());
    return 0;
  }
  int rc = 0;
  for (const auto& name : names) {
    vec<str> hits;
    if (o.has('m')) {
      auto pat = CompiledPattern::compile(name);
      for (const auto& n : all)
        if (pat.matches(n)) hits.push_back(n);
    } else if (s::binary_search(all.begin(), all.end(), name)) {
      hits.push_back(name);
    } else {
      s::fprintf(stderr, "jfsh: %.*s: no such hash table element: %s\n",
                 static_cast<int>(cmd.size()), cmd.data(), name.c_str());
      rc = 1;
    }
    for (const auto& h : hits) {
      if (on) set.erase(h);
      else    set.insert(h);
    }
  }
  return rc;
}

int builtin_enable(vec<str>& args, ExecContext& ctx)  { return enable_common(args, ctx, true); }
int builtin_disable(vec<str>& args, ExecContext& ctx) { return enable_common(args, ctx, false); }

}  // namespace jfsh
