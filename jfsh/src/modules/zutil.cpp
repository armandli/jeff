// zsh/zutil: a port of zsh 5.9's Src/Modules/zutil.c (zstyle, zformat,
// zregexparse, zparseopts).  zsh's copyright and licence:
//
// Copyright (c) 1999 Sven Wischnowsky
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Sven Wischnowsky or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Sven Wischnowsky and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Sven Wischnowsky and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Sven Wischnowsky and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/zutil.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#include <algorithm>
#include <memory>

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/modules/support.h>

namespace jfsh {

namespace {

// zsh's setsparam and setaparam, reporting a failure as zsh's zerr does.
bool set_scalar(ExecContext& ctx, const str& name, str value) {
  return ctx.executor->assign_var(name, s::move(value));
}

bool set_array(ExecContext& ctx, const str& name, vec<str> value) {
  if (auto r = ctx.params.set_array(name, s::move(value)); not r) {
    s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
    ctx.aborting = ctx.abort_line = true;
    return false;
  }
  return true;
}

// $match, $mbegin and $mend kept and put back (zsh's MatchData).
struct MatchData {
  opt<vec<str>> match, mbegin, mend;
};

MatchData savematch(ExecContext& ctx) {
  MatchData m;
  auto get = [&](const char* name) -> opt<vec<str>> {
    auto p = ctx.params.lookup(name);
    if (not p or not p->get().is_set() or not p->get().is_array()) return s::nullopt;
    return p->get().array();
  };
  m.match  = get("match");
  m.mbegin = get("mbegin");
  m.mend   = get("mend");
  return m;
}

void restorematch(ExecContext& ctx, const MatchData& m) {
  auto put = [&](const char* name, const opt<vec<str>>& v) {
    if (v) (void)ctx.params.set_array(name, *v);
    else   (void)ctx.params.unset(name);
  };
  put("match", m.match);
  put("mbegin", m.mbegin);
  put("mend", m.mend);
}

// ---------------------------------------------------------------------------
// Styles.

// A pattern and the values a style has for it.
struct Stypat {
  str             pat;
  CompiledPattern prog;
  s::uint64_t     weight = 0;
  ProgRef         eval;    // zstyle -e: code run at each lookup
  vec<str>        vals;
};

// A style: its patterns, by weight, heaviest first.
struct Style {
  vec<Stypat> pats;
};

using StyleTable = HashTable<Style>;

enum ListMode : int { ZSLIST_NONE, ZSLIST_BASIC, ZSLIST_SYNTAX };

void printstylenode(const str& name, const Style& s, int printflags,
                    const opt<CompiledPattern>& contprog) {
  if (printflags == ZSLIST_BASIC) {
    s::printf("%s\n", shell_quote(name).c_str());
  }
  for (const auto& p : s.pats) {
    if (contprog and not contprog->matches(p.pat)) continue;
    if (printflags == ZSLIST_BASIC) {
      s::printf("%s  %s", p.eval ? "(eval)" : "      ", p.pat.c_str());
    } else {
      s::printf("zstyle %s%s %s", p.eval ? "-e " : "", shell_quote(p.pat).c_str(),
                shell_quote(name).c_str());
    }
    for (const auto& v : p.vals) s::printf(" %s", shell_quote(v).c_str());
    s::putchar('\n');
  }
}

// How specific a pattern is: per `:`-separated component 0 for a lone
// `*`, 1 for one with pattern characters, 2 for a literal one, with the
// number of components in the high half.
s::uint64_t pattern_weight(strv pat) {
  s::uint64_t weight = 0, tmp = 2;
  bool first = true;
  for (s::size_t i = 0; i < pat.size(); ++i) {
    char c = pat[i];
    if (first and c == '*' and (i + 1 == pat.size() or pat[i + 1] == ':')) {
      tmp = 0;
      continue;
    }
    first = false;
    if (c == '(' or c == '|' or c == '*' or c == '[' or c == '<' or c == '?' or c == '#'
        or c == '^')
      tmp = 1;
    if (c == ':') {
      weight += s::uint64_t{1} << 32;
      first = true;
      weight += tmp;
      tmp = 2;
    }
  }
  return weight + tmp;
}

int setstypat(ExecContext& ctx, Style& s, const str& pat, CompiledPattern prog,
              vec<str> vals, bool eval) {
  ProgRef eprog;
  if (eval) {
    str code;
    for (s::size_t i = 0; i < vals.size(); ++i) code += (i ? " " : "") + vals[i];
    auto r = ctx.executor->parse_string(code);
    if (not r) {
      ctx.executor->report_parse_error(r.error());
      return 1;
    }
    eprog = *r;
  }
  for (auto& p : s.pats) {
    if (p.pat == pat) {
      p.vals = s::move(vals);
      p.eval = eprog;
      return 0;
    }
  }
  Stypat p{pat, s::move(prog), pattern_weight(pat), eprog, s::move(vals)};
  auto at = s::find_if(s.pats.begin(), s.pats.end(),
                       [&](const Stypat& q) { return q.weight < p.weight; });
  s.pats.insert(at, s::move(p));
  return 0;
}

opt<vec<str>> evalstyle(ExecContext& ctx, const Stypat& p) {
  (void)ctx.params.unset("reply");
  (void)ctx.executor->exec_parsed(*p.eval);
  if (ctx.aborting) return s::nullopt;
  opt<vec<str>> ret;
  if (auto r = ctx.params.lookup("reply"); r and r->get().is_set())
    ret = r->get().is_array() ? r->get().array() : vec<str>{r->get().scalar()};
  (void)ctx.params.unset("reply");
  return ret;
}

// The values of style `style` for context `ctxt` (zsh's lookupstyle).
opt<vec<str>> lookupstyle(ExecContext& ctx, StyleTable& tab, const str& ctxt, const str& style) {
  auto it = tab.find(style);
  if (it == tab.end()) return s::nullopt;
  MatchData match = savematch(ctx);
  opt<vec<str>> found;
  for (const auto& p : it->second.pats) {
    if (p.prog.matches(ctxt)) {
      found = p.eval ? evalstyle(ctx, p) : opt<vec<str>>{p.vals};
      break;
    }
  }
  restorematch(ctx, match);
  return found;
}

bool truthy(const str& v) { return v == "yes" or v == "true" or v == "on" or v == "1"; }

vec<const s::pair<const str, Style>*> sorted_styles(const StyleTable& tab) {
  vec<const s::pair<const str, Style>*> out;
  for (const auto& kv : tab) out.push_back(&kv);
  s::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->first < b->first; });
  return out;
}

