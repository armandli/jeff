#include <jfsh/glob/glob.h>

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <functional>

#include <jfsh/core/aliases.h>
#include <jfsh/core/sort.h>
#include <jfsh/core/stack.h>
#include <jfsh/core/utf8.h>

namespace jfsh {

// ---------------------------------------------------------------------------
// Pattern compiler.  Patterns arrive in escaped form: a backslash makes the
// next character literal, everything else unescaped is pattern syntax.
// ---------------------------------------------------------------------------

using PatternSeq = vec<CompiledPattern::Node>;

struct CompiledPattern::Node {
  enum class Kind : s::uint8_t {
    Lit, Any, Star, Class, Group, Range, Repeat, AtStart, AtEnd,
  };

  Kind kind = Kind::Lit;
  char ch   = 0;                                         // Lit

  bool class_negated = false;                            // Class
  vec<s::pair<char32_t, char32_t>> ranges;
  vec<s::pair<int (*)(int), wctype_t>> classes;

  vec<PatternSeq> alts;                          // Group
  // Under EXTENDED_GLOB, per alternative: its `~` exclusions, and whether
  // it began with `^` (anything but it).
  vec<vec<PatternSeq>> alt_excludes;
  vec<bool>            alt_negate;
  int capture = -1;                              // (#b): its backreference

  bool       has_lo = false, has_hi = false;              // Range <lo-hi>
  s::int64_t lo = 0, hi = 0;                              // and (#cN,M)

  sptr<Node> sub;                             // Repeat x# / x## / x(#cN,M)
  int min = 0;
  int max = -1;

  // The globbing flags in force here: (#i) (#l) and (#aN).
  bool nocase = false;
  bool lower  = false;
  int  approx = 0;
};

struct CompiledPattern::Impl {
  PatternSeq      main;
  vec<PatternSeq> excludes;   // a~b~c: `b` and `c`
  bool            negate = false;   // ^pat
  int             captures = 0;     // groups under (#b)
  bool            set_match = false;  // (#m) seen
  bool            approx = false;     // (#aN) with N > 0 seen
  int             end_approx = 0;   // (#aN) in force at the end
  bool            multibyte = false;   // ? and [...] take a UTF-8 character
};

namespace {

using Node = CompiledPattern::Node;
using Seq  = PatternSeq;
using Kind = Node::Kind;

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
};

struct PatternParser {
  struct Decoded {
    char32_t  c;
    s::size_t next;   // the position after it
  };
  // The character at p[j].
  Decoded take(s::size_t j) const {
    if (not mb) return {static_cast<unsigned char>(p[j]), j + 1};
    s::size_t len = 1;
    char32_t c = utf8_decode(p, j, len);
    return {c, j + len};
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

  // Groups nested deeper than the stack holds make the whole pattern
  // literal text (`too_deep`), so that neither parsing nor matching runs
  // off the end of the stack and the pattern stays consistent.
  bool parse_group(Node& n, PatFlags fl) {
    if (too_deep or stack_low()) { too_deep = true; return false; }
    s::size_t save = i;
    int saved_captures = impl.captures;
    ++i;
    n.kind = Kind::Group;
    if (fl.backref) n.capture = impl.captures++;
    for (;;) {
      bool negate = ext and i < p.size() and p[i] == '^';
      if (negate) ++i;
      n.alts.push_back(parse_seq(true, fl));
      n.alt_negate.push_back(negate);
      n.alt_excludes.emplace_back();
      while (ext and i < p.size() and p[i] == '~') {
        ++i;
        n.alt_excludes.back().push_back(parse_seq(true, fl));
      }
      if (i >= p.size() or too_deep) {
        i = save;
        n.alts.clear();
        n.alt_negate.clear();
        n.alt_excludes.clear();
        n.capture = -1;
        impl.captures = saved_captures;
        return false;
      }
      if (p[i] == ')') { ++i; return true; }
      ++i;  // '|'
    }
  }

  // `(#flags)` at p[i]: changes `fl`, or (for `(#s)`, `(#e)`) makes an
  // anchor node; `(#cN,M)` repeats the node before it.  False when it
  // isn't one.
  bool parse_flags(PatFlags& fl, Seq& seq) {
    s::size_t close = p.find(')', i);
    if (close == strv::npos) return false;
    strv f = p.substr(i + 2, close - i - 2);
    if (f.empty()) return false;
    PatFlags nf = fl;
    opt<Node> anchor;
    for (s::size_t k = 0; k < f.size(); ++k) {
      switch (f[k]) {
        case 'i': nf.nocase = true; nf.lower = false;
        break; case 'l': nf.lower = true; nf.nocase = false;
        break; case 'I': nf.nocase = nf.lower = false;
        break; case 'b': nf.backref = true;
        break; case 'B': nf.backref = false;
        break; case 'm': impl.set_match = true;
        break; case 'M': case 'u': case 'U': case 'q':
        break; case 's': case 'e': {
          Node n;
          n.kind = f[k] == 's' ? Kind::AtStart : Kind::AtEnd;
          anchor = n;
        }
        break; case 'a': {
          s::size_t b = k + 1;
          while (k + 1 < f.size() and s::isdigit(static_cast<unsigned char>(f[k + 1]))) ++k;
          nf.approx = b <= k ? s::atoi(str(f.substr(b, k - b + 1)).c_str()) : 1;
        }
        break; case 'c': {
          if (seq.empty()) return false;
          s::size_t b = k + 1;
          str spec(f.substr(b));
          auto comma = spec.find(',');
          Node rep;
          rep.kind = Kind::Repeat;
          rep.min = s::atoi(spec.c_str());
          rep.max = comma == str::npos ? rep.min
                  : comma + 1 < spec.size() ? s::atoi(spec.c_str() + comma + 1) : -1;
          rep.sub = s::make_shared<Node>(s::move(seq.back()));
          seq.back() = s::move(rep);
          k = f.size();
        }
        break; default:
          return false;
      }
    }
    fl = nf;
    if (fl.approx > 0) impl.approx = true;
    i = close + 1;
    if (anchor) seq.push_back(*anchor);
    return true;
  }

  // `top_bar`: at the top level, `|` separates alternatives (a|b).
  Seq parse_seq(bool in_group, PatFlags fl, bool top_bar = false) {
    Seq seq;
    while (i < p.size() and not too_deep) {
      char c = p[i];
      if (in_group and (c == '|' or c == ')')) break;
      if (top_bar and c == '|') break;
      // `~` excludes what follows; with nothing after it (`*~`, `(*~|x)`)
      // it is a plain `~`.
      // In a group it may start an alternative: (~a) is nothing but `a`.
      if (ext and c == '~' and (not seq.empty() or in_group) and i + 1 < p.size()
          and p[i + 1] != '|' and p[i + 1] != ')')
        break;
      if (ext and c == '(' and i + 1 < p.size() and p[i + 1] == '#' and parse_flags(fl, seq))
        continue;
      Node n;
      if (c == '\\' and i + 1 < p.size()) {
        n.ch = p[i + 1];
        i += 2;
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
        n = Node{};
        n.ch = c;
        ++i;
      }
      n.nocase = fl.nocase;
      n.lower  = fl.lower;
      n.approx = fl.approx;
      if (ext and i < p.size() and p[i] == '#') {
        Node rep;
        rep.kind = Kind::Repeat;
        rep.min  = 0;
        ++i;
        if (i < p.size() and p[i] == '#') { rep.min = 1; ++i; }
        rep.sub = s::make_shared<Node>(s::move(n));
        n = s::move(rep);
      }
      seq.push_back(s::move(n));
    }
    if (not in_group) impl.end_approx = fl.approx;
    return seq;
  }

  strv      p;
  bool      ext;
  CompiledPattern::Impl& impl;
  s::size_t i  = 0;
  bool      mb = false;
  bool      too_deep = false;
};

// Whether string byte `c` matches pattern byte `pc` under the node's
// flags: (#i) either case, (#l) a lower-case `pc` either case.
bool same_char(const Node& n, unsigned char c, unsigned char pc) {
  if (c == pc) return true;
  if (n.nocase or (n.lower and s::islower(pc)))
    return s::tolower(c) == s::tolower(pc);
  return false;
}

// Whether character `c` is in the class; with `mb` it is a UTF-8
// character's code point.
bool class_has(const Node& n, char32_t c, bool mb) {
  bool nocase = n.nocase or n.lower;
  auto in_ranges = [&](char32_t x) {
    for (auto [lo, hi] : n.ranges)
      if (x >= lo and x <= hi) return true;
    return false;
  };
  bool in = in_ranges(c);
  if (not in and nocase) {
    char32_t l = mb ? static_cast<char32_t>(::towlower(static_cast<wint_t>(c)))
                    : static_cast<char32_t>(s::tolower(static_cast<int>(c)));
    char32_t u = mb ? static_cast<char32_t>(::towupper(static_cast<wint_t>(c)))
                    : static_cast<char32_t>(s::toupper(static_cast<int>(c)));
    in = in_ranges(l) or in_ranges(u);
  }
  if (not in)
    for (auto [fn, wt] : n.classes)
      if (mb and c >= 0x80 ? ::iswctype(static_cast<wint_t>(c), wt) != 0
                           : c < 0x100 and fn(static_cast<int>(c)) != 0) {
        in = true;
        break;
      }
  return in != n.class_negated;
}

bool single_char(const Node& n) {
  return n.kind == Kind::Lit or n.kind == Kind::Any or n.kind == Kind::Class;
}

// The subject both matchers read: s[.., lim), with `s` the whole string so
// that (#s) and (#e) are its ends.
struct Subject {
  // The character at p (< lim) and the position after it.  Under `mb` a
  // UTF-8 character is one; a byte that starts none, or one cut off by
  // lim, is a character of its own.
  char32_t at(s::size_t p, s::size_t& next) const {
    auto b = static_cast<unsigned char>(s[p]);
    if (not mb or b < 0x80) { next = p + 1; return b; }
    s::size_t len = 1;
    char32_t c = utf8_decode(s, p, len);
    if (len == 0 or p + len > lim) { next = p + 1; return b; }
    next = p + len;
    return c;
  }
  // Whether a character may start at p.
  bool boundary(s::size_t p) const {
    return not mb or p >= lim or (static_cast<unsigned char>(s[p]) & 0xc0) != 0x80;
  }
  // Whether one character of `n` (a Lit, Any or Class) matches at p.
  bool one(const Node& n, s::size_t p, s::size_t& next) const {
    if (p >= lim) return false;
    if (n.kind == Kind::Lit) {
      next = p + 1;
      return same_char(n, static_cast<unsigned char>(s[p]), static_cast<unsigned char>(n.ch));
    }
    char32_t c = at(p, next);
    return n.kind == Kind::Any or class_has(n, c, mb);
  }

