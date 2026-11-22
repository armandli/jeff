// The pattern compiler and CompiledPattern: CompiledPattern handles
// * ? [...] (a|b) a|b <n-m>, the globbing flags (#i #l #I #b #B #m #M #s #e
// #aN #cN,M) and, under EXTENDED_GLOB, x# x## ^pat a~b.  The VM
// (pattern_vm.cpp) matches them, and the backtracker (pattern_backtrack.cpp)
// those with (#aN) or zsh quirks the VM does not follow.
//
// TODO(glob/kshglob): KSH_GLOB's `@(…)`, `*(…)`, `+(…)`, `?(…)`, `!(…)`.

#include <jfsh/glob/glob.h>

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cwctype>

#include <algorithm>
#include <utility>

#include <jfsh/core/aliases.h>
#include <jfsh/core/stack.h>
#include <jfsh/core/utf8.h>

#include <glob/pattern_internal.h>
#include <glob/pattern_syntax.h>

namespace jfsh::pattern {

namespace {

int (*class_fn(strv name))(int) {
  if (name == "alpha")  return [](int c) { return s::isalpha(c); };
  if (name == "digit")  return [](int c) { return s::isdigit(c); };
  if (name == "alnum")  return [](int c) { return s::isalnum(c); };
  if (name == "upper")  return [](int c) { return s::isupper(c); };
  if (name == "lower")  return [](int c) { return s::islower(c); };
  if (name == "space")  return [](int c) { return s::isspace(c); };
  if (name == "blank")  return [](int c) { return s::isblank(c); };
  if (name == "punct")  return [](int c) { return s::ispunct(c); };
  if (name == "print")  return [](int c) { return s::isprint(c); };
  if (name == "graph")  return [](int c) { return s::isgraph(c); };
  if (name == "cntrl")  return [](int c) { return s::iscntrl(c); };
  if (name == "xdigit") return [](int c) { return s::isxdigit(c); };
  return nullptr;
}

// The globbing flags as a pattern is read; a group's changes end with it.
struct PatFlags {
  bool nocase  = false;
  bool lower   = false;
  int  approx  = 0;
  bool backref = false;
  bool matchref = false;   // (#m)
};

// The parser.  Patterns arrive in escaped form: a backslash makes the next
// character literal, everything else unescaped is pattern syntax.
struct PatternParser {
  struct Decoded {
    char32_t  c;
    s::size_t next;   // the position after it
  };
  // A branch, and the flags in force at its end.
  struct Branch {
    Seq      seq;
    PatFlags flags;
    // zsh would compare it as a plain string (its P_PURESTR): literals
    // only, read with no case flags or (#aN), and flags that change
    // nothing it matches.
    bool     pure = true;
    // The (#aN) that globbing flags at its end set (-1: none).
    int      end_reset = -1;
  };
  // The character at p[j].
  Decoded take(s::size_t j) const {
    if (not mb) return {static_cast<unsigned char>(p[j]), j + 1};
    s::size_t len = 1;
    char32_t c = utf8_decode(p, j, len);
    return {c, j + len};
  }

  // The literal character at p[j], with i moved past it.
  Node literal(s::size_t j) {
    Node n;
    auto [c, next] = take(j);
    n.ch  = c;
    n.bad = mb and c >= 0x80 and next == j + 1;
    i = next;
    return n;
  }

  bool failed() const { return too_deep or bad; }

  // Whether zsh's flags words would be the same.
  static bool same_flags(const PatFlags& a, const PatFlags& b) {
    return a.nocase == b.nocase and a.lower == b.lower and a.approx == b.approx
       and a.backref == b.backref and a.matchref == b.matchref;
  }

  // `[…]` starting at p[i] == '['.  Returns false (and leaves i) when the
  // bracket is never closed, so the caller treats `[` as a literal.
  bool parse_class(Node& n) {
    s::size_t j = i + 1;
    n.kind = Kind::Class;
    if (j < p.size() and (p[j] == '!' or p[j] == '^')) { n.class_negated = true; ++j; }
    // A `]` first is a member when another `]` closes the class, and
    // otherwise closes an empty one, as in zsh: `[]]`, but `[]` and `[!]`.
    if (j < p.size() and p[j] == ']' and p.find(']', j + 1) == strv::npos) {
      i = j + 1;
      return true;
    }
    bool first = true;
    while (j < p.size() and (p[j] != ']' or first)) {
      first = false;
      if (p[j] == '[' and j + 1 < p.size() and p[j + 1] == ':') {
        auto close = p.find(":]", j + 2);
        if (close != strv::npos) {
          str name(p.substr(j + 2, close - j - 2));
          if (auto fn = class_fn(name)) {
            n.classes.emplace_back(fn, ::wctype(name.c_str()));
            j = close + 2;
            continue;
          }
        }
      }
      if (p[j] == '\\' and j + 1 < p.size()) ++j;
      auto [lo, after] = take(j);
      j = after;
      char32_t hi = lo;
      if (j + 1 < p.size() and p[j] == '-' and p[j + 1] != ']') {
        ++j;
        if (p[j] == '\\' and j + 1 < p.size()) ++j;
        auto d = take(j);
        hi = d.c;
        j = d.next;
      }
      n.ranges.emplace_back(lo, hi);
    }
    if (j >= p.size()) return false;
    i = j + 1;
    return true;
  }

