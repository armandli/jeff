#include <jfsh/modules/zutil.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <map>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>

// zsh/zutil (M10 Phase 2a): zstyle, zparseopts, is-at-least, zformat.
//
// These do not touch global state outside the module except through the
// shell's parameter table (zparseopts fills $arr; zformat fills $var;
// is-at-least sets no params but returns an exit code; zstyle keeps its
// table in a `StyleTable` singleton on the module handle's closure).

namespace jfsh {

namespace {

// ---------------------------------------------------------------------------
// zstyle: a table of `(context-pattern, name) → value-list`.
//
// Lookups match the context against each registered pattern using the
// shell's glob engine; on a hit, the value-list of the first match wins
// (zsh walks in insertion order, matching this).
// ---------------------------------------------------------------------------

struct StyleEntry {
  str      pattern;      // context pattern (`:completion:*`, …)
  str      name;         // style name
  vec<str> values;
};

struct StyleTable {
  // Insertion order matters — don't sort.
  vec<StyleEntry> entries;

  void set(str pattern, str name, vec<str> values) {
    for (auto& e : entries)
      if (e.pattern == pattern and e.name == name) {
        e.values = s::move(values);
        return;
      }
    entries.push_back({s::move(pattern), s::move(name), s::move(values)});
  }

  void erase(strv pattern, strv name) {
    entries.erase(
        s::remove_if(entries.begin(), entries.end(),
                     [&](const StyleEntry& e) {
                       return (pattern.empty() or e.pattern == pattern)
                          and (name.empty()    or e.name    == name);
                     }),
        entries.end());
  }

  // Match `context` against `pattern` using the shell's glob engine.  zsh's
  // zstyle patterns are shell patterns anchored to the whole string.
  static bool match(strv pattern, strv context, const OptionState& opts) {
    auto cp = CompiledPattern::compile(pattern, /*case_insensitive=*/false,
                                       opts.extended_glob());
    return cp.matches(context);
  }

