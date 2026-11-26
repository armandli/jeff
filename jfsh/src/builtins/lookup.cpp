// Builtins that look commands up or name them: whence, which, type,
// command -v/-V, builtin, hash, rehash, alias, unalias.

#include <cstdio>
#include <cstdlib>
#include <climits>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

namespace {

// zsh's reswdtab (hashtable.c): `in` and `always` aren't in it, the
// typeset family is.
constexpr strv RESERVED[] = {
  "!", "[[", "{", "}", "case", "coproc", "declare", "do", "done", "elif", "else", "end",
  "esac", "export", "fi", "float", "for", "foreach", "function", "if", "integer", "local",
  "nocorrect", "readonly", "repeat", "select", "then", "time", "typeset", "until", "while",
};

bool is_reserved(strv name) {
  return s::find(s::begin(RESERVED), s::end(RESERVED), name) != s::end(RESERVED);
}

bool is_executable(const str& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 and S_ISREG(st.st_mode)
     and ::access(path.c_str(), X_OK) == 0;
}

// Every executable called `name` along $PATH, in order.
vec<str> all_in_path(const ExecContext& ctx, const str& name) {
  vec<str> out;
  if (name.find('/') != str::npos) {
    if (is_executable(name)) out.push_back(name);
    return out;
  }
  for (const auto& dir : path_dirs(ctx.params)) {
    // An empty component is the current directory, named as zsh does.
    // A directory listed twice is searched twice, as in zsh.
    str candidate = dir.empty() ? name : dir + "/" + name;
    if (is_executable(candidate)) out.push_back(candidate);
  }
  return out;
}

// One step of zsh's xsymlinks: the path with its first symbolic link
// replaced by what it points to (`.` and `..` taken as written before
// it), or nothing when no component is a link.
opt<str> expand_one_link(const str& path) {
  vec<str> parts;
  for (s::size_t i = 0; i <= path.size();) {
    s::size_t j = path.find('/', i);
    if (j == str::npos) j = path.size();
    if (j > i) parts.push_back(path.substr(i, j - i));
    i = j + 1;
  }
  str done;
  for (s::size_t k = 0; k < parts.size(); ++k) {
    if (parts[k] == ".") continue;
    if (parts[k] == "..") {
      if (auto slash = done.rfind('/'); slash != str::npos) done.resize(slash);
      continue;
    }
    str next = done + "/" + parts[k];
    char buf[PATH_MAX];
    ssize_t n = ::readlink(next.c_str(), buf, sizeof buf);
    if (n < 0) {
      done = s::move(next);
      continue;
    }
    str target(buf, static_cast<s::size_t>(n));
    str out = target[0] == '/' ? target : done + "/" + target;
    for (++k; k < parts.size(); ++k) out += "/" + parts[k];
    return out;
  }
  return s::nullopt;
}

// What whence -s / -S add to an absolute path, as zsh's print_if_link:
// -s the real path, -S each step through the links.
str link_suffix(const str& path, bool steps) {
  if (path.empty() or path[0] != '/') return {};
  if (not steps) {
    char buf[PATH_MAX];
    return ::realpath(path.c_str(), buf) ? " -> " + str(buf) : str{};
  }
  str out;
  str at = path;
  // Bounded, as a loop of links would go round for ever.
  for (int n = 0; n < 64; ++n) {
    auto next = expand_one_link(at);
    if (not next) break;
    out += " -> " + (next->empty() ? str("/") : *next);
    at = s::move(*next);
  }
  return out;
}

enum class Style {
  Plain,    // whence: the text, name or path
  Verbose,  // -v, type: `x is …`
  Csh,      // -c, which: `x: aliased to …`
  Word,     // -w: `x: alias`
};

// What whence_one looks at: everything, or (for -m, a table at a time)
// one kind.
enum Kinds : unsigned {
  K_ALIAS = 1, K_RESERVED = 2, K_FUNCTION = 4, K_BUILTIN = 8, K_COMMAND = 16, K_ALL = 31,
};

struct WhenceOpts {
  Style style = Style::Plain;
  unsigned kinds = K_ALL;
  bool  all = false;          // -a
  bool  path_only = false;    // -p
  bool  func_body = false;    // -f, or -c/-v for functions in csh style
  bool  symlinks = false;     // -s / -S
  bool  link_steps = false;   // -S
};

// Describes `name`; false when it is nothing at all.
bool whence_one(const str& name, const WhenceOpts& w, ExecContext& ctx) {
  bool found = false;
  auto say = [&](const str& plain, const str& verbose, const str& csh, const str& word) {
    found = true;
    switch (w.style) {
      case Style::Plain:   s::printf("%s\n", plain.c_str());   break;
      case Style::Verbose: s::printf("%s\n", verbose.c_str()); break;
      case Style::Csh:     s::printf("%s\n", csh.c_str());     break;
      case Style::Word:    s::printf("%s: %s\n", name.c_str(), word.c_str()); break;
    }
  };
  if (not w.path_only) {
    if (auto a = ctx.aliases.regular.find(name);
        (w.kinds & K_ALIAS) and a != ctx.aliases.regular.end()) {
      const str& text = a->second.text;
      say(text,
          name + " is " + (a->second.global ? "a global alias" : "an alias") + " for " + text,
          name + ": " + (a->second.global ? "globally " : "") + "aliased to " + text,
          "alias");
      if (not w.all) return true;
    }
    if ((w.kinds & K_RESERVED) and is_reserved(name)) {
      say(name, name + " is a reserved word", name + ": shell reserved word", "reserved");
      if (not w.all) return true;
    }
    // A disabled function or builtin isn't there for whence (zsh's
    // getnode skips DISABLED nodes).
    auto fn = ctx.functions.find(name);
    if ((w.kinds & K_FUNCTION) and not ctx.disabled_functions.contains(name)
        and (fn != ctx.functions.end() or ctx.autoloads.contains(name))) {
      str file;
      if (auto f = ctx.function_files.find(name); f != ctx.function_files.end()) file = f->second;
      str def = fn != ctx.functions.end() ? function_text(name, *fn->second)
                                          : name + " () {\n\t# undefined\n\t"
                                                + autoload_stub_body(ctx, name) + "\n}";
      str verbose = fn != ctx.functions.end()
                  ? name + " is a shell function" + (file.empty() ? str{} : " from " + file)
                  : name + " is an autoload shell function";
      // -f shows the definition alone, -v or not (zsh's printshfuncexpand).
      if (w.func_body and w.style == Style::Verbose) verbose = def;
      say(w.func_body ? def : name, verbose, def, "function");
      if (not w.all) return true;
    }
    if ((w.kinds & K_BUILTIN) and not ctx.disabled_builtins.contains(name)
        and ctx.executor->is_builtin(name)) {
      say(name, name + " is a shell builtin", name + ": shell built-in command", "builtin");
      if (not w.all) return true;
    }
  }
  vec<str> paths;
  if (w.kinds & K_COMMAND) {
    if (w.all) paths = all_in_path(ctx, name);
    else if (auto p = ctx.executor->find_command(name); p and is_executable(*p))
      paths.push_back(*p);
  }
  for (const auto& p : paths) {
    str shown = p;
    if (w.symlinks) shown = p + link_suffix(p, w.link_steps);
    say(shown, name + " is " + shown, shown, "command");
  }
  if (not found) {
    if (w.style == Style::Verbose or w.style == Style::Csh) s::printf("%s not found\n", name.c_str());
    else if (w.style == Style::Word) s::printf("%s: none\n", name.c_str());
  }
  return found;
}

int whence_common(vec<str>& args, ExecContext& ctx, WhenceOpts w) {
  OptParse o;
  if (not parse_options(args, "acfmpsSvwxM", "", o)) return 1;
  if (o.has('v')) w.style = Style::Verbose;
  if (o.has('c')) { w.style = Style::Csh; w.func_body = true; }
  if (o.has('w')) w.style = Style::Word;
  if (o.has('a')) w.all = true;
  if (o.has('p')) w.path_only = true;
  if (o.has('f')) w.func_body = true;
  if (o.has('s') or o.has('S')) w.symlinks = true;
  w.link_steps = o.has('S');
  if (o.next >= args.size()) return 1;
  int rc = 0;
  for (s::size_t i = o.next; i < args.size(); ++i) {
    if (o.has('m')) {
      // -m: patterns over each table in turn, as zsh's bin_whence:
      // aliases, reserved words, functions, builtins, then the commands.
      auto pat = CompiledPattern::compile(args[i]);
      bool any = false;
      auto scan = [&](unsigned kind, vec<str> names) {
        s::sort(names.begin(), names.end());
        WhenceOpts one = w;
        one.kinds = kind;
        one.all = false;
        for (const auto& n : names)
          if (pat.matches(n)) any = whence_one(n, one, ctx) or any;
      };
      if (not w.path_only) {
        vec<str> names;
        for (const auto& [n, _] : ctx.aliases.regular) names.push_back(n);
        scan(K_ALIAS, s::move(names));
        scan(K_RESERVED, vec<str>(s::begin(RESERVED), s::end(RESERVED)));
        names.clear();
        for (const auto& [n, _] : ctx.functions) names.push_back(n);
        for (const auto& [n, _] : ctx.autoloads) names.push_back(n);
        scan(K_FUNCTION, s::move(names));
        scan(K_BUILTIN, ctx.executor->builtin_names());
      }
      ctx.executor->fill_command_hash();
      vec<str> names;
      for (const auto& [n, _] : ctx.executor->command_hash()) names.push_back(n);
      scan(K_COMMAND, s::move(names));
      if (not any) rc = 1;
      continue;
    }
    if (not whence_one(args[i], w, ctx)) rc = 1;
  }
  return rc;
}

void print_alias(const str& name, const str& text, char kind, bool as_command) {
  if (as_command) {
    str flag = kind == 'g' ? " -g" : kind == 's' ? " -s" : "";
    s::printf("alias%s %s=%s\n", flag.c_str(), shell_quote(name).c_str(), shell_quote(text).c_str());
  } else {
    s::printf("%s=%s\n", shell_quote(name).c_str(), shell_quote(text).c_str());
  }
}

}  // namespace

