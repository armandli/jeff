#include <jfsh/modules/zutil.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <array>
#include <deque>
#include <map>

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
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
// zstyle: for each style name, the context patterns it is set for, most
// specific first (zsh's weights: a component that is just `*` counts 0,
// one with pattern characters 1, a literal one 2), then the one with more
// components, then in the order they were set.  `-e` values are code run at each lookup.
// ---------------------------------------------------------------------------

struct StylePattern {
  str             pattern;
  CompiledPattern prog;
  vec<str>        values;
  bool            eval   = false;
  int             weight = 0;
  s::size_t       parts  = 0;
};

struct StyleTable {
  static int weight(strv pat) {
    int weight = 0, tmp = 2;
    bool first = true;
    for (s::size_t i = 0; i < pat.size(); ++i) {
      char c = pat[i];
      if (first and c == '*' and (i + 1 == pat.size() or pat[i + 1] == ':')) {
        tmp = 0;
        continue;
      }
      first = false;
      if (strv("(|*[<?#^").find(c) != strv::npos) tmp = 1;
      if (c == ':') {
        first = true;
        weight += tmp;
        tmp = 2;
      }
    }
    return weight + tmp;
  }

  void set(str pattern, const str& name, vec<str> values, bool eval, const OptionState& opts) {
    auto& pats = styles[name];
    for (auto& p : pats)
      if (p.pattern == pattern) {
        p.values = s::move(values);
        p.eval = eval;
        return;
      }
    StylePattern p{pattern, CompiledPattern::compile(pattern, false, opts.extended_glob()),
                   s::move(values), eval, weight(pattern),
                   static_cast<s::size_t>(s::count(pattern.begin(), pattern.end(), ':'))};
    auto at = s::find_if(pats.begin(), pats.end(), [&](const StylePattern& q) {
      return q.weight < p.weight or (q.weight == p.weight and q.parts < p.parts);
    });
    pats.insert(at, s::move(p));
  }

  // The values for `context` and `name`; null when the style isn't set
  // there (or its code failed).
  opt<vec<str>> lookup(strv context, const str& name, ExecContext& ctx) const {
    auto it = styles.find(name);
    if (it == styles.end()) return s::nullopt;
    for (const auto& p : it->second) {
      if (not p.prog.matches(context)) continue;
      if (not p.eval) return p.values;
      str code;
      for (s::size_t k = 0; k < p.values.size(); ++k) code += (k ? " " : "") + p.values[k];
      (void)ctx.params.unset("reply");
      bool aborting = ctx.aborting;
      ctx.executor->exec_string(code);
      if (ctx.aborting and not aborting) {
        ctx.aborting = false;
        return s::nullopt;
      }
      opt<vec<str>> out;
      if (auto r = ctx.params.lookup("reply"); r and r->get().is_set())
        out = r->get().is_array() ? r->get().array() : vec<str>{r->get().scalar()};
      (void)ctx.params.unset("reply");
      return out;
    }
    return s::nullopt;
  }

  std::map<str, vec<StylePattern>> styles;   // listed sorted by name
};

StyleTable& the_style_table() {
  static StyleTable t;
  return t;
}

bool style_true(strv v) { return v == "yes" or v == "true" or v == "on" or v == "1"; }

int zstyle_error(strv msg) {
  s::fprintf(stderr, "jfsh: zstyle: %.*s\n", static_cast<int>(msg.size()), msg.data());
  return 1;
}