  strv      s;
  s::size_t lim = 0;
  bool      mb  = false;
};

// The positions a pattern can have reached in the subject, ascending and
// without repeats.
using Positions = vec<s::size_t>;

Positions normalized(Positions p) {
  if (s::is_sorted(p.begin(), p.end()) and s::adjacent_find(p.begin(), p.end()) == p.end())
    return p;
  s::sort(p.begin(), p.end());
  p.erase(s::unique(p.begin(), p.end()), p.end());
  return p;
}

Positions joined(const Positions& a, const Positions& b) {
  Positions out;
  out.reserve(a.size() + b.size());
  s::set_union(a.begin(), a.end(), b.begin(), b.end(), s::back_inserter(out));
  return out;
}

Positions without(const Positions& a, const Positions& b) {
  Positions out;
  s::set_difference(a.begin(), a.end(), b.begin(), b.end(), s::back_inserter(out));
  return out;
}

bool has(const Positions& set, s::size_t p) {
  return s::binary_search(set.begin(), set.end(), p);
}

// For patterns without (#b) groups or (#aN): runs the pattern over the
// set of positions it may be at, one node at a time, as a Thompson NFA
// does.  No backtracking, so time is bounded by pattern size times
// subject length and the stack only grows with the nesting of groups in
// the pattern, never with the subject.
struct SetMatcher : Subject {
  // The positions after `n` has matched from any of `set`.
  Positions node(const Node& n, const Positions& set) const {
    Positions out;
    switch (n.kind) {
      case Kind::Lit: case Kind::Any: case Kind::Class:
        for (s::size_t p : set) {
          s::size_t next = 0;
          if (one(n, p, next)) out.push_back(next);
        }
        if (mb) out = normalized(s::move(out));
      break; case Kind::Star:
        if (set.empty()) break;
        for (s::size_t p = set.front(), k = 0; p <= lim; ++p) {
          bool listed = k < set.size() and set[k] == p;
          if (listed) ++k;
          if (listed or boundary(p)) out.push_back(p);
        }
      break; case Kind::AtStart:
        if (not set.empty() and set.front() == 0) out.push_back(0);
      break; case Kind::AtEnd:
        if (has(set, s.size())) out.push_back(s.size());
      break; case Kind::Group:
        // Matching nests as the pattern did; past the stack it fails.
        if (stack_low()) break;
        for (s::size_t a = 0; a < n.alts.size(); ++a) {
          bool negate = a < n.alt_negate.size() and n.alt_negate[a];
          bool excl   = a < n.alt_excludes.size() and not n.alt_excludes[a].empty();
          if (not negate and not excl) {
            out = joined(out, seq(n.alts[a], set));
            continue;
          }
          // `^x` and `x~y` depend on where the alternative started.
          for (s::size_t p : set) {
            Positions part = seq(n.alts[a], Positions{p});
            if (negate) {
              Positions rest;
              for (s::size_t e = p; e <= lim; ++e)
                if (boundary(e) and not has(part, e)) rest.push_back(e);
              part = s::move(rest);
            }
            if (excl)
              for (const auto& ex : n.alt_excludes[a])
                part = without(part, seq(ex, Positions{p}));
            out = joined(out, part);
          }
        }
      break; case Kind::Range:
        for (s::size_t p : set) {
          s::uint64_t v = 0;
          bool big = false;
          for (s::size_t e = p; e < lim and s::isdigit(static_cast<unsigned char>(s[e])); ++e) {
            unsigned d = static_cast<unsigned>(s[e] - '0');
            if (v > (INT64_MAX - d) / 10) big = true;
            else v = v * 10 + d;
            s::int64_t x = big ? INT64_MAX : static_cast<s::int64_t>(v);
            if ((not n.has_lo or x >= n.lo) and (not n.has_hi or x <= n.hi)) out.push_back(e + 1);
            else if (n.has_hi and x > n.hi) break;
          }
        }
        out = normalized(s::move(out));
      break; case Kind::Repeat:
        out = repeat(n, set);
      break; default:
        assert(false);  // should never get here
    }
    return out;
  }