int builtin_whence(vec<str>& args, ExecContext& ctx) { return whence_common(args, ctx, {}); }

// where: whence -ca, as zsh's table entry has it.
int builtin_where(vec<str>& args, ExecContext& ctx) {
  args.insert(args.begin() + 1, "-ca");
  return builtin_whence(args, ctx);
}

// ttyctl [-fu]: zsh freezes the terminal's settings with -f and lets them
// go with -u (-f wins), and says which without options.  jfsh's line editor
// sets the terminal up for each line itself, so only the flag is kept.
int builtin_ttyctl(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  if (zsh_options(args, "fu", 0, 0, ops) < 0) return 1;
  if (ops.isset('f'))      ctx.tty_frozen = true;
  else if (ops.isset('u')) ctx.tty_frozen = false;
  else                     s::printf("tty is %sfrozen\n", ctx.tty_frozen ? "" : "not ");
  return 0;
}

int builtin_type(vec<str>& args, ExecContext& ctx) {
  WhenceOpts w;
  w.style = Style::Verbose;
  return whence_common(args, ctx, w);
}

int builtin_which(vec<str>& args, ExecContext& ctx) {
  WhenceOpts w;
  w.style = Style::Csh;
  w.func_body = true;
  return whence_common(args, ctx, w);
}