  const StyleEntry* lookup(strv context, strv name, const OptionState& opts) const {
    for (const auto& e : entries)
      if (e.name == name and match(e.pattern, context, opts)) return &e;
    return nullptr;
  }
};

// Keep one StyleTable per shell by stashing it in a static.  Phase 2b
// moves it onto ExecContext proper when more modules want per-shell state.
static StyleTable& the_style_table() {
  static StyleTable t;
  return t;
}

int builtin_zstyle(vec<str>& args, ExecContext& ctx) {
  auto& table = the_style_table();

  // No args: list every entry in sourceable form.
  if (args.size() == 1) {
    for (const auto& e : table.entries) {
      s::printf("%s %s", e.pattern.c_str(), e.name.c_str());
      for (const auto& v : e.values) s::printf(" %s", v.c_str());
      s::putchar('\n');
    }
    return 0;
  }

  s::size_t i = 1;
  bool list      = false;
  bool list_long = false;
  bool delete_   = false;
  bool test_set  = false;
  bool match_set = false;
  str  test_context;

  // zstyle -L [pattern [style]]  — list sourceable
  // zstyle -g arr [context [pattern [style]]]  — get into array (M15)
  // zstyle -s|-b|-a|-t|-T|-m context style name — typed queries
  // zstyle -d [pattern [style]]  — delete
  // zstyle -e pattern style cmd  — eval form (treated as regular set)
  // Otherwise: zstyle PATTERN NAME VALUE…  — set
  if (i < args.size() and args[i].size() > 1 and args[i][0] == '-') {
    const str& flag = args[i];
    if (flag == "-L")      { list = true; list_long = true; ++i; }
    else if (flag == "-d") { delete_ = true; ++i; }
    else if (flag == "-t" or flag == "-T") {
      if (i + 2 >= args.size()) {
        s::fprintf(stderr, "jfsh: zstyle: not enough arguments\n");
        return 1;
      }
      test_context = args[i + 1];
      const str& style = args[i + 2];
      bool strict = (flag == "-T");
      const auto* e = table.lookup(test_context, style, ctx.opts);
      if (not e) return strict ? 0 : 1;
      // Any extra args are allowed values; -t succeeds if value matches.
      if (i + 3 >= args.size()) {
        // "truth" of the first value
        if (e->values.empty()) return 1;
        const str& v = e->values[0];
        return (v == "yes" or v == "true" or v == "on" or v == "1") ? 0 : 1;
      }
      for (s::size_t k = i + 3; k < args.size(); ++k)
        for (const auto& v : e->values) if (v == args[k]) return 0;
      return 1;
    }
    else if (flag == "-s" or flag == "-b" or flag == "-a") {
      if (i + 3 >= args.size()) {
        s::fprintf(stderr, "jfsh: zstyle: not enough arguments\n");
        return 1;
      }
      const str& context = args[i + 1];
      const str& style   = args[i + 2];
      const str& name    = args[i + 3];
      str sep = (i + 4 < args.size()) ? args[i + 4] : str(" ");
      const auto* e = table.lookup(context, style, ctx.opts);
      if (not e) return 1;
      if (flag == "-a") {
        auto r = ctx.params.set_array(name, e->values, false);
        return r ? 0 : 1;
      }
      if (flag == "-b") {
        bool truthy = not e->values.empty() and
                      (e->values[0] == "yes" or e->values[0] == "true"
                       or e->values[0] == "on" or e->values[0] == "1");
        auto r = ctx.params.set_scalar(name, truthy ? "yes" : "no");
        return r ? 0 : 1;
      }
      // -s: join with sep
      str out;
      for (s::size_t k = 0; k < e->values.size(); ++k) {
        if (k) out += sep;
        out += e->values[k];
      }
      auto r = ctx.params.set_scalar(name, out);
      return r ? 0 : 1;
    }
    else if (flag == "-m") {
      // zstyle -m context style pattern-list: true if any value matches
      if (i + 3 >= args.size()) return 1;
      const auto* e = table.lookup(args[i + 1], args[i + 2], ctx.opts);
      if (not e) return 1;
      for (const auto& val : e->values) {
        auto cp = CompiledPattern::compile(args[i + 3],
                                           /*case_insensitive=*/false,
                                           ctx.opts.extended_glob());
        if (cp.matches(val)) return 0;
      }
      return 1;
    }
    else if (flag == "-e") { ++i; /* eval form: treated as normal set */ }
    else if (flag == "--") { ++i; }
    else {
      s::fprintf(stderr, "jfsh: zstyle: bad option: %s\n", flag.c_str());
      return 1;
    }
  }

  if (list or list_long) {
    const str* pat = (i < args.size()) ? &args[i] : nullptr;
    const str* sty = (i + 1 < args.size()) ? &args[i + 1] : nullptr;
    for (const auto& e : table.entries) {
      if (pat and e.pattern != *pat) continue;
      if (sty and e.name    != *sty) continue;
      s::printf("zstyle %s %s", e.pattern.c_str(), e.name.c_str());
      for (const auto& v : e.values) s::printf(" %s", v.c_str());
      s::putchar('\n');
    }
    return 0;
  }

  if (delete_) {
    strv pat = (i < args.size()) ? strv(args[i]) : strv{};
    strv sty = (i + 1 < args.size()) ? strv(args[i + 1]) : strv{};
    table.erase(pat, sty);
    return 0;
  }

  // Set: zstyle PATTERN NAME VALUE…
  if (i + 1 >= args.size()) {
    s::fprintf(stderr, "jfsh: zstyle: not enough arguments\n");
    return 1;
  }
  str pattern = args[i];
  str name    = args[i + 1];
  vec<str> values(args.begin() + static_cast<long>(i) + 2, args.end());
  table.set(s::move(pattern), s::move(name), s::move(values));
  return 0;
}

// ---------------------------------------------------------------------------
// zparseopts: parse an option list out of the positional parameters.
//
// Form: zparseopts [-D] [-E] [-K] [-M] [-F] [-A hash] [-a arr] spec…
//   spec: name[+][:[:]][=target]
//     name   : the flag ("-f", "--long", "-x+")
//     +      : flag may repeat (with -K, accumulates)
//     :      : takes an argument (required)
//     ::     : takes an optional argument
//     =target : store under this name instead
// `-D` deletes parsed options from $argv; `-E` keeps going past non-options;
// `-K` keeps destinations defined before the call; `-F` fails on an unknown
// -flag; `-A` names a hash, `-a` an array to populate.
// ---------------------------------------------------------------------------

struct OptSpec {
  str  name;     // the flag text
  bool repeats = false;
  bool takes_arg = false;
  bool arg_optional = false;
  str  target;   // destination name or empty → use `name`
};

static bool starts_with(strv s, strv prefix) {
  return s.size() >= prefix.size() and s.substr(0, prefix.size()) == prefix;
}

static Result<OptSpec> parse_spec(strv text) {
  OptSpec sp;
  s::size_t eq = text.find('=');
  str head(eq == strv::npos ? text : text.substr(0, eq));
  if (eq != strv::npos) sp.target = str(text.substr(eq + 1));
  // Trailing modifiers: '+' (repeatable) and ':' / '::' (arg).
  while (not head.empty()) {
    char c = head.back();
    if (c == '+') { sp.repeats = true; head.pop_back(); continue; }
    if (c == ':') {
      if (sp.takes_arg) { sp.arg_optional = true; }
      else              { sp.takes_arg    = true; }
      head.pop_back();
      continue;
    }
    break;
  }
  if (head.empty())
    return s::unexpected(ShellError{ErrorKind::BadSubstitution,
        "zparseopts: empty option name"});
  sp.name = s::move(head);
  return sp;
}

int builtin_zparseopts(vec<str>& args, ExecContext& ctx) {
  bool delete_from_argv = false;
  bool keep_going       = false;
  bool keep_targets     = false;
  bool fail_on_unknown  = false;
  bool match_longest    = false;   // -M (prefix matching)
  str  array_name;
  str  hash_name;

  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-D") delete_from_argv = true;
    else if (a == "-E") keep_going      = true;
    else if (a == "-K") keep_targets    = true;
    else if (a == "-F") fail_on_unknown = true;
    else if (a == "-M") match_longest   = true;
    else if (a == "-A") {
      if (++i >= args.size()) {
        s::fprintf(stderr, "jfsh: zparseopts: -A requires a name\n");
        return 1;
      }
      hash_name = args[i];
    } else if (a == "-a") {
      if (++i >= args.size()) {
        s::fprintf(stderr, "jfsh: zparseopts: -a requires a name\n");
        return 1;
      }
      array_name = args[i];
    } else if (a == "--") { ++i; break; }
    else break;
  }