  // x# (min 0), x## (min 1) and x(#cN,M): every position after from `min`
  // to `max` (no limit when negative) matches of the sub-pattern.
  Positions repeat(const Node& n, const Positions& set) const {
    const Node& sub = *n.sub;
    Positions out;
    if (n.max < 0 and n.min <= 1 and single_char(sub)) {
      // A run of single characters: from each start, every position up to
      // where the run ends.  A start inside an earlier run adds nothing.
      bool any = false;
      s::size_t done = 0;   // where the last run ended
      for (s::size_t p : set) {
        if (any and p <= done) continue;
        if (n.min == 0) out.push_back(p);
        s::size_t q = p, next = 0;
        while (one(sub, q, next)) out.push_back(q = next);
        done = q;
        any = true;
      }
      return normalized(s::move(out));
    }
    Positions cur = set;
    int count = 0;
    for (; count < n.min and not cur.empty(); ++count) cur = node(sub, cur);
    if (count < n.min) return {};
    if (n.max >= 0) {
      out = cur;
      for (; count < n.max and not cur.empty(); ++count) {
        cur = node(sub, cur);
        out = joined(out, cur);
      }
      return out;
    }
    // Any number more: every position reachable from `cur`.
    vec<bool> seen(lim + 1, false);
    for (s::size_t p : cur) { seen[p] = true; out.push_back(p); }
    Positions frontier = s::move(cur);
    while (not frontier.empty()) {
      Positions fresh;
      for (s::size_t p : node(sub, frontier))
        if (not seen[p]) { seen[p] = true; fresh.push_back(p); }
      out.insert(out.end(), fresh.begin(), fresh.end());
      frontier = s::move(fresh);
    }
    return normalized(s::move(out));
  }

  Positions seq(const Seq& q, Positions set) const {
    for (const auto& n : q) {
      if (set.empty()) break;
      set = node(n, set);
    }
    return set;
  }
};

using Cont = func<bool(s::size_t)>;

// For patterns with (#b) groups or (#aN), which need to know how a match
// went: a backtracking matcher over s[.., lim).  `errs` counts the (#aN)
// errors made so far; `caps` the (#b) groups' extents, put back as
// alternatives fail.  It recurses per character, so a subject too long
// for the stack fails to match rather than overflowing it.
struct Matcher : Subject {
  // A one-character node, with (#aN) the errors it may absorb: a wrong
  // character, a missing one, an extra one.
  bool single(const Node& n, s::size_t pos, const Cont& k) const {
    if (stack_low()) return false;
    s::size_t next = 0;
    if (one(n, pos, next) and k(next)) return true;
    if (n.approx <= errs) return false;
    if (pos < lim) at(pos, next);
    ++errs;
    bool ok = (pos < lim and k(next)) or k(pos)
           or (pos < lim and single(n, next, k));
    --errs;
    return ok;
  }

  bool node(const Node& n, s::size_t pos, const Cont& k) const {
    // Each node matched recurses (x# once per repeat), so a subject too
    // long for the stack fails here rather than overflowing it.
    if (stack_low()) return false;
    switch (n.kind) {
      case Kind::Lit: case Kind::Any: case Kind::Class:
        return single(n, pos, k);
      case Kind::Star:
        // Greedy, as zsh's: the longest run first, so (#b)(*)X(*) on
        // aXbXc gives aXb and c.
        for (s::size_t e = lim + 1; e-- > pos;)
          if (boundary(e) and k(e)) return true;
        return false;
      case Kind::AtStart:
        return pos == 0 and k(pos);
      case Kind::AtEnd:
        return pos == s.size() and k(pos);   // (#e): the end of the whole string
      case Kind::Group:
        for (s::size_t a = 0; a < n.alts.size(); ++a) {
          const Seq& alt = n.alts[a];
          auto finish = [&](s::size_t e) {
            if (a < n.alt_excludes.size())
              for (const auto& ex : n.alt_excludes[a])
                if (spans(ex, pos, e)) return false;
            if (n.capture < 0) return k(e);
            auto saved = caps[static_cast<s::size_t>(n.capture)];
            caps[static_cast<s::size_t>(n.capture)] = {pos, e};
            if (k(e)) return true;
            caps[static_cast<s::size_t>(n.capture)] = saved;
            return false;
          };
          if (a < n.alt_negate.size() and n.alt_negate[a]) {
            for (s::size_t e = lim + 1; e-- > pos;)
              if (boundary(e) and not spans(alt, pos, e) and finish(e)) return true;
            continue;
          }
          if (seq(alt, 0, pos, finish)) return true;
        }
        return false;
      case Kind::Range: {
        // The values of each run of digits from pos, saturating; the
        // longest is tried first.
        vec<s::int64_t> vals;
        s::uint64_t v = 0;
        bool big = false;
        for (s::size_t e = pos; e < lim and s::isdigit(static_cast<unsigned char>(s[e])); ++e) {
          unsigned d = static_cast<unsigned>(s[e] - '0');
          if (v > (INT64_MAX - d) / 10) big = true;
          else v = v * 10 + d;
          vals.push_back(big ? INT64_MAX : static_cast<s::int64_t>(v));
        }
        for (s::size_t len = vals.size(); len > 0; --len) {
          s::int64_t x = vals[len - 1];
          if ((not n.has_lo or x >= n.lo) and (not n.has_hi or x <= n.hi) and k(pos + len))
            return true;
        }
        return false;
      }
      case Kind::Repeat:
        return repeat(n, 0, pos, k);
      default:
        assert(false);  // should never get here
    }
    return false;
  }

  // Whether `q` matches exactly s[b, e); the captures are left alone.
  bool spans(const Seq& q, s::size_t b, s::size_t e) const {
    auto saved = caps;
    bool ok = seq(q, 0, b, [&](s::size_t x) { return x == e; });
    caps = s::move(saved);
    return ok;
  }

  // x#, x##, (#cN,M): greedy, as zsh's — one more repeat is tried before
  // what follows.
  bool repeat(const Node& n, int count, s::size_t pos, const Cont& k) const {
    if (n.max < 0 or count < n.max) {
      bool more = node(*n.sub, pos, [&](s::size_t e) {
        return (e > pos or n.max >= 0) and repeat(n, count + 1, e, k);
      });
      if (more) return true;
    }
    return count >= n.min and k(pos);
  }

  bool plain(const Seq& q, s::size_t j) const {
    return j < q.size() and q[j].approx == 0 and single_char(q[j]);
  }

  bool seq(const Seq& q, s::size_t i, s::size_t pos, const Cont& k) const {
    if (stack_low()) return false;
    // Plain one-character nodes (no (#aN) errors allowed) are matched in a
    // loop, without a continuation each: they have one way to match.
    for (s::size_t next = 0; plain(q, i); ++i, pos = next)
      if (not one(q[i], pos, next)) return false;
    if (i == q.size()) return k(pos);
    // `*` then a plain character: only where that character is.
    if (q[i].kind == Kind::Star and plain(q, i + 1)) {
      for (s::size_t e = lim; e-- > pos;) {
        s::size_t next = 0;
        if (boundary(e) and one(q[i + 1], e, next) and seq(q, i + 1, e, k)) return true;
      }
      return false;
    }
    if (node(q[i], pos, [&](s::size_t e) { return seq(q, i + 1, e, k); })) return true;
    // (#aN): two characters the wrong way round are one error.
    const Node& a = q[i];
    s::size_t n1 = 0, n2 = 0;
    if (a.approx > errs and a.kind == Kind::Lit and i + 1 < q.size()
        and q[i + 1].kind == Kind::Lit and pos + 1 < lim
        and one(q[i + 1], pos, n1) and one(a, pos + 1, n2)) {
      ++errs;
      bool ok = seq(q, i + 2, pos + 2, k);
      --errs;
      return ok;
    }
    return false;
  }

  // All of s[b, lim); with (#aN) still in force at the end, extra
  // characters there are errors too.
  bool whole(const Seq& q, int end_approx, s::size_t b) const {
    return seq(q, 0, b, [&](s::size_t e) {
      return e == lim or lim - e <= static_cast<s::size_t>(s::max(0, end_approx - errs));
    });
  }