// `command -v name` / `command -V name` (the executor runs `command cmd`
// itself, bypassing functions and builtins).
int builtin_command(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  if (not parse_options(args, "pvV", "", o)) return 1;
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  if (rest.empty()) return o.has('v') or o.has('V') ? 1 : 0;
  if (o.has('v') or o.has('V')) {
    WhenceOpts w;
    if (o.has('V')) w.style = Style::Verbose;
    int rc = 0;
    for (const auto& n : rest) {
      // `command -v` shows an alias as the command that defines it.
      if (w.style == Style::Plain)
        if (auto a = ctx.aliases.regular.find(n); a != ctx.aliases.regular.end()) {
          print_alias(n, a->second.text, a->second.global ? 'g' : 0, true);
          continue;
        }
      if (not whence_one(n, w, ctx)) rc = 1;
    }
    return rc;
  }
  return ctx.executor->run_command(rest);
}

// `builtin name args`: the executor runs it; alone it does nothing.
int builtin_builtin(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return 0;
  vec<str> rest(args.begin() + 1, args.end());
  if (not ctx.executor->is_builtin(rest[0])) {
    s::fprintf(stderr, "jfsh: no such builtin: %s\n", rest[0].c_str());
    return 1;
  }
  return ctx.executor->call_builtin(rest[0], rest, ctx);
}