  // `<lo-hi>` with either bound optional.
  bool parse_range(Node& n) {
    s::size_t j = i + 1;
    auto digits = [&](bool& has, s::int64_t& v) {
      s::size_t s = j;
      while (j < p.size() and s::isdigit(static_cast<unsigned char>(p[j]))) ++j;
      has = j > s;
      if (has) v = s::strtoll(str(p.substr(s, j - s)).c_str(), nullptr, 10);
    };
    digits(n.has_lo, n.lo);
    if (j >= p.size() or p[j] != '-') return false;
    ++j;
    digits(n.has_hi, n.hi);
    if (j >= p.size() or p[j] != '>') return false;
    n.kind = Kind::Range;
    i = j + 1;
    return true;
  }

  // The alternatives of a group after its `(` (`in_group`), or of the whole
  // pattern, each with its `~` exclusions: in zsh `~` binds tighter than
  // `|`, so b~b|a is (b~b)|a.  False when the group is never closed or the
  // pattern is bad.
  bool parse_alts(Node& n, PatFlags fl, bool in_group) {
    n.kind = Kind::Group;
    for (;;) {
      // Each alternative starts with the group's flags.  Its exclusions
      // start, as in zsh, without (#aN) and with the group's flags inside a
      // group, with those the branch ends with at the top level.
      auto alt = parse_seq(in_group, fl);
      n.alts.push_back(s::move(alt.seq));
      n.alt_end_reset.push_back(alt.end_reset);
      if (not same_flags(alt.flags, fl)) n.restores = in_group;
      bool pure = alt.pure;
      n.alt_excludes.emplace_back();
      while (ext and i < p.size() and p[i] == '~' and not failed()) {
        ++i;
        PatFlags excl = in_group ? fl : alt.flags;
        excl.approx = 0;
        alt = parse_seq(in_group, excl);
        n.alt_excludes.back().push_back(s::move(alt.seq));
      }
      if (not in_group) {
        n.top = true;
        impl.pure = pure and n.alts.size() == 1 and n.alt_excludes[0].empty();
      }
      if (failed()) return false;
      if (i >= p.size()) return not in_group;
      ++i;   // `|`, or the `)` closing the group
      if (p[i - 1] == ')') return true;
    }
  }

  // `(…)` at p[i].  Groups nested deeper than the stack holds make the
  // whole pattern literal text (`too_deep`), so that neither parsing nor
  // matching runs off the end of the stack and the pattern stays
  // consistent.  False, with i left alone, when the group is never closed:
  // then `(` is text.
  bool parse_group(Node& n, PatFlags fl) {
    if (too_deep or stack_low()) { too_deep = true; return false; }
    s::size_t save = i;
    int saved_captures = impl.captures;
    ++i;
    if (fl.backref and impl.captures < MAX_CAPTURES) n.capture = impl.captures++;
    n.entry_epoch = epoch;
    bool outer_flags = s::exchange(level_flags, false);
    if (parse_alts(n, fl, true)) {
      // zsh puts its flags back at the end of a group whose own branches
      // changed them, not those of groups inside it.
      if (level_flags) ++epoch;
      level_flags = outer_flags;
      return true;
    }
    level_flags = outer_flags;
    i = save;
    n = Node{};
    impl.captures = saved_captures;
    return false;
  }

  // `(#…)` at p[i]: globbing flags, which change `fl` into the flags it
  // returns; an anchor, (#s) or (#e); or (#cN,M), repeating the piece just
  // before it (`can_count`).  Anything else is a bad pattern, as in zsh.
  PatFlags parse_flags(PatFlags fl, Seq& seq, bool can_count) {
    s::size_t close = p.find(')', i);
    if (close == strv::npos) { bad = true; return fl; }
    strv f = p.substr(i + 2, close - i - 2);
    i = close + 1;
    if (not f.empty() and f[0] == 'c') {
      parse_count(f.substr(1), seq, can_count);
      return fl;
    }
    PatFlags nf = fl;
    for (s::size_t k = 0; k < f.size(); ++k) {
      switch (f[k]) {
        case 'i': nf.nocase = true; nf.lower = false;

        break; case 'l': nf.lower = true; nf.nocase = false;

        break; case 'I': nf.nocase = nf.lower = false;

        break; case 'b': nf.backref = true;

        break; case 'B': nf.backref = false;

        break; case 'm': impl.set_match = nf.matchref = true;

        break; case 'M': nf.matchref = false;

        break; case 'u': case 'U':

        // `q` starts glob qualifiers, which the pattern ignores.
        break; case 'q': k = f.size();

        break; case 's': case 'e': {
          // An anchor stands alone.
          if (f.size() != 1) { bad = true; return fl; }
          Node n;
          n.kind   = f[k] == 's' ? Kind::AtStart : Kind::AtEnd;
          n.epoch  = epoch;
          n.approx = fl.approx;
          seq.push_back(s::move(n));
        }

        break; case 'a': {
          // A number of errors is required, up to 254.
          str num(f.substr(k + 1));
          char* end = nullptr;
          long v = s::strtol(num.c_str(), &end, 10);
          if (end == num.c_str() or v < 0 or v > 254) { bad = true; return fl; }
          nf.approx = static_cast<int>(v);
          k += static_cast<s::size_t>(end - num.c_str());
        }

        break; default:
          bad = true;
          return fl;
      }
    }
    if (nf.nocase != fl.nocase or nf.lower != fl.lower) {
      ++epoch;
      level_flags = true;
    }
    if (nf.approx > 0) impl.approx = true;
    return nf;
  }