  vec<OptSpec> specs;
  for (; i < args.size(); ++i) {
    auto sp = parse_spec(args[i]);
    if (not sp) {
      s::fprintf(stderr, "jfsh: zparseopts: %s\n", sp.error().message.c_str());
      return 1;
    }
    specs.push_back(*sp);
  }

  // Parse against $argv = ctx.positional.
  vec<str> consumed_flat;              // -a: flag, value, flag, value, …
  s::map<str, vec<str>> consumed_hash; // -A: flag → value (last wins unless +)
  vec<str> remainder;

  // Strip the leading `-` / `--` from a command-line flag: the spec name
  // in zsh is `f` for `-f`, `-help` for `--help`.
  const auto bare = [](strv flag) -> strv {
    if (flag.size() > 2 and flag[0] == '-' and flag[1] == '-') return flag.substr(2);
    if (flag.size() > 1 and flag[0] == '-') return flag.substr(1);
    return flag;
  };
  const auto find_spec = [&](strv flag) -> const OptSpec* {
    strv key = bare(flag);
    for (const auto& s : specs) if (s.name == key) return &s;
    if (match_longest) {
      const OptSpec* best = nullptr;
      for (const auto& s : specs)
        if (starts_with(key, s.name)
            and (not best or s.name.size() > best->name.size()))
          best = &s;
      return best;
    }
    return nullptr;
  };