int bin_zstyle(vec<str>& argv, ExecContext& ctx, StyleTable& tab) {
  ZOps ops;
  int first = zsh_options(argv, nullptr, 0, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  vec<str> args(argv.begin() + first, argv.end());
  s::size_t a = 0;   // where args start
  int list = ZSLIST_NONE;
  bool add = false, eval = false;
  if (args.empty()) {
    list = ZSLIST_BASIC;
  } else if (args[0][0] == '-') {
    char oc = args[0].size() > 1 ? args[0][1] : '\0';
    if (oc and oc != '-') {
      if (args[0].size() > 2) return builtin_error(nam, "invalid argument: " + args[0]);
      if (oc == 'L') {
        list = ZSLIST_SYNTAX;
        ++a;
      } else if (oc == 'e') {
        eval = add = true;
        ++a;
      }
    } else {
      add = true;
      ++a;
    }
  } else {
    add = true;
  }
  if (add) {
    if (args.size() - a < 2) return builtin_error(nam, "not enough arguments");
    auto prog = CompiledPattern::compile(args[a], false, ctx.opts.extended_glob());
    if (prog.bad()) return builtin_error(nam, "invalid pattern: " + args[a]);
    Style& s = tab[args[a + 1]];
    int rc = setstypat(ctx, s, args[a], s::move(prog),
                       vec<str>(args.begin() + static_cast<s::ptrdiff_t>(a + 2), args.end()), eval);
    if (s.pats.empty()) tab.erase(args[a + 1]);
    return rc;
  }
  if (list) {
    s::size_t n = args.size() - a;
    if (n > 2) return builtin_error(nam, "too many arguments");
    opt<CompiledPattern> contprog;
    if (n >= 1) {
      contprog = CompiledPattern::compile(args[a], false, ctx.opts.extended_glob());
      if (contprog->bad()) return 1;
    }
    if (n == 2) {
      auto it = tab.find(args[a + 1]);
      if (it == tab.end()) return 1;
      printstylenode(it->first, it->second, list, contprog);
    } else {
      for (auto* kv : sorted_styles(tab)) printstylenode(kv->first, kv->second, list, contprog);
    }
    return 0;
  }
  char op = args[0][1];
  int min, max;
  switch (op) {
    case 'd': min = 0; max = -1;

    break; case 's': min = 3; max = 4;

    break; case 'b': min = 3; max = 3;

    break; case 'a': min = 3; max = 3;

    break; case 't': min = 2; max = -1;

    break; case 'T': min = 2; max = -1;

    break; case 'm': min = 3; max = 3;

    break; case 'g': min = 1; max = 3;

    break; default: return builtin_error(nam, "invalid option: " + args[0]);
  }
  auto n = static_cast<int>(args.size()) - 1;
  if (n < min) return builtin_error(nam, "not enough arguments");
  if (max >= 0 and n > max) return builtin_error(nam, "too many arguments");
  switch (op) {
    case 'd':
      if (args.size() > 1) {
        const str& pat = args[1];
        auto drop = [&](const str& style) {
          auto it = tab.find(style);
          if (it == tab.end()) return;
          auto& pats = it->second.pats;
          auto at = s::find_if(pats.begin(), pats.end(),
                               [&](const Stypat& p) { return p.pat == pat; });
          if (at != pats.end()) pats.erase(at);
          if (pats.empty()) tab.erase(it);
        };
        if (args.size() > 2) {
          for (s::size_t i = 2; i < args.size(); ++i) drop(args[i]);
        } else {
          vec<str> names;
          for (const auto& [name, st] : tab) names.push_back(name);
          for (const auto& name : names) drop(name);
        }
      } else {
        tab.clear();
      }
      return 0;

    case 's': {
      auto vals = lookupstyle(ctx, tab, args[1], args[2]);
      if (vals and not vals->empty()) {
        str sep = args.size() > 4 ? args[4] : " ";
        str ret;
        for (s::size_t i = 0; i < vals->size(); ++i) ret += (i ? sep : "") + (*vals)[i];
        set_scalar(ctx, args[3], ret);
        return 0;
      }
      set_scalar(ctx, args[3], "");
      return 1;
    }

    case 'b': {
      auto vals = lookupstyle(ctx, tab, args[1], args[2]);
      bool yes = vals and vals->size() == 1 and truthy((*vals)[0]);
      set_scalar(ctx, args[3], yes ? "yes" : "no");
      return yes ? 0 : 1;
    }

    case 'a': {
      auto vals = lookupstyle(ctx, tab, args[1], args[2]);
      set_array(ctx, args[3], vals ? *vals : vec<str>{});
      return vals ? 0 : 1;
    }

    case 't': case 'T': {
      auto vals = lookupstyle(ctx, tab, args[1], args[2]);
      if (vals and not vals->empty()) {
        if (args.size() > 3) {
          for (s::size_t i = 3; i < args.size(); ++i)
            if (s::find(vals->begin(), vals->end(), args[i]) != vals->end()) return 0;
          return 1;
        }
        return truthy((*vals)[0]) ? 0 : 1;
      }
      return op == 't' ? (vals ? 1 : 2) : 0;
    }

    case 'm': {
      auto vals = lookupstyle(ctx, tab, args[1], args[2]);
      if (vals) {
        auto prog = CompiledPattern::compile(args[3], false, ctx.opts.extended_glob());
        if (not prog.bad())
          for (const auto& v : *vals)
            if (prog.matches(v)) return 0;
      }
      return 1;
    }

    case 'g': {
      int ret = 1;
      vec<str> out;
      if (args.size() > 2) {
        if (args.size() > 3) {
          if (auto it = tab.find(args[3]); it != tab.end()) {
            for (const auto& p : it->second.pats) {
              if (p.pat == args[2]) {
                out = p.vals;
                ret = 0;
                break;
              }
            }
          }
        } else {
          for (auto* kv : sorted_styles(tab))
            for (const auto& p : kv->second.pats)
              if (p.pat == args[2]) {
                out.push_back(kv->first);
                break;
              }
          ret = 0;
        }
      } else {
        for (auto* kv : sorted_styles(tab))
          for (const auto& p : kv->second.pats)
            if (s::find(out.begin(), out.end(), p.pat) == out.end()) out.push_back(p.pat);
        ret = 0;
      }
      set_array(ctx, args[1], s::move(out));
      return ret;
    }

    default:;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// zformat.

s::size_t display_width(strv s) {
  s::size_t w = 0;
  s::mbstate_t st{};
  for (s::size_t i = 0; i < s.size();) {
    wchar_t wc;
    s::size_t n = s::mbrtowc(&wc, s.data() + i, s.size() - i, &st);
    if (n == 0 or n > s.size() - i) {
      ++w;
      ++i;
      st = {};
      continue;
    }
    int cw = ::wcwidth(wc);
    w += cw > 0 ? static_cast<s::size_t>(cw) : 0;
    i += n;
  }
  return w;
}

// One run of zformat -f's text up to `endchar` (zsh's zformat_substring):
// `%x` and `%-N.Mx` from the specs, `%(x.true.false)` tests.  Returns where
// it stopped, or npos at a premature end.
s::size_t zformat_substring(ExecContext& ctx, strv in, s::size_t pos, const vec<opt<str>>& specs,
                            str& out, char endchar, bool presence, bool skip) {
  s::size_t s = pos;
  auto at = [&](s::size_t i) -> unsigned char {
    return i < in.size() ? static_cast<unsigned char>(in[i]) : 0;
  };
  for (; at(s) and at(s) != static_cast<unsigned char>(endchar); ++s) {
    if (at(s) == '%') {
      s::size_t start = s;
      int min = -1, max = -1;
      bool right = at(++s) == '-';
      if (right) ++s;
      if (s::isdigit(at(s)))
        for (min = 0; s::isdigit(at(s)); ++s) min = min * 10 + (at(s) - '0');
      bool testit = at(s) == '(';
      if (testit and at(s + 1) == '-') {
        right = true;
        ++s;
      }
      if ((at(s) == '.' or testit) and s::isdigit(at(s + 1))) {
        for (max = 0, ++s; s::isdigit(at(s)); ++s) max = max * 10 + (at(s) - '0');
      } else if (at(s) == '.' or testit) {
        ++s;
      }
      if (testit and at(s)) {
        int testval = min >= 0 ? min : max >= 0 ? max : 0;
        int actval;
        const opt<str>& spec = specs[at(s)];
        if (spec and not spec->empty()) {
          if (presence) {
            if (testval)
              actval = static_cast<int>(ctx.opts.multibyte() ? display_width(*spec) : spec->size());
            else
              actval = 1;
            actval = right ? testval < actval : testval >= actval;
          } else {
            if (right) testval = -testval;
            auto v = math_eval(*spec, ctx.params, math_options(ctx), &ctx.modules.math_funcs());
            actval = static_cast<int>(v ? v->as_int() : 0) - testval;
          }
        } else {
          actval = presence ? not right : testval;
        }
        char endcharl = static_cast<char>(at(++s));
        if (not endcharl) return strv::npos;
        s = zformat_substring(ctx, in, s + 1, specs, out, endcharl, presence, skip or actval);
        if (s == strv::npos) return s;
        s = zformat_substring(ctx, in, s + 1, specs, out, ')', presence, skip or not actval);
        if (s == strv::npos) return s;
      } else if (skip) {
        continue;
      } else if (const opt<str>& spec = specs[at(s)]) {
        auto len = static_cast<int>(spec->size());
        if (max >= 0 and len > max) len = max;
        int outl = min >= 0 ? s::max(min, len) : len;
        if (len >= outl) {
          out.append(*spec, 0, static_cast<s::size_t>(outl));
        } else {
          str pad(static_cast<s::size_t>(outl - len), ' ');
          if (right) out += pad + spec->substr(0, static_cast<s::size_t>(len));
          else       out += spec->substr(0, static_cast<s::size_t>(len)) + pad;
        }
      } else {
        out.append(in.substr(start, s - start + 1));
      }
    } else if (not skip) {
      out += in[s];
    }
  }
  return s;
}

int bin_zformat(vec<str>& argv, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(argv, nullptr, 3, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  vec<str> args(argv.begin() + first, argv.end());
  if (args[0][0] != '-' or args[0].size() != 2)
    return builtin_error(nam, "invalid argument: " + args[0]);
  char oc = args[0][1];
  args.erase(args.begin());
  switch (oc) {
    case 'F': case 'f': {
      vec<opt<str>> specs(256);
      specs['%'] = "%";
      specs[')'] = ")";
      for (s::size_t i = 2; i < args.size(); ++i) {
        const str& ap = args[i];
        auto c0 = static_cast<unsigned char>(ap.empty() ? 0 : ap[0]);
        if (ap.empty() or c0 == '-' or c0 == '.' or s::isdigit(c0) or ap.size() < 2 or ap[1] != ':')
          return builtin_error(nam, "invalid argument: " + ap);
        specs[c0] = ap.substr(2);
      }
      str out;
      (void)zformat_substring(ctx, args[1], 0, specs, out, '\0', oc == 'F', false);
      set_scalar(ctx, args[0], out);
      return 0;
    }

    case 'a': {
      // Each `left:right` with the lefts padded to one width and args[1]
      // after them; `\` escapes a `:` in the left.
      s::size_t pre = 0, prechars = 0;
      for (s::size_t i = 2; i < args.size(); ++i) {
        const str& ap = args[i];
        s::size_t nbc = 0, cp = 0;
        for (; cp < ap.size() and ap[cp] != ':'; ++cp)
          if (ap[cp] == '\\' and cp + 1 < ap.size()) {
            ++cp;
            ++nbc;
          }
        if (cp < ap.size() and cp + 1 < ap.size()) {
          s::size_t d = cp - nbc;
          pre = s::max(pre, d);
          s::size_t dchars = ctx.opts.multibyte() ? display_width(strv(ap).substr(0, cp)) - nbc : d;
          prechars = s::max(prechars, dchars);
        }
      }
      vec<str> ret;
      for (s::size_t i = 2; i < args.size(); ++i) {
        const str& ap = args[i];
        str copy;
        s::size_t cp = 0;
        for (; cp < ap.size() and ap[cp] != ':'; ++cp) {
          if (ap[cp] == '\\' and cp + 1 < ap.size()) ++cp;
          copy += ap[cp];
        }
        if (cp < ap.size() and cp + 1 < ap.size()) {
          s::size_t width = ctx.opts.multibyte() ? display_width(copy) : copy.size();
          s::size_t rempad = prechars > width ? prechars - width : 0;
          ret.push_back(copy + str(rempad, ' ') + args[1] + ap.substr(cp + 1));
        } else {
          ret.push_back(copy);
        }
      }
      set_array(ctx, args[0], s::move(ret));
      return 0;
    }

    default:;
  }
  return builtin_error(nam, str("invalid option: -") + oc);
}

// ---------------------------------------------------------------------------
// zregexparse: a regular expression over the words of the command line,
// each element a pattern (zsh's RParse*).

struct RParseState;

struct RParseBranch {
  RParseState*  state;
  vec<str>      actions;
};

struct RParseState {
  opt<str>             pattern;
  opt<CompiledPattern> patprog;
  opt<str>             guard;
  opt<str>             action;
  vec<RParseBranch>    branches;
  int                  cutoff = 0;
};

struct RParseResult {
  opt<vec<str>>     nullacts;
  vec<RParseBranch> in, out;
};

struct RParse {
  vec<str>                  args;
  s::size_t                 at = 0;
  vec<uptr<RParseState>>    states;

  const str* cur() const { return at < args.size() ? &args[at] : nullptr; }
  int elt(RParseResult& result);
  int clo(RParseResult& result);
  int seq(RParseResult& result);
  int alt(RParseResult& result);
};

struct RParseError {};

void connectstates(const vec<RParseBranch>& out, const vec<RParseBranch>& in) {
  for (const auto& ob : out) {
    for (const auto& ib : in) {
      RParseBranch br{ib.state, ob.actions};
      br.actions.insert(br.actions.end(), ib.actions.begin(), ib.actions.end());
      ob.state->branches.push_back(s::move(br));
    }
  }
}

int RParse::elt(RParseResult& result) {
  const str* sp = cur();
  if (not sp) return 1;
  const str& s = *sp;
  switch (s[0]) {
    case '/': {
      s::size_t l = s.size();
      if (not ((2 <= l and s[l - 1] == '/')
               or (3 <= l and s[l - 2] == '/' and (s[l - 1] == '+' or s[l - 1] == '-'))))
        return 1;
      auto st = s::make_unique<RParseState>();
      st->cutoff = s[l - 1];
      str pattern = s[l - 1] == '/' ? s.substr(1, l - 2) : s.substr(1, l - 3);
      ++at;
      opt<str> lookahead;
      if (const str* la = cur(); la and (*la)[0] == '%' and la->size() >= 2 and la->back() == '%') {
        lookahead = la->substr(1, la->size() - 2);
        ++at;
      }
      if (pattern != "[]") {
        str p = "(#b)((#B)" + pattern + ")";
        if (lookahead) p += "(#B)" + *lookahead;
        st->pattern = p + "*";
      }
      if (const str* g = cur(); g and (*g)[0] == '-') {
        st->guard = g->substr(1);
        ++at;
      }
      if (const str* a = cur(); a and (*a)[0] == ':') {
        st->action = a->substr(1);
        ++at;
      }
      RParseState* raw = st.get();
      states.push_back(s::move(st));
      result.nullacts.reset();
      result.in  = {RParseBranch{raw, {}}};
      result.out = {RParseBranch{raw, {}}};
    }

    break; case '(':
      if (s.size() > 1) return 1;
      ++at;
      if (alt(result)) throw RParseError{};
      if (not cur() or *cur() != ")") throw RParseError{};
      ++at;

    break; default:
      return 1;
  }
  return 0;
}

int RParse::clo(RParseResult& result) {
  if (elt(result)) return 1;
  if (cur() and *cur() == "#") {
    while (cur() and *cur() == "#") ++at;
    connectstates(result.out, result.in);
    result.nullacts = vec<str>{};
  }
  return 0;
}

int RParse::seq(RParseResult& result) {
  result.nullacts = vec<str>{};
  result.in.clear();
  result.out.clear();
  for (;;) {
    const str* s = cur();
    if (s and not s->empty() and (*s)[0] == '{' and s->back() == '}') {
      str action = s->substr(1, s->size() - 2);
      ++at;
      if (result.nullacts) result.nullacts->push_back(action);
      for (auto& br : result.out) br.actions.push_back(action);
      continue;
    }
    RParseResult sub;
    if (clo(sub)) break;
    connectstates(result.out, sub.in);
    if (result.nullacts) {
      for (auto& br : sub.in)
        br.actions.insert(br.actions.begin(), result.nullacts->begin(), result.nullacts->end());
      result.in.insert(result.in.end(), sub.in.begin(), sub.in.end());
    }
    if (sub.nullacts) {
      for (auto& br : result.out)
        br.actions.insert(br.actions.end(), sub.nullacts->begin(), sub.nullacts->end());
      result.out.insert(result.out.end(), sub.out.begin(), sub.out.end());
    } else {
      result.out = s::move(sub.out);
    }
    if (result.nullacts and sub.nullacts)
      result.nullacts->insert(result.nullacts->end(), sub.nullacts->begin(), sub.nullacts->end());
    else
      result.nullacts.reset();
  }
  return 0;
}

int RParse::alt(RParseResult& result) {
  if (seq(result)) return 1;
  while (cur() and *cur() == "|") {
    ++at;
    RParseResult sub;
    if (seq(sub)) throw RParseError{};
    if (not result.nullacts and sub.nullacts) result.nullacts = sub.nullacts;
    result.in.insert(result.in.end(), sub.in.begin(), sub.in.end());
    result.out.insert(result.out.end(), sub.out.begin(), sub.out.end());
  }
  return 0;
}

int rmatch(ExecContext& ctx, RParseResult& sm, strv subj, const str& var1, const str& var2,
           bool comp) {
  s::int64_t point1 = 0, point2 = 0;
  (void)set_iparam(ctx, var1, point1);
  (void)set_iparam(ctx, var2, point2);
  auto run = [&](const str& code) { (void)ctx.executor->exec_string(code); };
  if (not comp and subj.empty() and sm.nullacts) {
    for (const auto& a : *sm.nullacts) run(a);
    return 0;
  }
  vec<const vec<RParseBranch>*> nextslist;
  const vec<RParseBranch>* nexts = &sm.in;
  nextslist.push_back(nexts);
  RParseState* st = nullptr;
  bool matched;
  do {
    matched = false;
    MatchData match1 = savematch(ctx);
    for (const auto& br : *nexts) {
      RParseState* next = br.state;
      if (next->pattern and not next->patprog) {
        next->patprog = CompiledPattern::compile(*next->pattern, false, true);
        if (next->patprog->bad()) return 3;
      }
      PatternMatch info;
      if (not next->pattern or not next->patprog->matches(subj, &info)) continue;
      set_match_params(ctx.params, *next->patprog, subj, 0, subj.size(), info,
                       ctx.opts.get(OPT_KSH_ARRAYS));
      if (next->guard) {
        run(*next->guard);
        if (ctx.last_exit) continue;
      }
      bool none = info.groups.empty() or info.groups[0].first == strv::npos;
      s::size_t len = none ? 0 : info.groups[0].second;
      subj.remove_prefix(s::min(len, subj.size()));
      MatchData match2 = savematch(ctx);
      restorematch(ctx, match1);
      for (const auto& a : br.actions) run(a);
      restorematch(ctx, match2);
      point2 += static_cast<s::int64_t>(len);
      (void)set_iparam(ctx, var2, point2);
      st = br.state;
      nexts = &st->branches;
      if (next->cutoff == '-' or (next->cutoff == '/' and len)) {
        nextslist.clear();
        point1 = point2;
        (void)set_iparam(ctx, var1, point1);
      }
      nextslist.push_back(nexts);
      matched = true;
      break;
    }
  } while (matched);
  if (not comp and subj.empty()) {
    for (const auto& br : sm.out) {
      if (br.state == st) {
        for (const auto& a : br.actions) run(a);
        return 0;
      }
    }
  }
  for (const auto* list : nextslist)
    for (const auto& br : *list)
      if (br.state->action) run(*br.state->action);
  return nexts->empty() ? 2 : 1;
}

// zregexparse [-c] var1 var2 string regex…: 0 matched, 1 no candidate
// state matched, 2 there was none, 3 a bad regex.
int bin_zregexparse(vec<str>& argv, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(argv, "c", 3, -1, ops);
  if (first < 0) return 1;
  vec<str> args(argv.begin() + first, argv.end());
  bool oldextendedglob = ctx.opts.get(OPT_EXTENDEDGLOB);
  ctx.opts.set(OPT_EXTENDEDGLOB);
  RParse rp;
  rp.args.assign(args.begin() + 3, args.end());
  RParseResult result;
  int ret = 0;
  try {
    if (rp.alt(result) or rp.cur()) throw RParseError{};
  } catch (const RParseError&) {
    if (rp.cur()) mod_error(argv[0], "invalid regex : " + *rp.cur());
    else          mod_error(argv[0], "not enough regex arguments");
    ret = 3;
  }
  if (not ret) ret = rmatch(ctx, result, args[2], args[0], args[1], ops.isset('c'));
  ctx.opts.assign(OPT_EXTENDEDGLOB, oldextendedglob);
  return ret;
}

// ---------------------------------------------------------------------------
// zparseopts.

enum ZoptFlags : int {
  ZOF_ARG  = 1,
  ZOF_OPT  = 2,
  ZOF_MULT = 4,
  ZOF_SAME = 8,
  ZOF_MAP  = 16,
  ZOF_CYC  = 32,
};

struct Zoptval {
  str      name;
  opt<str> arg;
  opt<str> text;   // zsh's str: `-oarg` in one word
};

struct Zoptarr {
  str               name;
  vec<Zoptval*>     vals;
  int               num = 0;
};

struct Zoptdesc {
  str           name;
  vec<Zoptval*> vals;
  Zoptarr*      arr = nullptr;
  int           flags = 0;
};

struct ZParse {
  vec<uptr<Zoptdesc>> descs;   // newest first, as zsh's list
  vec<uptr<Zoptarr>>  arrs;    // newest first
  vec<uptr<Zoptval>>  vals;

  Zoptdesc* get_desc(strv name) {
    for (auto& d : descs)
      if (d->name == name) return d.get();
    return nullptr;
  }
  Zoptdesc* lookup(strv text) {
    for (auto& d : descs)
      if ((d->flags & ZOF_ARG) ? text.starts_with(d->name) : text == d->name) return d.get();
    return nullptr;
  }
  Zoptarr* get_arr(strv name) {
    for (auto& a : arrs)
      if (a->name == name) return a.get();
    return nullptr;
  }
  Zoptdesc* map_desc(Zoptdesc* start) {
    if (not start or not (start->flags & ZOF_MAP)) return start;
    Zoptdesc* map = get_desc(start->arr->name);
    if (not map) return start;
    if (map == start) {
      start->flags &= ~ZOF_MAP;
      return start;
    }
    if (map->flags & ZOF_CYC) return nullptr;
    start->flags |= ZOF_CYC;
    map = map_desc(map);
    start->flags &= ~ZOF_CYC;
    return map;
  }
  void add_val(Zoptdesc* d, opt<str> arg) {
    Zoptval* v = nullptr;
    str n = "-" + d->name;
    bool fresh = false;
    if (Zoptdesc* map = map_desc(d)) d = map;
    if (not (d->flags & ZOF_MULT) and not d->vals.empty()) v = d->vals.front();
    if (not v) {
      vals.push_back(s::make_unique<Zoptval>());
      v = vals.back().get();
      v->name = n;
      fresh = true;
    }
    v->arg = arg;
    if ((d->flags & ZOF_ARG) and not (d->flags & (ZOF_OPT | ZOF_SAME))) {
      v->text.reset();
      if (d->arr) d->arr->num += arg ? 2 : 1;
    } else if (arg) {
      v->text = "-" + d->name + *arg;
      if (d->arr) d->arr->num += 1;
    } else {
      v->text.reset();
      if (d->arr) d->arr->num += 1;
    }
    if (fresh) {
      if (d->arr) d->arr->vals.push_back(v);
      d->vals.push_back(v);
    }
  }
};

int bin_zparseopts(vec<str>& argv, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(argv, nullptr, 1, -1, ops);
  if (first < 0) return 1;
  strv nam = argv[0];
  vec<str> args(argv.begin() + first, argv.end());
  bool del = false, extract = false, fail = false, keep = false;
  int flags = 0;
  opt<str> assoc;
  Zoptarr* defarr = nullptr;
  ZParse zp;
  vec<Zoptdesc*> sopts(256, nullptr);
  s::size_t i = 0;
  bool described = false;   // zsh's `o` being set at the end of the loop
  while (i < args.size()) {
    const str& o = args[i++];
    if (o[0] != '-') {
      --i;
      described = true;
      break;
    }
    bool stop = false;
    switch (o.size() > 1 ? o[1] : '\0') {
      case '\0': stop = true;

      break; case '-':
        if (o.size() > 2) --i;
        stop = true;

      break; case 'D': case 'E': case 'F': case 'K': case 'M':
        if (o.size() > 2) {
          --i;
          stop = true;
          break;
        }
        switch (o[1]) {
          case 'D': del = true;

          break; case 'E': extract = true;

          break; case 'F': fail = true;

          break; case 'K': keep = true;

          break; default: flags |= ZOF_MAP;
        }

      break; case 'a': {
        if (defarr) return builtin_error(nam, "default array given more than once");
        str n;
        if (o.size() > 2) n = o.substr(2);
        else if (i < args.size()) n = args[i++];
        else return builtin_error(nam, "missing array name");
        zp.arrs.insert(zp.arrs.begin(), s::make_unique<Zoptarr>());
        defarr = zp.arrs.front().get();
        defarr->name = n;
      }

      break; case 'A':
        if (assoc) return builtin_error(nam, "associative array given more than once");
        if (o.size() > 2) assoc = o.substr(2);
        else if (i < args.size()) assoc = args[i++];
        else return builtin_error(nam, "missing array name");

      break; default:
        --i;
        stop = true;
    }
    if (stop) {
      described = true;
      break;
    }
  }
  if (not described) return builtin_error(nam, "missing option descriptions");
  for (; i < args.size(); ++i) {
    str o = args[i];
    int f = 0;
    if (o.empty()) return builtin_error(nam, "invalid option description: " + o);
    s::size_t p = 0;
    for (; p < o.size(); ++p) {
      if (o[p] == '\\' and p + 1 < o.size()) {
        ++p;
      } else if (p > 0) {
        if (o[p] == '+') {
          f |= ZOF_MULT;
          o.erase(p, 1);
          break;
        }
        if (o[p] == ':' or o[p] == '=') break;
      }
    }
    // o[p…] is what follows the name.
    str rest = p < o.size() ? o.substr(p) : str{};
    o.resize(s::min(p, o.size()));
    s::size_t r = 0;
    if (r < rest.size() and rest[r] == ':') {
      f |= ZOF_ARG;
      ++r;
      if (r < rest.size() and rest[r] == ':') {
        ++r;
        f |= ZOF_OPT;
      }
      if (r < rest.size() and rest[r] == '-') {
        ++r;
        f |= ZOF_SAME;
      }
    }
    Zoptarr* a = nullptr;
    if (r < rest.size() and rest[r] == '=') {
      str an = rest.substr(r + 1);
      f |= flags;
      if (not (a = zp.get_arr(an))) {
        zp.arrs.insert(zp.arrs.begin(), s::make_unique<Zoptarr>());
        a = zp.arrs.front().get();
        a->name = an;
      }
    } else if (r < rest.size()) {
      return builtin_error(nam, "invalid option description: " + args[i]);
    } else if (not (a = defarr) and not assoc) {
      return builtin_error(nam, "no default array defined: " + args[i]);
    }
    str name;
    for (s::size_t k = 0; k < o.size(); ++k) {
      if (o[k] == '\\' and k + 1 < o.size()) ++k;
      name += o[k];
    }
    if (zp.get_desc(name)) return builtin_error(nam, "option defined more than once: " + name);
    zp.descs.insert(zp.descs.begin(), s::make_unique<Zoptdesc>());
    Zoptdesc* d = zp.descs.front().get();
    d->name  = name;
    d->flags = f;
    d->arr   = a;
    if (name.size() == 1) sopts[static_cast<unsigned char>(name[0])] = d;
    if ((flags & ZOF_MAP) and not zp.map_desc(d))
      return builtin_error(nam, "cyclic option mapping: " + args[i]);
  }
  vec<str>& pparams = ctx.positional;
  vec<str> kept;   // with -D -E: the arguments that stay
  s::size_t pp = 0;
  for (; pp < pparams.size(); ++pp) {
    str o = pparams[pp];
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
    Zoptdesc* d = zp.lookup(strv(o).substr(1));
    if (not d) {
      bool bad = false;
      for (s::size_t k = 1; k < o.size(); ++k) {
        d = sopts[static_cast<unsigned char>(o[k])];
        if (not d) {
          if (fail) {
            if (o[k] != '-') return builtin_error(nam, str("bad option: -") + o[k]);
            return builtin_error(nam, "bad option: -" + o.substr(k));
          }
          bad = true;
          break;
        }
        if (d->flags & ZOF_ARG) {
          if (k + 1 < o.size()) {
            zp.add_val(d, o.substr(k + 1));
            break;
          }
          if (not (d->flags & ZOF_OPT) or (pp + 1 < pparams.size() and pparams[pp + 1][0] != '-')) {
            if (pp + 1 >= pparams.size())
              return builtin_error(nam, "missing argument for option: -" + d->name);
            zp.add_val(d, pparams[++pp]);
          } else {
            zp.add_val(d, s::nullopt);
          }
        } else {
          zp.add_val(d, s::nullopt);
        }
      }
      if (bad) {
        if (extract) {
          if (del) kept.push_back(pparams[pp]);
          continue;
        }
        break;
      }
    } else if (d->flags & ZOF_ARG) {
      str e = o.substr(d->name.size() + 1);
      if (not e.empty()) {
        zp.add_val(d, e);
      } else if (not (d->flags & ZOF_OPT)
                 or (pp + 1 < pparams.size() and pparams[pp + 1][0] != '-')) {
        if (pp + 1 >= pparams.size())
          return builtin_error(nam, "missing argument for option: -" + d->name);
        zp.add_val(d, pparams[++pp]);
      } else {
        zp.add_val(d, s::nullopt);
      }
    } else {
      zp.add_val(d, s::nullopt);
    }
  }
  if (flags & ZOF_MAP)
    for (auto& d : zp.descs)
      if (d->arr and d->vals.empty() and (d->flags & ZOF_MAP))
        if (d->arr->num == 0 and zp.get_desc(d->arr->name)) d->arr->num = -1;
  if (extract and del)
    for (; pp < pparams.size(); ++pp) kept.push_back(pparams[pp]);
  for (auto& a : zp.arrs) {
    if (a->num >= 0 and (not keep or a->num)) {
      vec<str> aval;
      for (Zoptval* v : a->vals) {
        if (v->text) {
          aval.push_back(*v->text);
        } else {
          aval.push_back(v->name);
          if (v->arg) aval.push_back(*v->arg);
        }
      }
      (void)set_array(ctx, a->name, s::move(aval));
    }
  }
  if (assoc) {
    int num = 0;
    for (auto& d : zp.descs)
      if (not d->vals.empty()) ++num;
    if (not keep or num) {
      vec<str> aval;
      if (keep and num)
        if (auto h = ctx.params.lookup(*assoc); h and h->get().is_hash())
          for (auto* kv : sorted_entries(h->get().hash_ref())) {
            aval.push_back(kv->first);
            aval.push_back(kv->second);
          }
      for (auto& d : zp.descs) {
        if (d->vals.empty()) continue;
        aval.push_back("-" + d->name);
        // zsh writes a blank after each value and then the next value over
        // it: the values run together.
        str joined;
        for (Zoptval* v : d->vals)
          if (v->arg) joined += *v->arg;
        aval.push_back(joined);
      }
      // sethparam: the parameter is made an association.
      if (auto h = ctx.params.lookup(*assoc); not h or not h->get().is_hash()) {
        if (h and h->get().is_readonly()) {
          s::fprintf(stderr, "jfsh: read-only variable: %s\n", assoc->c_str());
          return 1;
        }
        (void)ctx.params.unset(*assoc);
        ctx.params.assign(*assoc, umap<str, str>{}, PM_HASH).scope = 0;
      }
      (void)set_array(ctx, *assoc, s::move(aval));
    }
  }
  if (del) {
    if (extract) pparams = s::move(kept);
    else         pparams.erase(pparams.begin(), pparams.begin() + static_cast<s::ptrdiff_t>(pp));
  }
  return 0;
}

}  // namespace

void register_zutil_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/zutil", [](ModuleFeatures& out, ExecContext&) {
    auto styles = s::make_shared<StyleTable>();
    out.builtins.push_back(BuiltinDef{"zformat", bin_zformat, BINF_NONE});
    out.builtins.push_back(BuiltinDef{"zparseopts", bin_zparseopts, BINF_NONE});
    out.builtins.push_back(BuiltinDef{"zregexparse", bin_zregexparse, BINF_NONE});
    out.builtins.push_back(BuiltinDef{
        "zstyle", [styles](vec<str>& a, ExecContext& c) { return bin_zstyle(a, c, *styles); },
        BINF_NONE});
  });
}

}  // namespace jfsh