  // The `N,M` of (#cN,M): N to M repeats of the last piece of `seq`.  A
  // missing N is 0, a missing M no limit, and N alone exactly N.
  void parse_count(strv spec, Seq& seq, bool can_count) {
    str text(spec);
    s::size_t k = 0;
    // As zsh's zstrtol: it takes whatever blanks, sign and digits are
    // there, so a lone sign (or blank) is 0 and only an empty number is
    // missing; a negative count is no limit for M and nothing for N.
    auto number = [&](int none) {
      s::size_t b = k;
      while (k < text.size() and (text[k] == ' ' or text[k] == '\t' or text[k] == '\n')) ++k;
      bool neg = k < text.size() and text[k] == '-';
      if (k < text.size() and (text[k] == '-' or text[k] == '+')) ++k;
      long v = 0;
      for (; k < text.size() and s::isdigit(static_cast<unsigned char>(text[k])); ++k)
        v = s::min(v * 10 + (text[k] - '0'), static_cast<long>(INT32_MAX));
      if (k == b) return none;
      return neg and v > 0 ? -1 : static_cast<int>(v);
    };
    int lo = number(0), hi = lo;
    if (k < text.size() and text[k] == ',') {
      ++k;
      hi = number(-1);
    }
    if (k != text.size() or not can_count) { bad = true; return; }
    Node rep;
    rep.kind    = Kind::Repeat;
    rep.counted = true;
    rep.min     = s::max(lo, 0);
    rep.max     = hi;
    rep.epoch   = seq.back().epoch;
    rep.approx  = seq.back().approx;
    rep.reset   = s::exchange(seq.back().reset, false);
    rep.sub     = s::make_shared<Node>(s::move(seq.back()));
    // zsh repeats only the last literal of a string.
    if (rep.sub->kind == Kind::Lit) rep.sub->run = impl.runs++;
    // The branches are where they stay now, so the points can name them.
    flag_points(*rep.sub, rep.leaks);
    rep.leak_epoch = epoch;
    if (not rep.leaks.empty()) impl.leaky = true;
    seq.back()  = s::move(rep);
  }

  // Where the flags change in the one branch of group `g` and in the
  // one-branch groups inside it, which put nothing back when they fail,
  // and where such a group's end puts them back.
  static void flag_points(const Node& g, vec<Node::FlagPoint>& out) {
    if (g.kind != Kind::Group or g.negate or g.alts.size() != 1) return;
    // A branch with a `~` is tested as zsh's exclusion, which puts the
    // flags back.
    if (not g.alt_excludes.empty() and not g.alt_excludes[0].empty()) return;
    const Seq& alt = g.alts[0];
    int prev = g.entry_epoch;
    bool changed = false;
    for (s::size_t j = 0; j < alt.size(); prev = alt[j++].epoch) {
      if (alt[j].epoch != prev) {
        out.push_back({&alt, j, alt[j].nocase, alt[j].lower});
        changed = true;
      }
      flag_points(alt[j], out);
    }
    // The group's end puts its own flags back.
    if (changed) out.push_back({&alt, alt.size(), g.nocase, g.lower});
  }