// hash [-dfLmrv] [name[=path] …]: shows or edits the command hash table.
int builtin_hash(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "dfLmrv", "", o)) return 1;
  if (o.has('d')) return builtin_error(cmd, "-d: named directories are not implemented");
  auto& table = ctx.executor->command_hash();
  if (o.has('r')) table.clear();
  if (o.has('f')) ctx.executor->fill_command_hash();
  auto list = [&](const func<bool(const str&)>& want) {
    vec<str> names;
    for (const auto& [n, _] : ctx.executor->command_hash())
      if (want(n)) names.push_back(n);
    s::sort(names.begin(), names.end());
    for (const auto& n : names) {
      const str& path = ctx.executor->command_hash()[n];
      if (o.has('L')) s::printf("hash %s=%s\n", shell_quote(n).c_str(), shell_quote(path).c_str());
      else            s::printf("%s=%s\n", shell_quote(n).c_str(), shell_quote(path).c_str());
    }
  };
  if (o.next >= args.size()) {
    if (not o.has('r') and not o.has('f')) list([](const str&) { return true; });
    return 0;
  }
  int rc = 0;
  for (s::size_t i = o.next; i < args.size(); ++i) {
    const str& a = args[i];
    if (o.has('m')) {
      auto pat = CompiledPattern::compile(a);
      list([&](const str& n) { return pat.matches(n); });
      continue;
    }
    if (auto eq = a.find('='); eq != str::npos) {
      ctx.executor->command_hash()[a.substr(0, eq)] = a.substr(eq + 1);
      continue;
    }
    auto paths = all_in_path(ctx, a);
    if (paths.empty() or a.find('/') != str::npos) {
      builtin_error(cmd, "no such command: " + a);
      rc = 1;
      continue;
    }
    ctx.executor->command_hash()[a] = paths.front();
    if (o.has('v')) s::printf("%s=%s\n", a.c_str(), paths.front().c_str());
  }
  return rc;
}

// rehash: forgets the hashed command paths; they are looked up again.
int builtin_rehash(vec<str>& args, ExecContext& ctx) {
  (void)args;
  ctx.executor->command_hash().clear();
  return 0;
}

// alias [-gLmrs] [name[=value] …]: defines aliases (-g global, -s suffix)
// or lists them, as zsh's bin_alias: -L lists them as commands (and with
// `name=value` just looks name up), -r/-g/-s pick one kind.
int builtin_alias(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "gLmrs", "", o, true)) return 1;
  // +g, +r, +s, +m and a lone + list the names only (zsh's PRINT_NAMEONLY).
  bool names_only = o.has_plus('g') or o.has_plus('r') or o.has_plus('s') or o.has_plus('m');
  if (o.next < args.size() and args[o.next] == "+") {
    names_only = true;
    ++o.next;
  }
  bool global = o.has('g') or o.has_plus('g'), suffix = o.has('s') or o.has_plus('s');
  bool regular_only = o.has('r') or o.has_plus('r');
  if (static_cast<int>(global) + static_cast<int>(suffix) + static_cast<int>(regular_only) > 1)
    return builtin_error(cmd, "illegal combination of options");
  bool as_command = o.has('L') or o.has_plus('L');
  auto& regular = ctx.aliases.regular;
  auto& suffixes = ctx.aliases.suffix;
  auto list = [&](const func<bool(const str&)>& want) {
    if (suffix) {
      vec<str> names;
      for (const auto& [n, _] : suffixes)
        if (want(n)) names.push_back(n);
      s::sort(names.begin(), names.end());
      for (const auto& n : names) {
        if (names_only) s::printf("%s\n", n.c_str());
        else            print_alias(n, suffixes[n], 's', as_command);
      }
      return;
    }
    vec<str> names;
    for (const auto& [n, a] : regular) {
      if (global and not a.global) continue;
      if (regular_only and a.global) continue;
      if (want(n)) names.push_back(n);
    }
    s::sort(names.begin(), names.end());
    for (const auto& n : names) {
      if (names_only) s::printf("%s\n", n.c_str());
      else            print_alias(n, regular[n].text, regular[n].global ? 'g' : 0, as_command);
    }
  };
  if (o.next >= args.size()) {
    list([](const str&) { return true; });
    return 0;
  }
  if (o.has('m') or o.has_plus('m')) {
    for (s::size_t i = o.next; i < args.size(); ++i) {
      auto pat = CompiledPattern::compile(args[i]);
      list([&](const str& n) { return pat.matches(n); });
    }
    return 0;
  }
  int rc = 0;
  for (s::size_t i = o.next; i < args.size(); ++i) {
    const str& a = args[i];
    auto eq = a.find('=');
    // `=x`: zsh's getasg has no name to give (a zerr: the line ends).
    if (eq == 0) {
      s::fprintf(stderr, "jfsh: bad assignment\n");
      ctx.aborting = true;
      return rc;
    }
    str name = a.substr(0, eq);
    if (eq != str::npos and not as_command) {
      str text = a.substr(eq + 1);
      if (suffix) suffixes[name] = s::move(text);
      else        regular[name] = AliasDef{s::move(text), global};
      continue;
    }
    // `alias name`: show it, if it is of the kind asked for.
    if (suffix) {
      if (auto it = suffixes.find(name); it != suffixes.end())
        print_alias(name, it->second, 's', as_command);
      else rc = 1;
    } else if (auto it = regular.find(name); it != regular.end()) {
      if ((not global and not regular_only) or (global and it->second.global)
          or (regular_only and not it->second.global))
        print_alias(name, it->second.text, it->second.global ? 'g' : 0, as_command);
    } else {
      rc = 1;
    }
  }
  return rc;
}