  mutable int errs = 0;
  mutable vec<s::pair<s::size_t, s::size_t>> caps;
};

}  // namespace

bool locale_multibyte() noexcept { return MB_CUR_MAX > 1; }

CompiledPattern CompiledPattern::compile(strv pattern, bool case_insensitive,
                                         bool extended, bool multibyte) {
  auto impl = s::make_shared<Impl>();
  impl->multibyte = multibyte;
  PatternParser pp{pattern, extended, *impl};
  pp.mb = multibyte;
  if (extended and pattern.size() > 1 and pattern[0] == '^') {
    impl->negate = true;
    pp.i         = 1;
  }
  PatFlags fl;
  fl.nocase = case_insensitive;
  // Alternatives at the top level, each with its `~` exclusions: in zsh
  // `~` binds tighter than `|`, so b~b|a is (b~b)|a.
  vec<Seq> alts;
  vec<vec<Seq>> alt_excludes;
  int end_approx = 0;
  for (;;) {
    alts.push_back(pp.parse_seq(false, fl, true));
    end_approx = impl->end_approx;
    alt_excludes.emplace_back();
    while (pp.i < pattern.size() and pattern[pp.i] == '~' and not pp.too_deep) {
      ++pp.i;
      alt_excludes.back().push_back(pp.parse_seq(false, fl, true));
    }
    if (pp.too_deep or pp.i >= pattern.size() or pattern[pp.i] != '|') break;
    ++pp.i;
  }
  if (pp.too_deep) {
    // The whole pattern as literal text.
    auto lit = s::make_shared<Impl>();
    lit->multibyte = multibyte;
    for (char c : pattern) {
      Node n;
      n.ch     = c;
      n.nocase = case_insensitive;
      lit->main.push_back(s::move(n));
    }
    CompiledPattern cp;
    cp.mImpl = s::move(lit);
    return cp;
  }
  if (alts.size() == 1) {
    impl->main     = s::move(alts[0]);
    impl->excludes = s::move(alt_excludes[0]);
  } else {
    Node group;
    group.kind         = Kind::Group;
    group.alts         = s::move(alts);
    group.alt_excludes = s::move(alt_excludes);
    impl->main.push_back(s::move(group));
  }
  impl->end_approx = end_approx;
  CompiledPattern cp;
  cp.mImpl = s::move(impl);
  return cp;
}

// Without (#aN) the position-set matcher decides whether, and how far, a
// pattern matches: (#b) groups change only what a match reports.  With
// (#aN) the backtracking matcher decides, and with (#b) it then finds the
// groups of the match already chosen, so a subject that cannot match
// costs no backtracking.
namespace {

bool set_matches(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e) {
  SetMatcher m{{text, e, impl.multibyte}};
  if (has(m.seq(impl.main, Positions{b}), e) == impl.negate) return false;
  for (const auto& ex : impl.excludes)
    if (has(m.seq(ex, Positions{b}), e)) return false;
  return true;
}

// Where a match from b may end does not depend on where the subject is
// cut off after it, so one run over all of text gives every prefix that
// matches.
s::size_t set_prefix(const CompiledPattern::Impl& impl, strv text, s::size_t b, bool longest) {
  SetMatcher m{{text, text.size(), impl.multibyte}};
  Positions ends = m.seq(impl.main, Positions{b});
  if (impl.negate) {
    // ^pat: the prefixes that pat does not match.
    Positions all;
    for (s::size_t p = b; p <= text.size(); ++p)
      if (m.boundary(p) and not has(ends, p)) all.push_back(p);
    ends = s::move(all);
  }
  for (const auto& ex : impl.excludes) ends = without(ends, m.seq(ex, Positions{b}));
  if (ends.empty()) return strv::npos;
  return (longest ? ends.back() : ends.front()) - b;
}

// The backtracking matcher on text[b, e): whether it matches, and the
// (#b) groups into `info`.
bool backtrack(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
               PatternMatch* info) {
  auto none = vec<s::pair<s::size_t, s::size_t>>(static_cast<s::size_t>(impl.captures),
                                                  {strv::npos, strv::npos});
  Matcher m{{text, e, impl.multibyte}, 0, none};
  if (m.whole(impl.main, impl.end_approx, b) == impl.negate) return false;
  for (const auto& ex : impl.excludes) {
    Matcher x{{text, e, impl.multibyte}, 0, none};
    if (x.whole(ex, 0, b)) return false;
  }
  if (info) {
    info->groups = s::move(m.caps);
    for (auto& [gb, ge] : info->groups)
      if (gb != strv::npos) {
        gb -= b;
        ge -= b;
      }
  }
  return true;
}

// After the set matcher found text[b, e) to match: its (#b) groups.  When
// the backtracking matcher cannot follow (a subject too long for the
// stack), the groups took no part.
void fill_groups(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
                 PatternMatch* info) {
  if (not info) return;
  info->groups.clear();
  if (impl.captures == 0) return;
  if (not backtrack(impl, text, b, e, info))
    info->groups.assign(static_cast<s::size_t>(impl.captures), {strv::npos, strv::npos});
}

}  // namespace

bool CompiledPattern::matches(strv text, PatternMatch* info) const {
  return matches(text, 0, text.size(), info);
}

bool CompiledPattern::matches(strv text, s::size_t b, s::size_t e, PatternMatch* info) const {
  if (mImpl->approx) return backtrack(*mImpl, text, b, e, info);
  if (not set_matches(*mImpl, text, b, e)) return false;
  fill_groups(*mImpl, text, b, e, info);
  return true;
}

s::size_t CompiledPattern::match_prefix(strv text, bool longest, PatternMatch* info) const {
  return match_prefix(text, 0, longest, info);
}

s::size_t CompiledPattern::match_prefix(strv text, s::size_t b, bool longest,
                                        PatternMatch* info) const {
  if (not mImpl->approx) {
    s::size_t len = set_prefix(*mImpl, text, b, longest);
    if (len != strv::npos) fill_groups(*mImpl, text, b, b + len, info);
    return len;
  }
  s::size_t room = text.size() - b;
  for (s::size_t n = 0; n <= room; ++n) {
    s::size_t len = longest ? room - n : n;
    if (backtrack(*mImpl, text, b, b + len, info)) return len;
  }
  return strv::npos;
}

bool CompiledPattern::backrefs() const noexcept { return mImpl->captures > 0; }

void set_match_params(ParamTable& params, const CompiledPattern& pat, strv text,
                      s::size_t start, s::size_t len, const PatternMatch& m,
                      bool ksh_arrays) {
  s::int64_t base = ksh_arrays ? 0 : 1;
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
      mbegin.push_back(s::to_string(static_cast<s::int64_t>(start + b) + base));
      mend.push_back(s::to_string(static_cast<s::int64_t>(start + e) + base - 1));
    }
    (void)params.set_array("match", s::move(match));
    (void)params.set_array("mbegin", s::move(mbegin));
    (void)params.set_array("mend", s::move(mend));
  }
  if (pat.sets_match()) {
    (void)params.set_scalar("MATCH", str(text.substr(start, len)));
    params.assign("MBEGIN", static_cast<s::int64_t>(start) + base, PM_INTEGER);
    params.assign("MEND", static_cast<s::int64_t>(start + len) + base - 1, PM_INTEGER);
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

str glob_unescape(strv s) {
  str out;
  out.reserve(s.size());
  for (s::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' and i + 1 < s.size()) ++i;
    out += s[i];
  }
  return out;
}

GlobPattern GlobPattern::compile(strv pat, GlobFlags flags) {
  GlobPattern g;
  g.raw   = str(pat);
  g.flags = flags;
  return g;
}

int collate(const str& a, const str& b, bool numeric) {
  if (int c = zstrcmp(a, b, numeric)) return c;
  return a.compare(b);
}

str collate_key(const str& s) {
  str key(s.size() * 4 + 16, '\0');
  s::size_t n = ::strxfrm(key.data(), s.c_str(), key.size());
  if (n >= key.size()) {
    key.resize(n + 1);
    ::strxfrm(key.data(), s.c_str(), key.size());
  }
  key.resize(n);
  return key;
}

void collate_sort(vec<str>& v, bool numeric) {
  if (numeric) {
    s::sort(v.begin(), v.end(), [](const str& a, const str& b) { return collate(a, b, true) < 0; });
    return;
  }
  // strxfrm() once per string is much cheaper than strcoll() per
  // comparison in a UTF-8 locale.
  vec<s::pair<str, str>> keyed;
  keyed.reserve(v.size());
  for (auto& e : v) keyed.emplace_back(collate_key(e), s::move(e));
  s::sort(keyed.begin(), keyed.end());
  for (s::size_t k = 0; k < v.size(); ++k) v[k] = s::move(keyed[k].second);
}

// ---------------------------------------------------------------------------
// Filename generation.
// ---------------------------------------------------------------------------
namespace {

// One test of a glob qualifier list (zsh's struct qual).
struct QualTest {
  char        kind   = 0;
  bool        negate = false;
  bool        follow = false;   // `-`: test the target of a symlink
  int         range  = 0;       // `-` less than, `+` more than, 0 equal
  s::int64_t  value  = 0;
  s::int64_t  unit   = 1;       // L: bytes per unit; a m c: seconds
  s::uint32_t yes = 0, no = 0;  // f: mode bits that must be set, clear
  str         code;             // e: code, +: function name
};

struct SortKey {
  char key;
  bool desc;
};

struct Qualifiers {
  vec<vec<QualTest>> alternatives;   // a file passes when one set does
  bool nullglob = false;
  opt<bool> dots, numeric, markdirs, listtypes;
  vec<SortKey> sorts;
  bool select = false;
  s::int64_t sel_lo = 1, sel_hi = 1;
  s::int64_t first_n = -1;           // Y: stop after so many
  str modifiers;                     // `:t:r`
  vec<str> prepend;                  // P: words put before each match
};

using QualResult = Result<Qualifiers>;

ShellError qual_error(str msg) { return ShellError{ErrorKind::BadPattern, s::move(msg)}; }

char end_char(char c) {
  switch (c) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '<': return '>';
    default:  return c;
  }
}

// Reads the arguments inside a qualifier list: `i` is where the next one
// starts, and each read moves it past what it took.
struct QualReader {
  opt<str>        delimited();
  opt<s::int64_t> number();
  int             range_of();
  Result<s::pair<s::uint32_t, s::uint32_t>> mode_spec();
  Result<s::int64_t> owner(char kind);

  strv      body;
  s::size_t i = 0;
};

// A delimited argument at body[i] (`:text:`, `[text]`); i ends after it.
opt<str> QualReader::delimited() {
  if (i >= body.size()) return s::nullopt;
  char end = end_char(body[i]);
  s::size_t e = body.find(end, i + 1);
  if (e == strv::npos) return s::nullopt;
  str out(body.substr(i + 1, e - i - 1));
  i = e + 1;
  return out;
}

opt<s::int64_t> QualReader::number() {
  s::size_t b = i;
  while (i < body.size() and s::isdigit(static_cast<unsigned char>(body[i]))) ++i;
  if (i == b) return s::nullopt;
  return s::strtoll(str(body.substr(b, i - b)).c_str(), nullptr, 10);
}

int QualReader::range_of() {
  if (i < body.size() and (body[i] == '-' or body[i] == '+')) return body[i++] == '-' ? -1 : 1;
  return 0;
}

// f's mode specification (zsh's qgetmodespec): `NNN`, `=NNN`, `-NNN`,
// `+NNN` (`?` any digit), or delimited chmod-style `[ugoa]*[=+-][rwxst]*`
// clauses, comma-separated.  The modes that must be set and unset.
Result<s::pair<s::uint32_t, s::uint32_t>> QualReader::mode_spec() {
  s::uint32_t yes = 0, no = 0;
  char end = 0;
  char c = i < body.size() ? body[i] : 0;
  if (not (c == '=' or c == '+' or c == '-' or c == '?' or (c >= '0' and c <= '7'))) {
    end = end_char(c);
    ++i;
  }
  auto bad = [] { return s::unexpected(qual_error("invalid mode specification")); };
  do {
    s::uint32_t mask = 0;
    while (end and i < body.size() and strv("ugoa").find(body[i]) != strv::npos) {
      switch (body[i++]) {
        case 'o': mask |= 01007; break;
        case 'g': mask |= 02070; break;
        case 'u': mask |= 04700; break;
        default:  mask |= 07777;
      }
    }
    c = i < body.size() ? body[i] : 0;
    char how = c == '+' or c == '-' ? c : '=';
    if (c == '+' or c == '-' or c == '=') ++i;
    s::uint32_t val = 0;
    if (mask) {
      for (;;) {
        if (i >= body.size()) return bad();
        c = body[i++];
        if (c == ',' or c == end) break;
        switch (c) {
          case 'x': val |= 00111; break;
          case 'w': val |= 00222; break;
          case 'r': val |= 00444; break;
          case 's': val |= 06000; break;
          case 't': val |= 01000; break;
          default:
            if (c < '0' or c > '7') return bad();
            s::uint32_t d = static_cast<s::uint32_t>(c - '0');
            val |= d | (d << 3) | (d << 6);
        }
      }
      if (how == '=' or how == '+') {
        yes |= val & mask;
        val = ~val;
      }
      if (how == '=' or how == '-') no |= val & mask;
    } else {
      c = i < body.size() ? body[i] : 0;
      if ((end and c == end) or c == ',' or not c) return bad();
      s::uint32_t any = 07777;
      while (i < body.size() and (body[i] == '?' or (body[i] >= '0' and body[i] <= '7'))) {
        if (body[i] == '?') {
          any = (any << 3) | 7;
          val <<= 3;
        } else {
          any <<= 3;
          val = (val << 3) | static_cast<s::uint32_t>(body[i] - '0');
        }
        ++i;
      }
      c = i < body.size() ? body[i] : 0;
      if (end and c != end and c != ',') return bad();
      if (end) ++i;
      if (how == '=') {
        yes = (yes & ~any) | val;
        no  = (no & ~any) | (~val & ~any);
      } else if (how == '+') {
        yes |= val;
      } else {
        no |= val;
      }
    }
  } while (end and c != end);
  return s::pair{yes & 07777u, no & 07777u};
}

// u and g: a number, or a delimited user or group name.
Result<s::int64_t> QualReader::owner(char kind) {
  if (auto n = number()) return *n;
  if (i >= body.size())
    return s::unexpected(qual_error(str("missing delimiter for '") + kind + "' glob qualifier"));
  auto name = delimited();
  if (not name) return s::unexpected(qual_error("missing end of name"));
  if (kind == 'u') {
    if (struct passwd* pw = ::getpwnam(name->c_str())) return pw->pw_uid;
    return s::unexpected(qual_error("unknown username '" + *name + "'"));
  }
  if (struct group* gr = ::getgrnam(name->c_str())) return gr->gr_gid;
  return s::unexpected(qual_error("unknown group"));
}

// Parses the text between the parentheses of a qualifier list.
QualResult parse_qualifiers(strv body) {
  Qualifiers q;
  q.alternatives.emplace_back();
  bool negate = false, follow = false;
  QualReader in{body};
  s::size_t& i = in.i;
  while (i < body.size()) {
    char c = body[i++];
    QualTest t;
    t.kind   = c;
    t.negate = negate;
    t.follow = follow;
    auto add = [&] { q.alternatives.back().push_back(t); };
    switch (c) {
      case ',':
        negate = follow = false;
        if (not q.alternatives.back().empty()) q.alternatives.emplace_back();
      break; case ':':
        q.modifiers = ":" + str(body.substr(i));
        i = body.size();
      break; case '^': negate = not negate;
      break; case '-': follow = not follow;
      break; case 'N': q.nullglob = not negate;
      break; case 'D': q.dots = not negate;
      break; case 'n': q.numeric = not negate;
      break; case 'M': q.markdirs = not negate;
      break; case 'T': q.listtypes = not negate;
      break; case '/': case '.': case '@': case '=': case 'p': case '*': case 'r': case 'w':
      case 'x': case 'A': case 'I': case 'E': case 'R': case 'W': case 'X': case 's':
      case 'S': case 't': case 'U': case 'G': case 'F':
        add();
      break; case '%':
        if (i < body.size() and (body[i] == 'b' or body[i] == 'c')) t.value = body[i++];
        add();
      break; case 'f': {
        auto modes = in.mode_spec();
        if (not modes) return s::unexpected(modes.error());
        t.yes = modes->first;
        t.no  = modes->second;
        add();
      }
      break; case 'e': case '+': {
        if (c == 'e') {
          auto code = in.delimited();
          if (not code) return s::unexpected(qual_error("missing end of string"));
          t.code = *code;
        } else {
          s::size_t b = i;
          while (i < body.size() and (s::isalnum(static_cast<unsigned char>(body[i]))
                                      or body[i] == '_'))
            ++i;
          if (i == b) return s::unexpected(qual_error("missing identifier after `+'"));
          t.code = str(body.substr(b, i - b));
        }
        add();
      }
      break; case 'u': case 'g': {
        auto id = in.owner(c);
        if (not id) return s::unexpected(id.error());
        t.value = *id;
        add();
      }
      break; case 'd': case 'l': {
        if (c == 'l') t.range = in.range_of();
        auto n = in.number();
        if (not n) return s::unexpected(qual_error("number expected"));
        t.value = *n;
        add();
      }
      break; case 'L': case 'a': case 'm': case 'c': {
        if (i < body.size()) {
          char u = body[i];
          s::int64_t unit = 0;
          if (c == 'L') {
            switch (u) {
              case 'p': case 'P': unit = 512; break;
              case 'k': case 'K': unit = 1024; break;
              case 'm': case 'M': unit = 1024 * 1024; break;
              case 'g': case 'G': unit = 1024 * 1024 * 1024; break;
              case 't': case 'T': unit = 1024LL * 1024 * 1024 * 1024; break;
              default: ;
            }
          } else {
            switch (u) {
              case 'M': unit = 2592000; break;
              case 'w': unit = 604800; break;
              case 'h': unit = 3600; break;
              case 'm': unit = 60; break;
              case 's': unit = 1; break;
              case 'd': unit = 86400; break;
              default: ;
            }
          }
          if (unit) ++i;
          t.unit = unit ? unit : (c == 'L' ? 1 : 86400);
        }
        t.range = in.range_of();
        auto n = in.number();
        if (not n) return s::unexpected(qual_error("number expected"));
        t.value = *n;
        add();
      }
      break; case 'o': case 'O': {
        if (i >= body.size() or strv("nLlamcdN").find(body[i]) == strv::npos)
          return s::unexpected(qual_error("unknown sort specifier"));
        q.sorts.push_back({body[i++], c == 'O'});
      }
      break; case 'Y': {
        auto n = in.number();
        if (not n) return s::unexpected(qual_error("number expected after Y"));
        q.first_n = *n;
      }
      break; case 'P': {
        auto w = in.delimited();
        if (not w) return s::unexpected(qual_error("missing end of string"));
        q.prepend.push_back(*w);
      }
      break; case '[': {
        s::size_t close = body.find(']', i);
        if (close == strv::npos) return s::unexpected(qual_error("missing end of subscript"));
        str spec(body.substr(i, close - i));
        auto comma = spec.find(',');
        q.select = true;
        q.sel_lo = s::strtoll(spec.c_str(), nullptr, 10);
        q.sel_hi = comma == str::npos ? q.sel_lo
                                      : s::strtoll(spec.c_str() + comma + 1, nullptr, 10);
        i = close + 1;
      }
      break; default:
        return s::unexpected(qual_error(str("unknown file attribute: ") + c));
    }
  }
  if (q.alternatives.back().empty() and q.alternatives.size() > 1) q.alternatives.pop_back();
  return q;
}

// The final unescaped `(` … `)` group of `raw`, or npos.  With `q_only`,
// only a `(#q…)` group.  A `|` in it, or with `tilde` (EXTENDED_GLOB) a
// `~`, makes it a pattern group instead.
s::size_t final_group(strv raw, bool q_only, bool tilde = false) {
  if (raw.size() < 3 or raw.back() != ')') return strv::npos;
  // An escaped `)` at the end isn't one.
  s::size_t bs = 0;
  for (s::size_t k = raw.size() - 1; k > 0 and raw[k - 1] == '\\'; --k) ++bs;
  if (bs % 2) return strv::npos;
  for (s::size_t k = raw.size() - 1; k-- > 0;) {
    char c = raw[k];
    s::size_t n = 0;
    for (s::size_t j = k; j > 0 and raw[j - 1] == '\\'; --j) ++n;
    if (n % 2) continue;
    if (c == ')') return strv::npos;
    if (c != '(') {
      if ((c == '|' or (c == '~' and tilde)) and not q_only) return strv::npos;
      continue;
    }
    bool hash_q = k + 2 < raw.size() and raw[k + 1] == '#' and raw[k + 2] == 'q';
    if (q_only and not hash_q) return strv::npos;
    return k;
  }
  return strv::npos;
}

// A pattern with its qualifier lists split off the end.
struct SplitPattern {
  str             pattern;
  opt<Qualifiers> quals;
};

// Splits the qualifier lists off the end of `raw`: `(#q…)` groups under
// EXTENDED_GLOB, and with BARE_GLOB_QUAL one final group with no `|`.
Result<SplitPattern> split_qualifiers(str raw, const OptionState& opts, bool extended) {
  str all;
  bool found = false;
  for (;;) {
    s::size_t open = extended ? final_group(raw, true) : strv::npos;
    if (open != strv::npos) {
      all.insert(0, raw.substr(open + 3, raw.size() - open - 4));
    } else if (not found and opts.get(OPT_BAREGLOBQUAL)
               and (open = final_group(raw, false, extended)) != strv::npos
               and not (raw[open + 1] == '#')) {
      // A word that is all one group is qualifiers too, on nothing, as in
      // zsh: `(a1)` finds no files.
      all.insert(0, raw.substr(open + 1, raw.size() - open - 2));
      raw.resize(open);
      found = true;
      break;
    } else {
      break;
    }
    raw.resize(open);
    found = true;
  }
  if (not found) return SplitPattern{s::move(raw), s::nullopt};
  auto q = parse_qualifiers(glob_unescape(all));
  if (not q) return s::unexpected(q.error());
  return SplitPattern{s::move(raw), s::move(*q)};
}

bool nonempty_dir(const str& path) {
  DIR* d = ::opendir(path.c_str());
  if (not d) return false;
  bool any = false;
  while (auto* e = ::readdir(d)) {
    strv n(e->d_name);
    if (n != "." and n != "..") { any = true; break; }
  }
  ::closedir(d);
  return any;
}

// The test against `st`; e and + are run by the caller.
bool qual_test(const QualTest& t, const struct stat& st, const str& path) {
  auto ranged = [&](s::int64_t v) {
    return t.range < 0 ? v < t.value : t.range > 0 ? v > t.value : v == t.value;
  };
  mode_t m = st.st_mode;
  switch (t.kind) {
    case '/': return S_ISDIR(m);
    case '.': return S_ISREG(m);
    case '@': return S_ISLNK(m);
    case '=': return S_ISSOCK(m);
    case 'p': return S_ISFIFO(m);
    case '%':
      return t.value == 'b' ? S_ISBLK(m) : t.value == 'c' ? S_ISCHR(m)
                            : S_ISBLK(m) or S_ISCHR(m);
    case '*': return S_ISREG(m) and (m & 0111);
    case 'r': return m & S_IRUSR;
    case 'w': return m & S_IWUSR;
    case 'x': return m & S_IXUSR;
    case 'A': return m & S_IRGRP;
    case 'I': return m & S_IWGRP;
    case 'E': return m & S_IXGRP;
    case 'R': return m & S_IROTH;
    case 'W': return m & S_IWOTH;
    case 'X': return m & S_IXOTH;
    case 's': return m & S_ISUID;
    case 'S': return m & S_ISGID;
    case 't': return m & S_ISVTX;
    case 'f': return (m & t.yes) == t.yes and not (m & t.no);
    case 'U': return st.st_uid == ::geteuid();
    case 'G': return st.st_gid == ::getegid();
    case 'u': return static_cast<s::int64_t>(st.st_uid) == t.value;
    case 'g': return static_cast<s::int64_t>(st.st_gid) == t.value;
    case 'd': return static_cast<s::int64_t>(st.st_dev) == t.value;
    case 'l': return ranged(static_cast<s::int64_t>(st.st_nlink));
    case 'F': return S_ISDIR(m) and nonempty_dir(path);
    case 'L': {
      s::int64_t size = static_cast<s::int64_t>(st.st_size);
      return ranged((size + t.unit - 1) / t.unit);
    }
    case 'a': case 'm': case 'c': {
      s::time_t when = t.kind == 'a' ? st.st_atime : t.kind == 'm' ? st.st_mtime : st.st_ctime;
      return ranged((s::time(nullptr) - when) / t.unit);
    }
    default: return false;
  }
}

// zsh's file_type() mark for LIST_TYPES / the T qualifier.
char type_mark(const str& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return 0;
  mode_t m = st.st_mode;
  if (S_ISBLK(m)) return '#';
  if (S_ISCHR(m)) return '%';
  if (S_ISDIR(m)) return '/';
  if (S_ISFIFO(m)) return '|';
  if (S_ISLNK(m)) return '@';
  if (S_ISSOCK(m)) return '=';
  if (S_ISREG(m) and (m & 0111)) return '*';
  // Anything else, a plain file too, gets a blank, as zsh's file_type.
  return ' ';
}

bool is_dir(const str& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 and S_ISDIR(st.st_mode);
}

str join_path(const str& dir, strv name) {
  if (dir.empty()) return str(name);
  if (dir.back() == '/') return dir + str(name);
  return dir + '/' + str(name);
}

// The locale's collation, except that with NUMERIC_GLOB_SORT runs of
// digits compare by value.
bool name_less(const str& a, const str& b, bool numeric) {
  return collate(a, b, numeric) < 0;
}

struct Walker {
  bool dots;
  bool nocase;
  bool extended;
  bool multibyte;

  // Every directory below `dir`, depth first, `dir` itself first.  For `**`
  // symlinks to directories are not followed; for `***` they are.
  void descend(const str& dir, bool follow, vec<str>& out) {
    out.push_back(dir);
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (not d) return;
    vec<str> subdirs;
    while (auto* ent = ::readdir(d)) {
      strv name(ent->d_name);
      if (name == "." or name == "..") continue;
      if (name[0] == '.' and not dots) continue;
      str path = join_path(dir, name);
      struct stat st{};
      int rc = follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
      if (rc == 0 and S_ISDIR(st.st_mode)) subdirs.push_back(path);
    }
    ::closedir(d);
    collate_sort(subdirs, false);
    for (const auto& s : subdirs) descend(s, follow, out);
  }

  void match_dir(const str& dir, strv seg, bool need_dir,
                 vec<str>& out) {
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (not d) return;
    auto pat = CompiledPattern::compile(seg, nocase, extended, multibyte);
    bool dot_ok = dots or (not seg.empty() and seg[0] == '.')
               or (seg.size() > 1 and seg[0] == '\\' and seg[1] == '.');
    while (auto* ent = ::readdir(d)) {
      strv name(ent->d_name);
      if (name == "." or name == "..") continue;
      if (name[0] == '.' and not dot_ok) continue;
      if (not pat.matches(name)) continue;
      str path = join_path(dir, name);
      if (need_dir and not is_dir(path)) continue;
      out.push_back(s::move(path));
    }
    ::closedir(d);
  }
};

vec<str> split_segments(strv raw) {
  vec<str> segs;
  str cur;
  for (s::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' and i + 1 < raw.size()) {
      cur += raw[i];
      cur += raw[++i];
    } else if (raw[i] == '/') {
      segs.push_back(s::move(cur));
      cur.clear();
    } else {
      cur += raw[i];
    }
  }
  segs.push_back(s::move(cur));
  return segs;
}

}  // namespace