  // A branch: pieces up to a `|`, a `)` closing the group (`in_group`), a
  // `~` exclusion or the end.  `lead_tilde`: a `~` first is an exclusion
  // too, not text.
  Branch parse_seq(bool in_group, PatFlags fl, bool lead_tilde = false) {
    const PatFlags entry = fl;
    Seq seq;
    bool can_count = false;   // the last thing read is a piece (#c) may repeat
    bool any = false;         // a piece or globbing flags were read
    bool in_run = false;      // the last thing read is a literal of a string
    bool pure = true;         // see Branch::pure
    bool pure_last = false;   // the last piece read is a plain string
    bool reset = false;       // the next node gets Node::reset
    // Flags right at the end of the pattern are left for nothing, as zsh
    // does: they do not reach its end (`end_flags`).
    PatFlags end_flags = fl;
    while (i < p.size() and not failed()) {
      char c = p[i];
      if (c == '|' or (in_group and c == ')')) break;
      // `~` excludes what follows; with nothing after it (`*~`, `(*~|x)`)
      // it is a plain `~`.
      // In a group it may start an alternative: (~a) is nothing but `a`.
      if (ext and c == '~' and (any or in_group or lead_tilde) and i + 1 < p.size()
          and p[i + 1] != '|' and p[i + 1] != ')' and p[i + 1] != '~')
        break;
      any = true;
      if (ext and c == '(' and i + 1 < p.size() and p[i + 1] == '#') {
        bool first = i == 0;
        PatFlags before = fl;
        s::size_t anchors = seq.size();
        fl = parse_flags(fl, seq, can_count);
        bool last = i == p.size() and not first;
        end_flags = last ? before : fl;
        if (first) impl.head_approx = fl.approx;
        // zsh keeps flags at the very start in the pattern's header and
        // drops those at the very end; others that change something are
        // a node, which leaves a plain string only after one.
        bool node = not first and not last and not same_flags(before, fl);
        if (seq.size() != anchors) {
          pure = pure_last = false;
          seq.back().reset = s::exchange(reset, false);
        } else if (node) {
          if (not pure_last) pure = false;
          reset = true;
        }
        can_count = false;
        in_run = false;
        continue;
      }
      if (ext and c == '^') {
        // The rest of the branch becomes a group that matches anything
        // but it, as zsh's *~rest; inside a group the rest starts with the
        // group's flags, as an exclusion does.
        ++i;
        Node n;
        n.kind   = Kind::Group;
        n.negate = true;
        n.epoch  = epoch;
        n.approx = fl.approx;
        // Exclusions are tested without (#aN), but the branch goes on
        // with it: zsh's P_END is reached before the test.
        PatFlags rest_fl = in_group ? entry : fl;
        rest_fl.approx = 0;
        auto rest = parse_seq(in_group, rest_fl, true);
        n.alts.push_back(s::move(rest.seq));
        n.alt_excludes.emplace_back();
        n.alt_end_reset.push_back(-1);
        if (not in_group) {
          int approx = fl.approx;
          fl = rest.flags;
          fl.approx = approx;
        }
        end_flags = fl;
        n.reset = s::exchange(reset, false);
        seq.push_back(s::move(n));
        can_count = false;
        in_run = false;
        pure = pure_last = false;
        continue;
      }
      // A `#` repeats the piece before it; with none, it is bad.
      if (ext and c == '#') { bad = true; break; }
      int at = epoch;
      Node n;
      if (c == '\\' and i + 1 < p.size()) {
        n = literal(i + 1);
      } else if (c == '*') {
        n.kind = Kind::Star;
        while (i < p.size() and p[i] == '*') ++i;
      } else if (c == '?') {
        n.kind = Kind::Any;
        ++i;
      } else if (c == '[' and parse_class(n)) {
      } else if (c == '(' and parse_group(n, fl)) {
      } else if (c == '<' and parse_range(n)) {
      } else {
        if (failed()) break;
        n = literal(i);
      }
      n.nocase = fl.nocase;
      n.lower  = fl.lower;
      n.approx = fl.approx;
      n.epoch  = at;
      bool lit = n.kind == Kind::Lit;
      can_count = n.kind != Kind::Star;
      if (ext and i < p.size() and p[i] == '#') {
        // `*#` and `x###` are bad patterns in zsh.
        if (n.kind == Kind::Star) { bad = true; break; }
        Node rep;
        rep.kind = Kind::Repeat;
        rep.min  = 0;
        ++i;
        if (i < p.size() and p[i] == '#') { rep.min = 1; ++i; }
        if (i < p.size() and p[i] == '#') { bad = true; break; }
        rep.nocase = n.nocase;
        rep.lower  = n.lower;
        rep.approx = n.approx;
        rep.epoch  = n.epoch;
        if (lit) n.run = impl.runs++;
        rep.sub = s::make_shared<Node>(s::move(n));
        n = s::move(rep);
        can_count = false;
        lit = false;
      } else if (lit) {
        if (not in_run) ++impl.runs;
        n.run = impl.runs - 1;
      }
      in_run = lit;
      pure_last = lit and fl.approx == 0 and not fl.nocase and not fl.lower;
      if (not pure_last) pure = false;
      end_flags = fl;
      n.reset = s::exchange(reset, false);
      seq.push_back(s::move(n));
      ++impl.nodes;
    }
    return {s::move(seq), fl, pure, reset ? end_flags.approx : -1};
  }

