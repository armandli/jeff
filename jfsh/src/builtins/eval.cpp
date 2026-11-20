// Builtins that run code or end it: `.`, source, eval, exit, return,
// autoload and functions.

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

namespace {

bool is_regular_file(const str& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 and S_ISREG(st.st_mode);
}

// Where `.` (and `source`, which tries the current directory first) finds
// `name`: as given when it has a slash, otherwise along $PATH.
opt<str> find_sourced(const str& name, bool cwd_first, ExecContext& ctx) {
  if (name.find('/') != str::npos)
    return is_regular_file(name) ? opt<str>{name} : s::nullopt;
  if (cwd_first and is_regular_file(name)) return name;
  for (const auto& dir : path_dirs(ctx.params)) {
    str candidate = dir.empty() ? name : dir + "/" + name;
    if (is_regular_file(candidate)) return candidate;
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
  auto v = arith_eval(args[1], ctx.params, math_options(ctx.opts),
                      &ctx.modules.math_funcs());
  if (not v) {
    builtin_error(args[0], v.error().message);
    return s::nullopt;
  }
  return static_cast<int>(*v);
}

// What `functions` prints for an autoload function not yet loaded.
str autoload_stub(const str& name) {
  return name + " () {\n\t# undefined\n\tbuiltin autoload -XU\n}";
}

}  // namespace

int builtin_dot(vec<str>& args, ExecContext& ctx)    { return source_common(args, ctx, false); }
int builtin_source(vec<str>& args, ExecContext& ctx) { return source_common(args, ctx, true); }

// eval arg …: the arguments joined with spaces, run in this shell.
int builtin_eval(vec<str>& args, ExecContext& ctx) {
  str text;
  for (s::size_t i = 1; i < args.size(); ++i) {
    if (i > 1) text += ' ';
    text += args[i];
  }
  if (text.find_first_not_of(" \t\n") == str::npos) return 0;
  return ctx.executor->exec_string(text);
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

// autoload [-UXkmtz] [name …]: marks functions to be loaded from $fpath
// when first called.  `autoload /dir/name` loads from that file.  With no
// names, lists the functions still to be loaded.
int builtin_autoload(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  if (not parse_options(args, "UXdkmrRtTwz", "", o, true)) return 1;
  if (o.has('X')) return builtin_error(args[0], "-X: not implemented");
  if (o.next >= args.size()) {
    vec<str> names(ctx.autoloads.begin(), ctx.autoloads.end());
    s::sort(names.begin(), names.end());
    for (const auto& n : names) s::printf("%s\n", autoload_stub(n).c_str());
    return 0;
  }
  for (s::size_t i = o.next; i < args.size(); ++i) {
    str name = args[i];
    str path;
    if (auto slash = name.rfind('/'); slash != str::npos) {
      path = name;
      name = name.substr(slash + 1);
    }
    if (name.empty() or ctx.functions.contains(name)) continue;
    ctx.autoloads.insert(name);
    if (not path.empty()) ctx.function_files[name] = path;
  }
  return 0;
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
  if (not parse_options(a2, "mux", "", o, true)) return 1;
  if (o.has('u')) {
    vec<str> al{"autoload"};
    al.insert(al.end(), a2.begin() + static_cast<s::ptrdiff_t>(o.next), a2.end());
    return builtin_autoload(al, ctx);
  }
  vec<str> requested(a2.begin() + static_cast<s::ptrdiff_t>(o.next), a2.end());
  vec<str> all;
  for (const auto& [name, body] : ctx.functions) all.push_back(name);
  for (const auto& name : ctx.autoloads) all.push_back(name);
  s::sort(all.begin(), all.end());
  all.erase(s::unique(all.begin(), all.end()), all.end());
  auto print = [&](const str& name) {
    if (names_only) {
      s::printf("%s\n", name.c_str());
      return;
    }
    if (auto fn = ctx.functions.find(name); fn != ctx.functions.end())
      s::printf("%s\n", function_text(name, *fn->second).c_str());
    else
      s::printf("%s\n", autoload_stub(name).c_str());
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
    if (ctx.functions.contains(r) or ctx.autoloads.contains(r)) print(r);
    else rc = 1;
  }
  return rc;
}

}  // namespace jfsh