// unalias [-ams] name …: removes aliases (-s suffix ones; -a all; -m
// patterns).
int builtin_unalias(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "amsr", "", o)) return 1;
  bool suffix = o.has('s');
  if (o.has('a')) {
    if (suffix) ctx.aliases.suffix.clear();
    else        ctx.aliases.regular.clear();
    return 0;
  }
  if (o.next >= args.size()) return builtin_error(cmd, "not enough arguments");
  int rc = 0;
  bool matched = false;   // -m: by any of the patterns (none is status 1)
  for (s::size_t i = o.next; i < args.size(); ++i) {
    const str& name = args[i];
    if (o.has('m')) {
      auto pat = CompiledPattern::compile(name);
      auto hit = [&](const auto& kv) { return pat.matches(kv.first); };
      matched = (suffix ? s::erase_if(ctx.aliases.suffix, hit)
                        : s::erase_if(ctx.aliases.regular, hit)) != 0 or matched;
      continue;
    }
    bool erased = suffix ? ctx.aliases.suffix.erase(name) : ctx.aliases.regular.erase(name);
    if (not erased) {
      builtin_error(cmd, "no such hash table element: " + name);
      rc = 1;
    }
  }
  if (o.has('m') and not matched) rc = 1;
  return rc;
}

// unhash [-adfms] name …: zsh's bin_unhash.  -a aliases and -s suffix
// aliases (unalias's), -f functions (unfunction's), -d named directories
// (jfsh keeps none), and the command hash otherwise; -m takes patterns,
// and matching nothing is status 1.
int builtin_unhash(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(args, "adfms", 1, -1, ops);
  if (first < 0) return 1;
  vec<str> rest(args.begin() + first, args.end());
  bool pattern = ops.isset('m');
  auto forward = [&](int (*fn)(vec<str>&, ExecContext&), const char* flag) {
    vec<str> a{args[0]};
    if (flag) a.emplace_back(flag);
    if (pattern) a.emplace_back("-m");
    a.emplace_back("--");
    a.insert(a.end(), rest.begin(), rest.end());
    return fn(a, ctx);
  };
  if (ops.isset('d')) {
    if (pattern) return 1;
    for (const auto& n : rest) builtin_error(args[0], "no such hash table element: " + n);
    return 1;
  }
  if (ops.isset('f')) return forward(builtin_unset, "-f");
  if (ops.isset('s')) return forward(builtin_unalias, "-s");
  if (ops.isset('a')) return forward(builtin_unalias, nullptr);
  auto& hash = ctx.executor->command_hash();
  int rc = 0;
  if (pattern) {
    bool any = false;
    for (const auto& n : rest) {
      auto pat = CompiledPattern::compile(n);
      any = s::erase_if(hash, [&](const auto& kv) { return pat.matches(kv.first); }) or any;
    }
    return any ? 0 : 1;
  }
  for (const auto& n : rest) {
    if (hash.erase(n)) continue;
    builtin_error(args[0], "no such hash table element: " + n);
    rc = 1;
  }
  return rc;
}

}  // namespace jfsh