  strv      p;
  bool      ext;
  CompiledPattern::Impl& impl;
  s::size_t i  = 0;
  bool      mb = false;
  bool      too_deep = false;
  bool      bad = false;
  int       epoch = 0;   // see Node::epoch
  // The branches of the group being read changed the case flags.
  bool      level_flags = false;
};

// The bytes a literal takes as written in the pattern.
s::size_t lit_bytes(const Node& n, bool mb) {
  if (not mb or n.bad or n.ch < 0x80) return 1;
  return n.ch < 0x800 ? 2 : n.ch < 0x10000 ? 3 : 4;
}

// zsh's matcher, when a branch of the whole pattern ends with `*` (or a
// one-character x# or x##) and literal text, compares that text with the
// last bytes of the subject instead of trying each place the `*` could
// end.  On characters the two agree; but a byte that is no character in
// the pattern can then match part of one in the subject (`*\xa9` matches
// é).  Only for whole matches.
opt<TailLiteral> tail_literal(const Seq& alt, bool mb) {
  if (alt.empty()) return s::nullopt;
  const Node& last = alt.back();
  auto same_flags = [&](const Node& n) {
    return n.nocase == last.nocase and n.lower == last.lower;
  };
  s::size_t r = alt.size();
  while (r > 0 and alt[r - 1].kind == Kind::Lit and alt[r - 1].approx == 0
         and same_flags(alt[r - 1]))
    --r;
  if (r == alt.size() or r == 0) return s::nullopt;
  const Node& x = alt[r - 1];
  bool simple = x.kind == Kind::Repeat and not x.counted and x.max < 0
            and single_char(*x.sub) and x.sub->approx == 0;
  if ((x.kind != Kind::Star and not simple) or not same_flags(x) or x.approx > 0)
    return s::nullopt;
  TailLiteral t;
  t.head.assign(alt.begin(), alt.begin() + static_cast<s::ptrdiff_t>(r - 1));
  t.rep = x;
  t.text.assign(alt.begin() + static_cast<s::ptrdiff_t>(r), alt.end());
  for (const auto& n : t.text) t.bytes += lit_bytes(n, mb);
  return t;
}

// Whether node n, matched from a place, goes on from at most one end, in
// one way.  Then zsh's (#cN,M) of it always knows where its last repeat
// began: its stale start differs from the true one only after another
// way of matching a repeat was tried.  Conservative: alternatives must
// start with literals that no one character matches both of.
bool calls_once(const Node& n, bool mb);

bool seq_once(const Seq& q, bool mb) {
  return s::all_of(q.begin(), q.end(), [&](const Node& n) { return calls_once(n, mb); });
}

// The characters, folded for case, that branch q may start with: false
// when it may start otherwise.
bool first_keys(const Seq& q, bool mb, vec<s::uint64_t>& out) {
  if (q.empty() or q[0].kind != Kind::Lit) return false;
  const Node& l = q[0];
  if (l.bad) {
    out.push_back((s::uint64_t{1} << 40) | l.ch);
    return true;
  }
  if (not mb) {
    out.push_back(static_cast<s::uint64_t>(s::tolower(static_cast<int>(l.ch))));
    return true;
  }
  auto fold = [](char32_t c) {
    auto w = static_cast<wint_t>(c);
    return static_cast<s::uint64_t>(::iswupper(w) ? ::towlower(w) : w);
  };
  out.push_back(fold(l.ch));
  if (l.lower and ::iswlower(static_cast<wint_t>(l.ch)))
    out.push_back(fold(static_cast<char32_t>(::towupper(static_cast<wint_t>(l.ch)))));
  return true;
}

bool calls_once(const Node& n, bool mb) {
  switch (n.kind) {
    case Kind::Lit: case Kind::Any: case Kind::Class: case Kind::AtStart: case Kind::AtEnd:
      return true;

    case Kind::Repeat:
      return n.counted and n.min == n.max and calls_once(*n.sub, mb);

    case Kind::Group: {
      if (n.negate) return false;
      for (s::size_t a = 0; a < n.alts.size(); ++a)
        if (not n.alt_excludes[a].empty() or not seq_once(n.alts[a], mb)) return false;
      if (n.alts.size() < 2) return true;
      vec<s::uint64_t> all;
      for (const auto& alt : n.alts) {
        vec<s::uint64_t> keys;
        if (not first_keys(alt, mb, keys)) return false;
        s::sort(keys.begin(), keys.end());
        keys.erase(s::unique(keys.begin(), keys.end()), keys.end());
        all.insert(all.end(), keys.begin(), keys.end());
      }
      s::sort(all.begin(), all.end());
      return s::adjacent_find(all.begin(), all.end()) == all.end();
    }

    default:
      return false;
  }
}

// Whether, outside the programs of `^` and `~`, a repeat of a group (zsh's
// P_WBRANCH, with marks) is in the operand of another repeat (`inside`).
// zsh keeps the inner one's marks across the outer one's repeats: a
// repeat of the inner one is not tried again from a place an attempt that
// failed already tried it from, even where what follows differs: on ba,
// (#b)(((|?)#)|()a)# matches the a with ()a, the inner repeat having been
// tried from there.  The VM does not follow.
bool nested_repeats(const Node& n, bool inside) {
  if (n.kind == Kind::Repeat) {
    if (inside and not n.counted and not single_char(*n.sub)) return true;
    return nested_repeats(*n.sub, true);
  }
  if (n.kind != Kind::Group or n.negate) return false;
  for (const auto& alt : n.alts)
    for (const auto& x : alt)
      if (nested_repeats(x, inside)) return true;
  return false;
}

// Whether a (#cN,M) under n repeats something that calls_once() is not
// sure of.
bool has_stale_count(const Node& n, bool mb) {
  if (n.kind == Kind::Repeat)
    return (n.counted and not calls_once(*n.sub, mb)) or has_stale_count(*n.sub, mb);
  if (n.kind != Kind::Group) return false;
  auto any = [&](const Seq& q) {
    return s::any_of(q.begin(), q.end(), [&](const Node& x) { return has_stale_count(x, mb); });
  };
  for (s::size_t a = 0; a < n.alts.size(); ++a) {
    if (any(n.alts[a])) return true;
    for (const auto& ex : n.alt_excludes[a])
      if (any(ex)) return true;
  }
  return false;
}

// Whether there is a repeat at or under n.
bool has_repeat(const Node& n) {
  if (n.kind == Kind::Repeat) return true;
  if (n.kind != Kind::Group) return false;
  for (const auto& alt : n.alts)
    for (const auto& x : alt)
      if (has_repeat(x)) return true;
  return false;
}

// Whether the VM follows the flags that the (#cN,M) under n leak
// (Node::leaks): each repeats something that matches in one way and holds
// no repeat, inside no repeat, and there is no `^` or `~`, whose programs
// run apart from the threads that carry the flags.
bool vm_follows_leaks(const Node& n, bool inside) {
  switch (n.kind) {
    case Kind::Repeat:
      if (not n.leaks.empty() and (inside or has_repeat(*n.sub))) return false;
      return vm_follows_leaks(*n.sub, true);

    case Kind::Group:
      if (n.negate) return false;
      for (s::size_t a = 0; a < n.alts.size(); ++a) {
        if (not n.alt_excludes[a].empty()) return false;
        for (const auto& x : n.alts[a])
          if (not vm_follows_leaks(x, inside)) return false;
      }
      return true;

    default:
      return true;
  }
}

// Which patterns the backtracker decides (see backtracked()).
void route(CompiledPattern::Impl& impl) {
  // (#aN) has only the backtracker.
  if (impl.approx) return;
  const Node& top = impl.main.front();
  impl.stale    = has_stale_count(top, impl.multibyte);
  impl.marks    = nested_repeats(top, false);
  impl.vm_leaks = impl.leaky and vm_follows_leaks(top, false);
}

// match_prefix() with (#aN), which only the backtracker runs, as zsh's
// igetmatch: the end its search with the end left open reaches first;
// for the shortest, then each shorter length of the text cut there.  A
// search that gives up finds nothing.
s::size_t approx_prefix(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                        bool longest, PatternMatch* info, PrefixSearch& how) {
  // Once it gave up in this search, the rest of it is not run.
  if (how.lost) return strv::npos;
  bool spent = false;
  opt<s::size_t> end = how.below;
  if (longest or how.below == strv::npos) {
    end = backtrack_open(impl, text, b, info, how, spent);
    if (not end) how.lost = true;
    if (not end or *end == strv::npos) return strv::npos;
    if (longest) return *end - b;
  }
  // The cut text: (#e) fails before the end of the string.
  Subject subject{text, text.size(), impl.multibyte};
  for (s::size_t t = b, next = 0; t < *end; t = next) {
    auto e = backtrack_open(impl, text, b, info, how, spent, t);
    if (not e) {
      how.lost = true;
      return strv::npos;
    }
    if (*e != strv::npos) return *e - b;
    subject.at(t, next);
  }
  return *end - b;
}

// The places from b where a match could end: after each character, as
// the matchers read them (a byte that starts none is one).
Positions char_ends(const CompiledPattern::Impl& impl, strv text, s::size_t b) {
  Subject subject{text, text.size(), impl.multibyte};
  Positions ends;
  for (s::size_t e = b;;) {
    ends.push_back(e);
    if (e >= text.size()) return ends;
    subject.at(e, e);
  }
}

// The characters in text[b, e) as the matcher counts them: under MULTIBYTE
// a UTF-8 character is one, and so is a byte that starts none.  zsh's count
// of the places in a match keeps its conversion state from b, which a byte
// that starts no character leaves in error (`stray`): every byte after it
// counts as one.
s::size_t chars_in(strv text, s::size_t b, s::size_t e, bool mb, bool stray = false) {
  if (not mb) return e - b;
  s::size_t n = 0;
  for (s::size_t i = b; i < e; ++n) {
    if (static_cast<unsigned char>(text[i]) < 0x80) {
      ++i;
      continue;
    }
    s::size_t len = 1;
    utf8_decode(text, i, len);
    if (len == 1 and stray) return n + (e - i);
    i += len;
  }
  return n;
}

// Whether the backtracker decides for `impl`: zsh's counter keeps a stale
// start, its marks matter, or its flags leak where the VM does not follow.
bool backtracked(const CompiledPattern::Impl& impl) {
  return impl.stale or impl.marks or (impl.leaky and not impl.vm_leaks);
}

// Whether the VM's matches include all zsh's, so that where it finds none
// there is none: zsh's stale start and its marks only reject matches,
// leaked flags the VM does not follow may allow more.
bool vm_filters(const CompiledPattern::Impl& impl) {
  return not impl.leaky or impl.vm_leaks;
}

// match_prefix() where the backtracker decides (see backtracked()), or
// with `all` (for tests) for every pattern.  When it gives up, the VM's
// answer.
s::size_t backtracked_prefix(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                             bool longest, PatternMatch* info, PrefixSearch& how, bool all) {
  bool filter = not all and vm_filters(impl);
  // Once the backtracker ran out of steps in this search, the VM.
  if (how.gave_up) return vm_prefix(impl, text, b, longest, info, how);
  if (longest) {
    PatternMatch found;
    s::size_t len = strv::npos;
    if (filter) {
      len = vm_prefix(impl, text, b, true, info ? &found : nullptr, how);
      if (len == strv::npos) return len;
    }
    bool spent = false;
    if (auto end = backtrack_open(impl, text, b, info, how, spent))
      return *end == strv::npos ? *end : *end - b;
    if (spent) how.gave_up = true;
    // Given up: the VM's answer, found already when it filtered.
    if (not filter) return vm_prefix(impl, text, b, true, info, how);
    if (info) *info = s::move(found);
    return len;
  }
  // The shortest end the backtracker agrees with.
  Positions ends = filter ? vm_ends(impl, text, b, how) : char_ends(impl, text, b);
  for (s::size_t e : ends) {
    bool spent = false;
    if (not how.gave_up) {
      switch (backtrack(impl, text, b, e, info, false, how.notstart, how.notend, &spent)) {
        case Verdict::Yes: return e - b;
        case Verdict::No: continue;
        default: break;
      }
      if (spent) how.gave_up = true;
    }
    // Given up: the VM's answer, there (where the VM found an end) or,
    // without a filter, for the whole search.
    if (not filter) return vm_prefix(impl, text, b, false, info, how);
    if (vm_matches(impl, text, b, e, info, false, how.notstart, how.notend)) return e - b;
  }
  return strv::npos;
}

}  // namespace

bool Subject::continues(s::size_t p) const {
  for (s::size_t q = p; q > origin and p - q < 3;) {
    if ((static_cast<unsigned char>(s[--q]) & 0xc0) == 0x80) continue;
    s::size_t next = 0;
    at(q, next);
    return next > p;
  }
  return false;
}

s::size_t bracket_end(strv p, s::size_t i) {
  CompiledPattern::Impl impl;
  PatternParser pp{p, false, impl, i};
  Node n;
  return pp.parse_class(n) ? pp.i : strv::npos;
}

}  // namespace jfsh::pattern