  auto& argv = ctx.positional;
  for (s::size_t k = 0; k < argv.size(); ++k) {
    const str& w = argv[k];
    if (w.size() < 2 or w[0] != '-' or w == "--") {
      if (w == "--") {
        // Pass the rest through as remainder.
        for (++k; k < argv.size(); ++k) remainder.push_back(argv[k]);
        break;
      }
      remainder.push_back(w);
      if (not keep_going) {
        for (++k; k < argv.size(); ++k) remainder.push_back(argv[k]);
        break;
      }
      continue;
    }
    // Possible `--flag=value`.
    s::size_t eq = w.find('=');
    str flag  = (eq == str::npos) ? w : w.substr(0, eq);
    str value = (eq == str::npos) ? str{} : w.substr(eq + 1);
    const OptSpec* sp = find_spec(flag);
    if (not sp) {
      if (fail_on_unknown) {
        s::fprintf(stderr, "jfsh: zparseopts: bad option: %s\n", w.c_str());
        return 1;
      }
      remainder.push_back(w);
      continue;
    }
    if (sp->takes_arg and eq == str::npos) {
      if (sp->arg_optional) {
        value.clear();  // optional and absent
      } else {
        if (k + 1 >= argv.size()) {
          s::fprintf(stderr, "jfsh: zparseopts: missing argument for %s\n", flag.c_str());
          return 1;
        }
        value = argv[++k];
      }
    }
    const str& dest = sp->target.empty() ? sp->name : sp->target;
    consumed_flat.push_back(dest);
    consumed_flat.push_back(value);
    consumed_hash[dest].push_back(value);
  }

  if (delete_from_argv) ctx.positional = remainder;

  if (not array_name.empty()) {
    if (not keep_targets) (void)ctx.params.set_array(array_name, {}, false);
    (void)ctx.params.set_array(array_name, consumed_flat, false);
  }
  if (not hash_name.empty()) {
    // An existing associative array parameter takes words as key/value
    // pairs: declare it as an association first, then assign.
    if (not keep_targets) {
      Param& p = ctx.params.assign(hash_name, umap<str, str>{},
                                   PM_HASH);
      (void)p;
    }
    vec<str> flat;
    for (const auto& [k, v] : consumed_hash) {
      flat.push_back(k);
      flat.push_back(v.empty() ? str{} : v.back());
    }
    (void)ctx.params.set_array(hash_name, flat, false);
  }

  // Each spec with `=target` also updates a dedicated variable.
  for (const auto& sp : specs) {
    if (sp.target.empty()) continue;
    auto it = consumed_hash.find(sp.target);
    if (it == consumed_hash.end()) continue;
    if (sp.repeats) (void)ctx.params.set_array (sp.target, it->second, false);
    else            (void)ctx.params.set_scalar(sp.target, it->second.back());
  }

