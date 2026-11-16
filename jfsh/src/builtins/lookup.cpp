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

constexpr strv RESERVED[] = {
  "!", "[[", "{", "}", "always", "case", "coproc", "do", "done", "elif", "else",
  "esac", "fi", "for", "foreach", "function", "if", "in", "nocorrect", "repeat",
  "select", "then", "time", "until", "while",
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
  str path_var;
  if (auto p = ctx.params.lookup("PATH"); p and p->get().is_set()) path_var = p->get().scalar();
  s::size_t start = 0;
  while (not path_var.empty() and start <= path_var.size()) {
    s::size_t end = path_var.find(':', start);
    if (end == str::npos) end = path_var.size();
    str dir = path_var.substr(start, end - start);
    start = end + 1;
    str candidate = (dir.empty() ? "." : dir) + "/" + name;
    if (is_executable(candidate) and s::find(out.begin(), out.end(), candidate) == out.end())
      out.push_back(candidate);
  }
  return out;
}

str resolve_links(const str& path) {
  char buf[PATH_MAX];
  return ::realpath(path.c_str(), buf) ? str(buf) : path;
}

enum class Style {
  Plain,    // whence: the text, name or path
  Verbose,  // -v, type: `x is …`
  Csh,      // -c, which: `x: aliased to …`
  Word,     // -w: `x: alias`
};

struct WhenceOpts {
  Style style = Style::Plain;
  bool  all = false;          // -a
  bool  path_only = false;    // -p
  bool  func_body = false;    // -f, or -c/-v for functions in csh style
  bool  symlinks = false;     // -s / -S
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
    if (auto a = ctx.aliases.regular.find(name); a != ctx.aliases.regular.end()) {
      const str& text = a->second.text;
      say(text,
          name + " is " + (a->second.global ? "a global alias" : "an alias") + " for " + text,
          name + ": " + (a->second.global ? "globally " : "") + "aliased to " + text,
          "alias");
      if (not w.all) return true;
    }
    if (is_reserved(name)) {
      say(name, name + " is a reserved word", name + ": shell reserved word", "reserved");
      if (not w.all) return true;
    }
    auto fn = ctx.functions.find(name);
    if (fn != ctx.functions.end() or ctx.autoloads.contains(name)) {
      str file;
      if (auto f = ctx.function_files.find(name); f != ctx.function_files.end()) file = f->second;
      str def = fn != ctx.functions.end() ? function_text(name, *fn->second)
                                          : name + " () {\n\t# undefined\n\tbuiltin autoload -XU\n}";
      str verbose = fn != ctx.functions.end()
                  ? name + " is a shell function" + (file.empty() ? str{} : " from " + file)
                  : name + " is an autoload shell function";
      if (w.func_body and w.style == Style::Verbose) verbose += "\n" + def;
      say(w.func_body ? def : name, verbose, def, "function");
      if (not w.all) return true;
    }
    if (ctx.executor->is_builtin(name)) {
      say(name, name + " is a shell builtin", name + ": shell built-in command", "builtin");
      if (not w.all) return true;
    }
  }
  vec<str> paths;
  if (w.all) {
    paths = all_in_path(ctx, name);
  } else if (auto p = ctx.executor->find_command(name); p and is_executable(*p)) {
    paths.push_back(*p);
  }
  for (const auto& p : paths) {
    str shown = p;
    if (w.symlinks) {
      str real = resolve_links(p);
      if (real != p) shown = p + " -> " + real;
    }
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
  int rc = 0;
  for (s::size_t i = o.next; i < args.size(); ++i) {
    if (o.has('m')) {
      // -m: patterns over aliases, functions, builtins and $PATH commands.
      auto pat = CompiledPattern::compile(args[i]);
      vec<str> names;
      for (const auto& [n, _] : ctx.aliases.regular) names.push_back(n);
      for (const auto& [n, _] : ctx.functions) names.push_back(n);
      for (const auto& n : ctx.executor->builtin_names()) names.push_back(n);
      ctx.executor->fill_command_hash();
      for (const auto& [n, _] : ctx.executor->command_hash()) names.push_back(n);
      s::sort(names.begin(), names.end());
      names.erase(s::unique(names.begin(), names.end()), names.end());
      bool any = false;
      for (const auto& n : names)
        if (pat.matches(n)) any = whence_one(n, w, ctx) or any;
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
  if (rest.empty()) return 0;
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
// or lists them.
int builtin_alias(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  if (not parse_options(args, "gLmrs", "", o, true)) return 1;
  bool global = o.has('g'), suffix = o.has('s'), regular_only = o.has('r');
  bool as_command = o.has('L');
  auto& regular = ctx.aliases.regular;
  auto& suffixes = ctx.aliases.suffix;
  auto list = [&](const func<bool(const str&)>& want) {
    if (not suffix) {
      vec<str> names;
      for (const auto& [n, a] : regular) {
        if (global and not a.global) continue;
        if (regular_only and a.global) continue;
        if (want(n)) names.push_back(n);
      }
      s::sort(names.begin(), names.end());
      for (const auto& n : names)
        print_alias(n, regular[n].text, regular[n].global ? 'g' : 0, as_command);
    }
    if (suffix or (not global and not regular_only and as_command)) {
      vec<str> names;
      for (const auto& [n, _] : suffixes)
        if (want(n)) names.push_back(n);
      s::sort(names.begin(), names.end());
      for (const auto& n : names) print_alias(n, suffixes[n], 's', as_command);
    }
  };
  if (o.next >= args.size()) {
    list([](const str&) { return true; });
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
    auto eq = a.find('=');
    if (eq == str::npos or eq == 0) {
      // `alias name`: show it.
      if (suffix) {
        if (auto it = suffixes.find(a); it != suffixes.end()) print_alias(a, it->second, 's', as_command);
        else rc = 1;
      } else if (auto it = regular.find(a); it != regular.end()) {
        print_alias(a, it->second.text, it->second.global ? 'g' : 0, as_command);
      } else {
        rc = 1;
      }
      continue;
    }
    str name = a.substr(0, eq);
    str text = a.substr(eq + 1);
    if (suffix) suffixes[name] = s::move(text);
    else        regular[name] = AliasDef{s::move(text), global};
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
  for (s::size_t i = o.next; i < args.size(); ++i) {
    const str& name = args[i];
    if (o.has('m')) {
      auto pat = CompiledPattern::compile(name);
      if (suffix) s::erase_if(ctx.aliases.suffix, [&](const auto& kv) { return pat.matches(kv.first); });
      else        s::erase_if(ctx.aliases.regular, [&](const auto& kv) { return pat.matches(kv.first); });
      continue;
    }
    bool erased = suffix ? ctx.aliases.suffix.erase(name) : ctx.aliases.regular.erase(name);
    if (not erased) {
      builtin_error(cmd, "no such hash table element: " + name);
      rc = 1;
    }
  }
  return rc;
}

}  // namespace jfsh