Result<vec<str>> glob_expand(
    const GlobPattern& pat,
    const OptionState& opts,
    const GlobHooks*   hooks)
{
  str        raw = pat.raw;
  bool extended = opts.extended_glob() or (pat.flags & GLOB_EXTGLOB);
  // An unclosed `(` or `[` is a bad pattern, as in zsh (`echo a[b`,
  // `echo (b`); a `[` alone is the word `[`.  Under NO_BAD_PATTERN the
  // word stays as it is.
  if (raw != "[") {
    int parens = 0;
    bool bad = false;
    for (s::size_t i = 0; i < raw.size() and not bad; ++i) {
      char c = raw[i];
      if (c == '\\') { ++i; continue; }
      if (c == '[') {
        CompiledPattern::Impl impl;
        PatternParser pp{raw, extended, impl, i};
        Node n;
        if (not pp.parse_class(n)) bad = true;
        else i = pp.i - 1;
      } else if (c == '(') {
        ++parens;
      } else if (c == ')' and parens > 0) {
        --parens;
      }
    }
    if (bad or parens > 0) {
      if (not opts.get(OPT_BADPATTERN)) return vec<str>{glob_unescape(pat.raw)};
      return s::unexpected(qual_error("bad pattern: " + glob_unescape(pat.raw)));
    }
  }
  auto split = split_qualifiers(s::move(raw), opts, extended);
  if (not split) return s::unexpected(split.error());
  raw = s::move(split->pattern);
  bool has_quals = split->quals.has_value();
  Qualifiers q = has_quals ? s::move(*split->quals) : Qualifiers{};
  // A word with no wildcard is itself, its `~`s and all.
  if (not has_quals and not has_glob_chars(raw, extended))
    return vec<str>{glob_unescape(raw)};

  // Under EXTENDED_GLOB, `a~b` excludes paths matching `b`; the exclusion
  // applies to the whole path, so split it off before walking segments.
  vec<CompiledPattern> excludes;
  if (extended) {
    int depth = 0;
    for (s::size_t i = 0; i < raw.size(); ++i) {
      char c = raw[i];
      if (c == '\\') { ++i; continue; }
      if (c == '(' or c == '[') ++depth;
      else if ((c == ')' or c == ']') and depth > 0) --depth;
      else if (c == '~' and depth == 0 and i > 0 and i + 1 < raw.size()) {
        for (s::size_t s = i + 1, e; s <= raw.size(); s = e + 1) {
          e = raw.find('~', s);
          if (e == str::npos) e = raw.size();
          excludes.push_back(CompiledPattern::compile(
              strv(raw).substr(s, e - s),
              not opts.get(OPT_CASEGLOB), true, opts.multibyte()));
        }
        raw.resize(i);
        break;
      }
    }
  }
  if (not has_quals and excludes.empty() and not has_glob_chars(raw, extended))
    return vec<str>{glob_unescape(raw)};

  Walker w{q.dots.value_or((pat.flags & GLOB_DOTFILES) or opts.get(OPT_GLOBDOTS)),
           not opts.get(OPT_CASEGLOB) or (pat.flags & GLOB_NOCASE),
           extended, opts.multibyte()};

  auto segs = split_segments(raw);
  vec<str> cur{""};
  s::size_t first = 0;
  if (segs.size() > 1 and segs[0].empty()) {   // absolute path
    cur   = {"/"};
    first = 1;
  }
  bool trailing_slash = segs.size() > 1 and segs.back().empty();
  s::size_t last = trailing_slash ? segs.size() - 1 : segs.size();

  for (s::size_t s = first; s < last; ++s) {
    const str& seg = segs[s];
    bool final_seg = s + 1 == last;
    bool need_dir  = not final_seg or trailing_slash;
    vec<str> next;
    if ((seg == "**" or seg == "***") and (not final_seg or trailing_slash)) {
      for (const auto& dir : cur) w.descend(dir, seg == "***", next);
      // A final `**/` lists the directories below, not the current one.
      if (final_seg) s::erase_if(next, [](const str& d) { return d.empty(); });
    } else if (not has_glob_chars(seg, extended)) {
      str lit = glob_unescape(seg);
      for (const auto& dir : cur) {
        str path = seg.empty() ? dir : join_path(dir, lit);
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0 and (not need_dir or is_dir(path)))
          next.push_back(s::move(path));
      }
    } else {
      for (const auto& dir : cur) w.match_dir(dir, seg, need_dir, next);
    }
    cur = s::move(next);
    if (cur.empty()) break;
  }
  if (trailing_slash)
    for (auto& p : cur) if (p.empty() or p.back() != '/') p += '/';

  // Exclusions and qualifier tests: by default of the file itself, a
  // symlink not followed.
  s::erase_if(cur, [&](const str& p) {
    for (const auto& ex : excludes)
      if (ex.matches(p)) return true;
    return false;
  });
  // Each match keeps the file it came from, for sorting once modifiers or
  // e's $reply have changed its name.
  struct Match { str name, file; };
  vec<Match> kept;
  for (auto& p : cur) {
    struct stat st{}, target{};
    bool have = false, have_target = false;
    vec<str> instead;
    auto passes = [&](const vec<QualTest>& tests) {
      for (const auto& t : tests) {
        bool ok;
        if (t.kind == 'e' or t.kind == '+') {
          auto r = hooks and hooks->eval ? hooks->eval(t.code, t.kind == '+', p)
                                         : opt<vec<str>>{};
          ok = r.has_value();
          if (ok and not r->empty()) instead = s::move(*r);
        } else {
          if (not have) {
            if (::lstat(p.c_str(), &st) != 0) return false;
            have = true;
          }
          const struct stat* sp = &st;
          if (t.follow) {
            if (not have_target) {
              if (::stat(p.c_str(), &target) != 0) target = st;
              have_target = true;
            }
            sp = &target;
          }
          ok = qual_test(t, *sp, p);
        }
        if (ok == t.negate) return false;
      }
      return true;
    };
    bool any = q.alternatives.empty();
    for (const auto& alt : q.alternatives)
      if (passes(alt)) { any = true; break; }
    if (not any) continue;
    if (instead.empty()) instead.push_back(p);
    for (auto& name : instead) {
      if (q.listtypes.value_or(false)) {
        if (char m = type_mark(p)) name += m;
      } else if (q.markdirs.value_or(opts.get(OPT_MARKDIRS))) {
        if (name.back() != '/' and is_dir(p)) name += '/';
      }
      if (not q.modifiers.empty() and hooks and hooks->modify) {
        auto m = hooks->modify(q.modifiers, name);
        if (not m) return s::unexpected(m.error());
        name = s::move(*m);
      }
      kept.push_back({s::move(name), p});
    }
    if (q.first_n >= 0 and kept.size() >= static_cast<s::size_t>(q.first_n)) break;
  }

  bool numeric = q.numeric.value_or(opts.get(OPT_NUMERICGLOBSORT));
  // With Y and no o, the first matches found, unsorted.
  if (q.sorts.empty() and q.first_n < 0) q.sorts.push_back({'n', false});
  if (not q.sorts.empty() and q.sorts.front().key != 'N') {
    struct Times { s::int64_t sec, nsec; };
    auto value = [](const str& p, char key) -> Times {
      struct stat st{};
      if (::stat(p.c_str(), &st) != 0 and ::lstat(p.c_str(), &st) != 0) return {0, 0};
#ifdef __APPLE__
      auto ts = key == 'a' ? st.st_atimespec : key == 'm' ? st.st_mtimespec : st.st_ctimespec;
#else
      auto ts = key == 'a' ? st.st_atim : key == 'm' ? st.st_mtim : st.st_ctim;
#endif
      switch (key) {
        case 'L': return {static_cast<s::int64_t>(st.st_size), 0};
        case 'l': return {static_cast<s::int64_t>(st.st_nlink), 0};
        default:  return {static_cast<s::int64_t>(ts.tv_sec), static_cast<s::int64_t>(ts.tv_nsec)};
      }
    };
    // zsh's `om` lists the newest first, `oL` the smallest first, `od`
    // the deeper paths first; ties go by name.
    auto cmp = [&](const Match& a, const Match& b) {
      for (auto [key, desc] : q.sorts) {
        int r = 0;
        if (key == 'n') {
          r = name_less(a.name, b.name, numeric) ? -1 : name_less(b.name, a.name, numeric) ? 1 : 0;
        } else if (key == 'd') {
          auto da = s::count(a.file.begin(), a.file.end(), '/');
          auto db = s::count(b.file.begin(), b.file.end(), '/');
          r = da > db ? -1 : da < db ? 1 : 0;
        } else if (key != 'N') {
          Times va = value(a.file, key), vb = value(b.file, key);
          bool newest = key == 'a' or key == 'm' or key == 'c';
          bool same = va.sec == vb.sec and va.nsec == vb.nsec;
          bool less = va.sec < vb.sec or (va.sec == vb.sec and va.nsec < vb.nsec);
          r = same ? 0 : less != newest ? -1 : 1;
        }
        if (r) return desc ? r > 0 : r < 0;
      }
      return name_less(a.name, b.name, numeric);
    };
    s::stable_sort(kept.begin(), kept.end(), cmp);
  } else if (not q.sorts.empty() and q.sorts.front().desc) {
    s::reverse(kept.begin(), kept.end());
  }
  cur.clear();
  for (auto& m : kept) cur.push_back(s::move(m.name));

  if (q.select and not cur.empty()) {
    auto       n  = static_cast<s::int64_t>(cur.size());
    s::int64_t lo = q.sel_lo < 0 ? n + q.sel_lo + 1 : q.sel_lo;
    s::int64_t hi = q.sel_hi < 0 ? n + q.sel_hi + 1 : q.sel_hi;
    lo = s::max<s::int64_t>(lo, 1);
    hi = s::min(hi, n);
    if (lo > hi) cur.clear();
    else cur = vec<str>(cur.begin() + (lo - 1), cur.begin() + hi);
  }

  if (not q.prepend.empty()) {
    vec<str> out;
    for (auto& p : cur) {
      out.insert(out.end(), q.prepend.begin(), q.prepend.end());
      out.push_back(s::move(p));
    }
    cur = s::move(out);
  }

  if (cur.empty()) {
    if (q.nullglob or opts.nullglob()) return vec<str>{};
    if (opts.nomatch())
      return s::unexpected(ShellError{ErrorKind::BadPattern,
          "no matches found: " + glob_unescape(pat.raw)});
    return vec<str>{glob_unescape(pat.raw)};
  }
  return cur;
}

Result<vec<str>> expand_args(
    vec<str>           args,
    const OptionState& opts)
{
  vec<str> result;
  for (auto& arg : args) {
    auto expanded = glob_expand(GlobPattern::compile(arg), opts);
    if (not expanded) return s::unexpected(expanded.error());
    for (auto& m : *expanded) result.push_back(s::move(m));
  }
  return result;
}

}  // namespace jfsh