  return 0;
}

// ---------------------------------------------------------------------------
// is-at-least: version comparison.  `is-at-least v1 [v2]` succeeds when
// v2 (or $ZSH_VERSION) is >= v1 as zsh compares them (lexicographic on
// `.`-separated numeric and alpha components).
// ---------------------------------------------------------------------------

static vec<str> split_version(const str& v) {
  vec<str> parts;
  s::size_t start = 0;
  for (s::size_t j = 0; j <= v.size(); ++j) {
    if (j == v.size() or v[j] == '.' or v[j] == '-' or v[j] == '+') {
      if (j > start) parts.push_back(v.substr(start, j - start));
      start = j + 1;
    }
  }
  return parts;
}

// Compare one component: numeric ones compare as ints, else lexicographic.
static int cmp_component(const str& a, const str& b) {
  bool na = not a.empty() and s::isdigit(static_cast<unsigned char>(a[0]));
  bool nb = not b.empty() and s::isdigit(static_cast<unsigned char>(b[0]));
  if (na and nb) {
    long long ai = s::strtoll(a.c_str(), nullptr, 10);
    long long bi = s::strtoll(b.c_str(), nullptr, 10);
    return (ai < bi) ? -1 : (ai > bi) ? 1 : 0;
  }
  return a.compare(b);
}

static int cmp_version(const str& a, const str& b) {
  auto ap = split_version(a), bp = split_version(b);
  s::size_t n = s::min(ap.size(), bp.size());
  for (s::size_t j = 0; j < n; ++j) {
    int c = cmp_component(ap[j], bp[j]);
    if (c) return c;
  }
  if (ap.size() < bp.size()) return -1;
  if (ap.size() > bp.size()) return  1;
  return 0;
}

int builtin_is_at_least(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) {
    s::fprintf(stderr, "jfsh: is-at-least: not enough arguments\n");
    return 1;
  }
  str want = args[1];
  str have;
  if (args.size() > 2) have = args[2];
  else {
    auto p = ctx.params.lookup("ZSH_VERSION");
    have = (p and p->get().is_set()) ? p->get().scalar() : str("0");
  }
  return cmp_version(have, want) >= 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// zformat -f var fmt args…
//   fmt: a string with `%key` / `%-NNkey` / `%NNkey` sequences; each `key`
//        is replaced by the value `key:value` arg (or the empty string if
//        no such arg).  `%` is a literal `%`.  args are k:v pairs.
// ---------------------------------------------------------------------------

int builtin_zformat(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 4 or args[1] != "-f") {
    s::fprintf(stderr, "jfsh: zformat: expected -f var fmt args…\n");
    return 1;
  }
  const str& var = args[2];
  const str& fmt = args[3];
  umap<char, str> table;
  for (s::size_t k = 4; k < args.size(); ++k) {
    const str& a = args[k];
    if (a.size() < 2 or a[1] != ':') continue;
    table[a[0]] = a.substr(2);
  }
  str out;
  for (s::size_t j = 0; j < fmt.size(); ++j) {
    if (fmt[j] != '%') { out += fmt[j]; continue; }
    if (++j >= fmt.size()) break;
    // Width: optional - and digits before the key char.
    bool left_align = false;
    if (fmt[j] == '-') { left_align = true; if (++j >= fmt.size()) break; }
    int width = 0;
    while (j < fmt.size() and s::isdigit(static_cast<unsigned char>(fmt[j]))) {
      width = width * 10 + (fmt[j] - '0');
      ++j;
    }
    if (j >= fmt.size()) break;
    char key = fmt[j];
    if (key == '%') { out += '%'; continue; }
    str value;
    auto it = table.find(key);
    if (it != table.end()) value = it->second;
    if (width <= 0) { out += value; continue; }
    if (static_cast<int>(value.size()) >= width) out += value;
    else {
      int pad = width - static_cast<int>(value.size());
      if (left_align) out += value + str(static_cast<s::size_t>(pad), ' ');
      else            out += str(static_cast<s::size_t>(pad), ' ') + value;
    }
  }
  auto r = ctx.params.set_scalar(var, out);
  return r ? 0 : 1;
}

}  // namespace

void register_zutil_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/zutil", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"zstyle",      builtin_zstyle,      BINF_NONE});
    out.builtins.push_back(BuiltinDef{"zparseopts",  builtin_zparseopts,  BINF_NONE});
    out.builtins.push_back(BuiltinDef{"is-at-least", builtin_is_at_least, BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"zformat",     builtin_zformat,     BINF_NONE, 3});
  });
}

}  // namespace jfsh