namespace jfsh {

using pattern::backtrack;
using pattern::backtracked;
using pattern::backtracked_prefix;
using pattern::approx_prefix;
using pattern::chars_in;
using pattern::compile_vm;
using pattern::Kind;
using pattern::Node;
using pattern::PatFlags;
using pattern::PatternParser;
using pattern::route;
using pattern::tail_literal;
using pattern::Verdict;
using pattern::vm_filters;
using pattern::vm_find_start;
using pattern::vm_matches;
using pattern::vm_prefix;

bool locale_multibyte() noexcept { return MB_CUR_MAX > 1; }

CompiledPattern CompiledPattern::compile(strv pattern, bool case_insensitive,
                                         bool extended, bool multibyte) {
  auto impl = s::make_shared<Impl>();
  impl->multibyte = multibyte;
  PatternParser pp{pattern, extended, *impl};
  pp.mb = multibyte;
  PatFlags fl;
  fl.nocase = case_insensitive;
  Node top;
  pp.parse_alts(top, fl, false);
  CompiledPattern cp;
  if (pp.bad) {
    auto bad = s::make_shared<Impl>();
    bad->bad = true;
    cp.mImpl = s::move(bad);
    return cp;
  }
  if (pp.too_deep) {
    // The whole pattern as literal text.
    auto lit = s::make_shared<Impl>();
    lit->multibyte = multibyte;
    PatternParser lp{pattern, false, *lit};
    lp.mb = multibyte;
    Node all;
    all.kind = Kind::Group;
    all.alts.emplace_back();
    all.alt_excludes.emplace_back();
    while (lp.i < pattern.size()) {
      Node n = lp.literal(lp.i);
      n.nocase = case_insensitive;
      all.alts[0].push_back(s::move(n));
    }
    all.alt_end_reset.push_back(-1);
    all.top = true;
    lit->main.push_back(s::move(all));
    lit->tails.resize(1);
    route(*lit);
    compile_vm(*lit);
    cp.mImpl = s::move(lit);
    return cp;
  }
  for (s::size_t a = 0; a < top.alts.size(); ++a)
    impl->tails.push_back(top.alt_excludes[a].empty() ? tail_literal(top.alts[a], multibyte)
                                                      : s::nullopt);
  impl->main.push_back(s::move(top));
  route(*impl);
  compile_vm(*impl);
  cp.mImpl = s::move(impl);
  return cp;
}

CompiledPattern CompiledPattern::engine(PatternEngine e) const {
  CompiledPattern cp = *this;
  cp.mEngine = e;
  return cp;
}

bool CompiledPattern::matches(strv text, PatternMatch* info) const {
  return matches(text, 0, text.size(), info);
}

bool CompiledPattern::matches(strv text, s::size_t b, s::size_t e, PatternMatch* info) const {
  const Impl& impl = *mImpl;
  if (impl.bad) return false;
  if (impl.approx) return backtrack(impl, text, b, e, info) == Verdict::Yes;
  bool all = mEngine == PatternEngine::Backtrack;
  if (all or (mEngine == PatternEngine::Auto and backtracked(impl))) {
    bool filter = not all and vm_filters(impl);
    PatternMatch found;
    if (filter and not vm_matches(impl, text, b, e, info ? &found : nullptr)) return false;
    switch (backtrack(impl, text, b, e, info)) {
      case Verdict::Yes: return true;
      case Verdict::No: return false;
      default: break;
    }
    // Given up: the VM's answer, found already when it filtered.
    if (filter) {
      if (info) *info = s::move(found);
      return true;
    }
  }
  return vm_matches(impl, text, b, e, info);
}

bool CompiledPattern::matches(strv text, s::size_t b, s::size_t e, PatternMatch* info,
                              PrefixSearch& search) const {
  const Impl& impl = *mImpl;
  if (not impl.approx or impl.bad) return matches(text, b, e, info);
  if (search.lost) return false;
  Verdict v = backtrack(impl, text, b, e, info);
  if (v == Verdict::GaveUp) search.lost = true;
  return v == Verdict::Yes;
}

s::size_t CompiledPattern::match_prefix(strv text, bool longest, PatternMatch* info) const {
  return match_prefix(text, 0, longest, info);
}

s::size_t CompiledPattern::match_prefix(strv text, s::size_t b, bool longest,
                                        PatternMatch* info, PrefixSearch* search) const {
  const Impl& impl = *mImpl;
  if (impl.bad) return strv::npos;
  PrefixSearch none;
  PrefixSearch& how = search ? *search : none;
  if (impl.approx) return approx_prefix(impl, text, b, longest, info, how);
  if (mEngine == PatternEngine::Backtrack)
    return backtracked_prefix(impl, text, b, longest, info, how, true);
  if (mEngine == PatternEngine::Auto and backtracked(impl))
    return backtracked_prefix(impl, text, b, longest, info, how, false);
  return vm_prefix(impl, text, b, longest, info, how);
}

bool CompiledPattern::ordered_ends() const noexcept {
  const Impl& impl = *mImpl;
  if (impl.bad or impl.approx) return false;
  return mEngine == PatternEngine::Vm or (mEngine == PatternEngine::Auto and not backtracked(impl));
}

s::size_t CompiledPattern::find_start(strv text, s::size_t from, s::size_t until, bool whole,
                                      bool last, PrefixSearch& search,
                                      PatternMatch* info) const {
  const Impl& impl = *mImpl;
  if (impl.bad) return strv::npos;
  if (not whole) search.notstart = false;
  if (ordered_ends())
    return vm_find_start(impl, text, from, until, whole, last, search.notend, info);
  auto hit = [&](s::size_t b) {
    if (whole) return matches(text, b, text.size(), info, search);
    search.notstart = false;
    return match_prefix(text, b, true, nullptr, &search) != strv::npos;
  };
  pattern::Subject subject{text, text.size(), impl.multibyte};
  subject.origin = from;
  until = s::min(until, text.size() + 1);
  if (from >= until) return strv::npos;
  if (not last) {
    for (s::size_t b = from, next = 0; b < until; b = next) {
      if (hit(b)) return b;
      if (search.lost or b == text.size()) break;
      subject.at(b, next);
    }
    return strv::npos;
  }
  for (s::size_t b = until - 1; not search.lost; --b) {
    while (b > from and not subject.boundary(b)) --b;
    if (hit(b)) return b;
    if (b == from) break;
  }
  return strv::npos;
}

bool CompiledPattern::backrefs() const noexcept { return mImpl->captures > 0; }

bool CompiledPattern::approximate() const noexcept { return mImpl->approx; }

bool CompiledPattern::bad() const noexcept { return mImpl->bad; }

bool CompiledPattern::multibyte() const noexcept { return mImpl->multibyte; }

void set_match_params(ParamTable& params, const CompiledPattern& pat, strv text,
                      s::size_t start, s::size_t len, const PatternMatch& m,
                      bool ksh_arrays, s::size_t start_char) {
  // zsh counts characters for the indexes.
  bool mb = pat.multibyte();
  s::int64_t base = ksh_arrays ? 0 : 1;
  if (start_char == strv::npos) start_char = chars_in(text, 0, start, mb);
  auto index = [&](s::size_t off) {
    return static_cast<s::int64_t>(start_char + chars_in(text, start, start + off, mb, true))
           + base;
  };
  if (pat.backrefs()) {
    vec<str> match, mbegin, mend;
    for (auto [b, e] : m.groups) {
      if (b == strv::npos) {
        match.emplace_back();
        mbegin.emplace_back("-1");
        mend.emplace_back("-1");
        continue;
      }
      match.emplace_back(text.substr(start + b, e - b));
      mbegin.push_back(s::to_string(index(b)));
      mend.push_back(s::to_string(index(e) - 1));
    }
    (void)params.set_array("match", s::move(match));
    (void)params.set_array("mbegin", s::move(mbegin));
    (void)params.set_array("mend", s::move(mend));
  }
  if (pat.sets_match()) {
    (void)params.set_scalar("MATCH", str(text.substr(start, len)));
    params.assign("MBEGIN", index(0), PM_INTEGER);
    params.assign("MEND", index(len) - 1, PM_INTEGER);
  }
}

bool CompiledPattern::sets_match() const noexcept { return mImpl->set_match; }

bool pattern_match(strv pattern, strv text,
                   bool case_insensitive, bool extended) noexcept {
  try {
    return CompiledPattern::compile(pattern, case_insensitive, extended).matches(text);
  } catch (...) {
    return false;
  }
}

bool ext_pattern_match(strv pattern, strv text) noexcept {
  return pattern_match(pattern, text, false, true);
}

bool has_glob_chars(strv s, bool extended) noexcept {
  for (s::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\') { ++i; continue; }
    // An unquoted `|` comes only from $~var: alternatives (a|b).
    if (c == '*' or c == '?' or c == '|') return true;
    // `[` and `(` are pattern characters only when closed; a lone `[` is
    // the test command.
    if (c == '[') {
      CompiledPattern::Impl impl;
      PatternParser pp{s, false, impl, i};
      Node n;
      if (pp.parse_class(n)) return true;
    }
    if (c == '(' and s.find(')', i + 1) != strv::npos) return true;
    if (c == '<') {
      CompiledPattern::Impl impl;
      PatternParser pp{s, false, impl, i};
      Node n;
      if (pp.parse_range(n)) return true;
    }
    // `~` alone makes no pattern (`x~y`, `rm foo.c~`), as zsh's haswilds.
    if (extended and (c == '^' or c == '#')) return true;
  }
  return false;
}

}  // namespace jfsh