// zstyle [-L [metapattern [style]]] | -d [pattern [style…]] | -g name
// [pattern [style]] | -s|-b|-a|-t|-T|-m context style … | [-e] pattern
// style value…
int builtin_zstyle(vec<str>& args, ExecContext& ctx) {
  auto& table = the_style_table();
  vec<str> a(args.begin() + 1, args.end());
  auto quoted = [](const vec<str>& vs) {
    str out;
    for (const auto& v : vs) out += " " + shell_quote(v);
    return out;
  };
  if (a.empty() or a[0] == "-L") {
    bool sourceable = not a.empty();
    opt<CompiledPattern> meta;
    if (a.size() > 1) meta = CompiledPattern::compile(a[1], false, ctx.opts.extended_glob());
    for (const auto& [name, pats] : table.styles) {
      if (a.size() > 2 and name != a[2]) continue;
      bool header = false;
      for (const auto& p : pats) {
        if (meta and not meta->matches(p.pattern)) continue;
        if (sourceable) {
          s::printf("zstyle %s%s %s%s\n", p.eval ? "-e " : "", shell_quote(p.pattern).c_str(),
                    shell_quote(name).c_str(), quoted(p.values).c_str());
          continue;
        }
        if (not header) s::printf("%s\n", name.c_str());
        header = true;
        s::printf("%s  %s%s\n", p.eval ? "(eval)" : "      ", p.pattern.c_str(),
                  quoted(p.values).c_str());
      }
    }
    return 0;
  }
  str opt_word = a[0];
  bool is_opt = opt_word.size() == 2 and opt_word[0] == '-';
  if (not is_opt or opt_word == "-e") {
    bool eval = is_opt;
    if (eval) a.erase(a.begin());
    if (a.size() < 2) return zstyle_error("not enough arguments");
    if (eval and a.size() < 3) return zstyle_error("not enough arguments");
    vec<str> values(a.begin() + 2, a.end());
    table.set(a[0], a[1], s::move(values), eval, ctx.opts);
    return 0;
  }
  char o = opt_word[1];
  a.erase(a.begin());
  auto need = [&](s::size_t lo, s::size_t hi) {
    if (a.size() < lo) return zstyle_error("not enough arguments");
    if (a.size() > hi) return zstyle_error("too many arguments");
    return 0;
  };
  switch (o) {
    case 'd':
      if (a.empty()) {
        table.styles.clear();
      } else {
        for (auto it = table.styles.begin(); it != table.styles.end();) {
          bool listed = a.size() == 1 or s::find(a.begin() + 1, a.end(), it->first) != a.end();
          if (listed)
            s::erase_if(it->second, [&](const StylePattern& p) { return p.pattern == a[0]; });
          it = it->second.empty() ? table.styles.erase(it) : s::next(it);
        }
      }
      return 0;

    case 'g': {
      if (need(1, 3)) return 1;
      vec<str> out;
      auto add = [&](const str& v) {
        if (s::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
      };
      bool found = a.size() == 1;
      for (const auto& [name, pats] : table.styles)
        for (const auto& p : pats) {
          if (a.size() == 1) add(p.pattern);
          else if (p.pattern == a[1] and a.size() == 2) { add(name); found = true; }
          else if (p.pattern == a[1] and name == a[2]) { out = p.values; found = true; }
        }
      (void)ctx.params.set_array(a[0], s::move(out));
      return found ? 0 : 1;
    }

    case 's': case 'b': case 'a': case 'm': {
      if (o == 's' ? need(3, 4) : need(3, 3)) return 1;
      auto vals = table.lookup(a[0], a[1], ctx);
      if (o == 'm') {
        if (not vals) return 1;
        auto pat = CompiledPattern::compile(a[2], false, ctx.opts.extended_glob());
        for (const auto& v : *vals)
          if (pat.matches(v)) return 0;
        return 1;
      }
      if (o == 'a') {
        (void)ctx.params.set_array(a[2], vals ? *vals : vec<str>{});
        return vals ? 0 : 1;
      }
      str out;
      if (o == 'b') {
        bool yes = vals and vals->size() == 1 and style_true((*vals)[0]);
        (void)ctx.params.set_scalar(a[2], yes ? "yes" : "no");
        return yes ? 0 : 1;
      }
      if (vals) {
        str sep = a.size() > 3 ? a[3] : " ";
        for (s::size_t k = 0; k < vals->size(); ++k) out += (k ? sep : "") + (*vals)[k];
      }
      (void)ctx.params.set_scalar(a[2], s::move(out));
      return vals ? 0 : 1;
    }

    case 't': case 'T': {
      if (need(2, s::size_t(-1))) return 1;
      auto vals = table.lookup(a[0], a[1], ctx);
      if (not vals) return o == 'T' ? 0 : 2;
      if (a.size() == 2)
        return vals->size() == 1 and style_true((*vals)[0]) ? 0 : 1;
      for (const auto& v : *vals)
        if (s::find(a.begin() + 2, a.end(), v) != a.end()) return 0;
      return 1;
    }

    default:
      return zstyle_error(str("invalid option: ") + opt_word);
  }
}

// ---------------------------------------------------------------------------
// zparseopts [-D] [-E] [-F] [-K] [-M] [-a array] [-A assoc] [-] spec…: a port
// of zsh's bin_zparseopts.  A spec is `name[+][:[:][-]][=array]`: `+` lets
// the option repeat, `:` takes an argument (in the same word or the next),
// `::` an optional one, `:-` one stored with the option (`-xarg`);
// `=array` collects it in its own array.  -D takes the options out of the
// positional parameters, -E goes on past other words, -F fails on an
// unknown option, -K keeps arrays no option was found for, -M maps
// `=name` to the spec of option `name`.
// ---------------------------------------------------------------------------

struct ZoptVal {
  str           name;   // `-x`
  opt<str>      arg;
  opt<str>      text;   // `-xarg` for :: and :- options
};

struct ZoptArr {
  str              name;
  vec<ZoptVal*>    vals;
};

struct ZoptDesc {
  str           name;
  bool          mult = false, arg = false, optarg = false, same = false;
  ZoptArr*      arr = nullptr;
  vec<ZoptVal*> vals;
};

struct ZparseState {
  ZoptDesc* get_desc(strv name) {
    for (auto& d : descs)
      if (d.name == name) return &d;
    return nullptr;
  }
  ZoptArr* get_arr(strv name) {
    for (auto& a : arrs)
      if (a.name == name) return &a;
    return nullptr;
  }
  // -M: an option whose array is named after another option goes there.
  ZoptDesc* map_desc(ZoptDesc* d) {
    ZoptDesc* seen = d;
    for (int n = 0; d and d->arr and n < 64; ++n) {
      ZoptDesc* next = get_desc(strv(d->arr->name).substr(d->arr->name.starts_with('-') ? 1 : 0));
      if (not next or next == d) break;
      d = next;
      if (d == seen) return nullptr;
    }
    return d;
  }
  ZoptDesc* lookup(strv str) {
    for (auto& d : descs)
      if (d.arg ? str.starts_with(d.name) : str == d.name) return &d;
    return nullptr;
  }
  void add_val(ZoptDesc* d, opt<str> arg) {
    str n = "-" + d->name;
    if (map) d = map_desc(d) ? map_desc(d) : d;
    ZoptVal* v = nullptr;
    if (not d->mult and not d->vals.empty()) v = d->vals.front();
    bool fresh = not v;
    if (fresh) {
      v = &store.emplace_back();
      v->name = n;
    }
    v->arg = arg;
    if (d->arg and not (d->optarg or d->same)) v->text.reset();
    else v->text = arg ? n + *arg : n;
    if (fresh) {
      if (d->arr) d->arr->vals.push_back(v);
      d->vals.push_back(v);
    }
  }

  s::deque<ZoptDesc> descs;   // newest first, as zsh's list
  s::deque<ZoptArr>  arrs;    // likewise
  s::deque<ZoptVal>  store;
  bool               map = false;
};

int zparse_error(strv msg) {
  s::fprintf(stderr, "jfsh: zparseopts: %.*s\n", static_cast<int>(msg.size()), msg.data());
  return 1;
}

int builtin_zparseopts(vec<str>& args, ExecContext& ctx) {
  ZparseState zs;
  bool del = false, extract = false, fail = false, keep = false;
  ZoptArr* defarr = nullptr;
  opt<str> assoc;
  s::size_t i = 1;
  bool have_descs = false;
  for (; i < args.size(); ++i) {
    const str& o = args[i];
    if (o.empty() or o[0] != '-') {
      have_descs = true;
      break;
    }
    char c = o.size() > 1 ? o[1] : '\0';
    bool more = o.size() > 2;
    if (c == '\0') { ++i; have_descs = true; break; }
    if (c == '-') { if (not more) ++i; have_descs = true; break; }
    if ((c == 'D' or c == 'E' or c == 'F' or c == 'K' or c == 'M') and not more) {
      (c == 'D' ? del : c == 'E' ? extract : c == 'F' ? fail : c == 'K' ? keep : zs.map) = true;
      continue;
    }
    if (c == 'a' or c == 'A') {
      str name;
      if (more) name = o.substr(2);
      else if (i + 1 < args.size()) name = args[++i];
      else return zparse_error(c == 'a' ? "missing array name" : "missing associative array name");
      if (c == 'a') {
        if (defarr) return zparse_error("default array given more than once");
        defarr = &zs.arrs.emplace_front(ZoptArr{name, {}});
      } else {
        if (assoc) return zparse_error("associative array given more than once");
        assoc = name;
      }
      continue;
    }
    have_descs = true;
    break;
  }
  if (not have_descs or i > args.size()) return zparse_error("missing option descriptions");
  for (; i < args.size(); ++i) {
    str o = args[i];
    if (o.empty()) return zparse_error("invalid option description: " + o);
    ZoptDesc d;
    s::size_t p = 0;
    for (; p < o.size(); ++p) {
      if (o[p] == '\\' and p + 1 < o.size()) {
        ++p;
      } else if (p > 0) {
        if (o[p] == '+') {
          d.mult = true;
          o.erase(p, 1);
          break;
        }
        if (o[p] == ':' or o[p] == '=') break;
      }
    }
    str spec = o.substr(0, p);
    if (p < o.size() and o[p] == ':') {
      d.arg = true;
      ++p;
      if (p < o.size() and o[p] == ':') { ++p; d.optarg = true; }
      if (p < o.size() and o[p] == '-') { ++p; d.same = true; }
    }
    if (p < o.size() and o[p] == '=') {
      str name = o.substr(p + 1);
      d.arr = zs.get_arr(name);
      if (not d.arr) d.arr = &zs.arrs.emplace_front(ZoptArr{name, {}});
    } else if (p < o.size()) {
      return zparse_error("invalid option description: " + args[i]);
    } else if (not (d.arr = defarr) and not assoc) {
      return zparse_error("no default array defined: " + args[i]);
    }
    for (s::size_t k = 0; k < spec.size(); ++k)
      if (spec[k] == '\\' and k + 1 < spec.size()) spec.erase(k, 1);
    d.name = spec;
    if (zs.get_desc(d.name)) return zparse_error("option defined more than once: " + d.name);
    zs.descs.push_front(s::move(d));
  }
  auto single = [&](char c) -> ZoptDesc* {
    for (auto& d : zs.descs)
      if (d.name.size() == 1 and d.name[0] == c) return &d;
    return nullptr;
  };
  vec<str>& pos = ctx.positional;
  vec<str> kept;   // -D -E: the words that aren't options
  s::size_t pp = 0;
  for (; pp < pos.size(); ++pp) {
    const str& o = pos[pp];
    if (o.empty() or o[0] != '-') {
      if (extract) {
        if (del) kept.push_back(o);
        continue;
      }
      break;
    }
    if (o.size() == 1 or o == "--") {
      if (del and extract) kept.push_back(o);
      ++pp;
      break;
    }
    auto next_arg = [&](ZoptDesc* d) -> opt<int> {
      if (not d->optarg or (pp + 1 < pos.size() and not pos[pp + 1].starts_with('-'))) {
        if (pp + 1 >= pos.size())
          return zparse_error("missing argument for option: -" + d->name);
        zs.add_val(d, pos[++pp]);
      } else {
        zs.add_val(d, s::nullopt);
      }
      return s::nullopt;
    };
    if (ZoptDesc* d = zs.lookup(strv(o).substr(1))) {
      if (d->arg) {
        strv e = strv(o).substr(d->name.size() + 1);
        if (not e.empty()) zs.add_val(d, str(e));
        else if (auto rc = next_arg(d)) return *rc;
      } else {
        zs.add_val(d, s::nullopt);
      }
      continue;
    }
    bool unknown = false;
    for (s::size_t k = 1; k < o.size(); ++k) {
      ZoptDesc* d = single(o[k]);
      if (not d) {
        if (fail) {
          return zparse_error(o[k] != '-' ? str("bad option: -") + o[k]
                                          : "bad option: -" + o.substr(k));
        }
        unknown = true;
        break;
      }
      if (d->arg) {
        if (k + 1 < o.size()) zs.add_val(d, o.substr(k + 1));
        else if (auto rc = next_arg(d)) return *rc;
        break;
      }
      zs.add_val(d, s::nullopt);
    }
    if (unknown) {
      if (extract) {
        if (del) kept.push_back(o);
        continue;
      }
      break;
    }
  }
  for (auto& a : zs.arrs) {
    if (keep and a.vals.empty()) continue;
    vec<str> out;
    for (ZoptVal* v : a.vals) {
      if (v->text) {
        out.push_back(*v->text);
      } else {
        out.push_back(v->name);
        if (v->arg) out.push_back(*v->arg);
      }
    }
    (void)ctx.params.set_array(a.name, s::move(out));
  }
  if (assoc) {
    vec<str> pairs;
    for (auto& d : zs.descs) {
      if (d.vals.empty()) continue;
      pairs.push_back("-" + d.name);
      str joined;
      for (s::size_t k = 0; k < d.vals.size(); ++k) {
        if (k) joined += ' ';
        if (d.vals[k]->arg) joined += *d.vals[k]->arg;
      }
      pairs.push_back(s::move(joined));
    }
    if (not (keep and pairs.empty())) {
      auto p = ctx.params.lookup(*assoc);
      if (not p or not p->get().is_hash()) ctx.params.assign(*assoc, umap<str, str>{}, PM_HASH);
      (void)ctx.params.set_array(*assoc, s::move(pairs));
    }
  }
  if (del) {
    if (extract) {
      kept.insert(kept.end(), pos.begin() + static_cast<s::ptrdiff_t>(s::min(pp, pos.size())),
                  pos.end());
      pos = s::move(kept);
    } else {
      pos.erase(pos.begin(), pos.begin() + static_cast<s::ptrdiff_t>(s::min(pp, pos.size())));
    }
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
// zformat -f|-F var format spec… | -a array sep spec…: a port of zsh's
// bin_zformat.  A spec is `c:value`; in the format `%[-][min][.max]c`
// is the value padded to min (on the left with `-`) and cut to max, and
// `%[-][n](c.true.false)` picks a branch: with -f when the value minus n
// is 0, with -F when it is there (and, with n, as wide as n).
// ---------------------------------------------------------------------------

struct ZFormat {
  // Formats `in` from `i` to `end` (or its end), into out unless `skip`;
  // returns where it stopped, npos when a ternary isn't closed.
  s::size_t run(strv in, s::size_t i, char end, bool skip) {
    for (; i < in.size() and in[i] != end; ++i) {
      if (in[i] != '%') {
        if (not skip) out += in[i];
        continue;
      }
      s::size_t start = i;
      bool right = i + 1 < in.size() and in[i + 1] == '-';
      i += right ? 2 : 1;
      auto at = [&](s::size_t k) { return k < in.size() ? in[k] : '\0'; };
      int min = -1, max = -1;
      if (s::isdigit(static_cast<unsigned char>(at(i)))) {
        for (min = 0; s::isdigit(static_cast<unsigned char>(at(i))); ++i)
          min = min * 10 + (in[i] - '0');
      }
      bool testit = at(i) == '(';
      if (testit and at(i + 1) == '-') {
        right = true;
        ++i;
      }
      if ((at(i) == '.' or testit) and s::isdigit(static_cast<unsigned char>(at(i + 1)))) {
        for (max = 0, ++i; s::isdigit(static_cast<unsigned char>(at(i))); ++i)
          max = max * 10 + (in[i] - '0');
      } else if (at(i) == '.' or testit) {
        ++i;
      }
      unsigned char c = static_cast<unsigned char>(at(i));
      if (testit and c) {
        int testval = min >= 0 ? min : max >= 0 ? max : 0;
        bool actval;
        const opt<str>& spec = specs[c];
        if (spec and not spec->empty()) {
          if (presence) {
            int width = testval ? static_cast<int>(spec->size()) : 1;
            actval = right ? testval < width : testval >= width;
          } else {
            if (right) testval = -testval;
            auto v = math_eval(*spec, ctx.params, math_options(ctx),
                               &ctx.modules.math_funcs());
            actval = (v ? v->as_int() : 0) - testval != 0;
          }
        } else {
          actval = presence ? not right : testval != 0;
        }
        char sep = at(++i);
        if (not sep) return strv::npos;
        i = run(in, i + 1, sep, skip or actval);
        if (i == strv::npos) return i;
        i = run(in, i + 1, ')', skip or not actval);
        if (i == strv::npos) return i;
        continue;
      }
      if (skip) continue;
      if (const opt<str>& spec = specs[c]; c and spec) {
        s::size_t len = spec->size();
        if (max >= 0 and len > static_cast<s::size_t>(max)) len = static_cast<s::size_t>(max);
        s::size_t outl = min >= 0 ? s::max(static_cast<s::size_t>(min), len) : len;
        str pad(outl - len, ' ');
        out += right ? pad + spec->substr(0, len) : spec->substr(0, len) + pad;
      } else {
        out += in.substr(start, i - start + 1);
      }
    }
    return i;
  }

  const s::array<opt<str>, 256>& specs;
  bool presence;
  ExecContext& ctx;
  str out;
};

int builtin_zformat(vec<str>& args, ExecContext& ctx) {
  auto fail = [](strv msg) {
    s::fprintf(stderr, "jfsh: zformat: %.*s\n", static_cast<int>(msg.size()), msg.data());
    return 1;
  };
  if (args.size() < 3 or args[1].size() != 2 or args[1][0] != '-')
    return args.size() < 3 ? fail("not enough arguments") : fail("invalid argument: " + args[1]);
  char o = args[1][1];
  if (o == 'f' or o == 'F') {
    if (args.size() < 4) return fail("not enough arguments");
    s::array<opt<str>, 256> specs{};
    // %% is a %, %) a ) (zsh presets them).
    specs['%'] = "%";
    specs[')'] = ")";
    for (s::size_t k = 4; k < args.size(); ++k) {
      const str& a = args[k];
      if (a.size() < 2 or a[1] != ':') return fail("invalid argument: " + a);
      specs[static_cast<unsigned char>(a[0])] = a.substr(2);
    }
    ZFormat zf{specs, o == 'F', ctx, {}};
    zf.run(args[3], 0, '\0', false);
    (void)ctx.params.set_scalar(args[2], zf.out);
    return 0;
  }
  if (o == 'a') {
    if (args.size() < 4) return fail("not enough arguments");
    // The part before an unescaped colon, padded to the widest.
    struct Part { str left, right; bool split; };
    vec<Part> parts;
    s::size_t pre = 0;
    for (s::size_t k = 4; k < args.size(); ++k) {
      const str& a = args[k];
      Part p{{}, {}, false};
      s::size_t c = 0;
      for (; c < a.size() and a[c] != ':'; ++c) {
        if (a[c] == '\\' and c + 1 < a.size()) ++c;
        p.left += a[c];
      }
      if (c < a.size() and c + 1 < a.size()) {
        p.split = true;
        p.right = a.substr(c + 1);
        pre = s::max(pre, p.left.size());
      } else {
        p.left = a;
      }
      parts.push_back(s::move(p));
    }
    vec<str> out;
    for (auto& p : parts) {
      if (not p.split) {
        out.push_back(p.left);
        continue;
      }
      out.push_back(p.left + str(pre - p.left.size(), ' ') + args[3] + p.right);
    }
    (void)ctx.params.set_array(args[2], s::move(out));
    return 0;
  }
  return fail("invalid option: " + args[1]);
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
