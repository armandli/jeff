#include <jfsh/glob/glob.h>

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <functional>

#include <jfsh/core/aliases.h>
#include <jfsh/core/stack.h>
#include <jfsh/core/utf8.h>

namespace jfsh {

// ---------------------------------------------------------------------------
// Pattern compiler.  Patterns arrive in escaped form: a backslash makes the
// next character literal, everything else unescaped is pattern syntax.
// ---------------------------------------------------------------------------

using PatternSeq = vec<CompiledPattern::Node>;

struct CompiledPattern::Node {
  enum class Kind : s::uint8_t { Lit, Any, Star, Class, Group, Range, Repeat };

  Kind kind = Kind::Lit;
  char ch   = 0;                                         // Lit

  bool class_negated = false;                            // Class
  vec<s::pair<char32_t, char32_t>> ranges;
  vec<s::pair<int (*)(int), wctype_t>> classes;

  vec<PatternSeq> alts;                          // Group

  bool       has_lo = false, has_hi = false;              // Range <lo-hi>
  s::int64_t lo = 0, hi = 0;

  sptr<Node> sub;                             // Repeat x# / x##
  int min = 0;
};

struct CompiledPattern::Impl {
  PatternSeq      main;
  vec<PatternSeq> excludes;   // a~b~c: `b` and `c`
  bool            negate = false;   // ^pat
  bool            nocase = false;
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

struct PatternParser {
  strv      p;
  bool      ext;
  s::size_t i  = 0;
  bool      mb = false;
  bool      too_deep = false;

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
  bool parse_group(Node& n) {
    if (too_deep or stack_low()) { too_deep = true; return false; }
    s::size_t save = i;
    ++i;
    n.kind = Kind::Group;
    for (;;) {
      n.alts.push_back(parse_seq(true));
      if (i >= p.size()) { i = save; n.alts.clear(); return false; }
      if (p[i] == ')') { ++i; return true; }
      ++i;  // '|'
    }
  }

  Seq parse_seq(bool in_group) {
    Seq seq;
    while (i < p.size()) {
      char c = p[i];
      if (in_group and (c == '|' or c == ')')) break;
      if (ext and not in_group and c == '~' and not seq.empty()) break;
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
      } else if (c == '(' and parse_group(n)) {
      } else if (c == '<' and parse_range(n)) {
      } else {
        n = Node{};
        n.ch = c;
        ++i;
      }
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
    return seq;
  }
};

char32_t fold(char32_t c, bool nocase, bool mb) noexcept {
  if (not nocase) return c;
  if (mb and c >= 0x80) return static_cast<char32_t>(::towlower(static_cast<wint_t>(c)));
  return static_cast<char32_t>(s::tolower(static_cast<int>(c)));
}

bool class_has(const Node& n, char32_t c, bool nocase, bool mb) {
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

// Matching runs the pattern over the set of positions it may be at, one
// node at a time, as a Thompson NFA does: no backtracking, so time is
// bounded by pattern size times subject length and the stack only grows
// with the nesting of groups in the pattern, never with the subject.
struct Matcher {
  strv s;
  bool nocase;
  bool mb;

  // The character at p and the position after it.
  char32_t at(s::size_t p, s::size_t& next) const {
    if (not mb) { next = p + 1; return static_cast<unsigned char>(s[p]); }
    s::size_t len = 1;
    char32_t c = utf8_decode(s, p, len);
    next = p + len;
    return c;
  }
  bool boundary(s::size_t p) const {
    return not mb or p >= s.size() or (static_cast<unsigned char>(s[p]) & 0xc0) != 0x80;
  }

  // Whether one character of `n` (a Lit, Any or Class) matches at p.
  bool one(const Node& n, s::size_t p, s::size_t& next) const {
    if (p >= s.size()) return false;
    if (n.kind == Kind::Lit) {
      next = p + 1;
      return fold(static_cast<unsigned char>(s[p]), nocase, false)
          == fold(static_cast<unsigned char>(n.ch), nocase, false);
    }
    char32_t c = at(p, next);
    return n.kind == Kind::Any or class_has(n, c, nocase, mb);
  }

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
        for (s::size_t p = set.front(), k = 0; p <= s.size(); ++p) {
          bool listed = k < set.size() and set[k] == p;
          if (listed) ++k;
          if (listed or boundary(p)) out.push_back(p);
        }
      break; case Kind::Group:
        // Matching nests as the pattern did; past the stack it fails.
        if (stack_low()) break;
        for (const auto& alt : n.alts) {
          Positions part = seq(alt, set);
          Positions merged;
          s::set_union(out.begin(), out.end(), part.begin(), part.end(),
                       s::back_inserter(merged));
          out = s::move(merged);
        }
      break; case Kind::Range:
        for (s::size_t p : set) {
          s::uint64_t v = 0;
          bool big = false;
          for (s::size_t e = p; e < s.size() and s::isdigit(static_cast<unsigned char>(s[e])); ++e) {
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

  // x# (min 0) and x## (min 1): every position after `min` or more
  // matches of the sub-pattern.
  Positions repeat(const Node& n, const Positions& set) const {
    const Node& sub = *n.sub;
    Positions out;
    if (sub.kind == Kind::Lit or sub.kind == Kind::Any or sub.kind == Kind::Class) {
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
    vec<bool> seen(s.size() + 1, false);
    if (n.min == 0)
      for (s::size_t p : set) { seen[p] = true; out.push_back(p); }
    Positions frontier = set;
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

  Positions ends(const Seq& q) const { return seq(q, Positions{0}); }
  bool whole(const Seq& q) const {
    Positions e = ends(q);
    return not e.empty() and e.back() == s.size();
  }
};

}  // namespace

bool locale_multibyte() noexcept { return MB_CUR_MAX > 1; }

CompiledPattern CompiledPattern::compile(strv pattern, bool case_insensitive,
                                         bool extended, bool multibyte) {
  auto impl = s::make_shared<Impl>();
  impl->nocase    = case_insensitive;
  impl->multibyte = multibyte;
  PatternParser pp{pattern, extended};
  pp.mb = multibyte;
  if (extended and pattern.size() > 1 and pattern[0] == '^') {
    impl->negate = true;
    pp.i         = 1;
  }
  impl->main = pp.parse_seq(false);
  while (pp.i < pattern.size() and pattern[pp.i] == '~') {
    ++pp.i;
    impl->excludes.push_back(pp.parse_seq(false));
  }
  if (pp.too_deep) {
    // The whole pattern as literal text.
    impl->negate = false;
    impl->excludes.clear();
    impl->main.clear();
    for (char c : pattern) {
      Node lit;
      lit.ch = c;
      impl->main.push_back(s::move(lit));
    }
  }
  CompiledPattern cp;
  cp.mImpl = s::move(impl);
  return cp;
}

bool CompiledPattern::matches(strv text) const {
  Matcher m{text, mImpl->nocase, mImpl->multibyte};
  if (m.whole(mImpl->main) == mImpl->negate) return false;
  for (const auto& ex : mImpl->excludes)
    if (m.whole(ex)) return false;
  return true;
}

s::size_t CompiledPattern::match_prefix(strv text, bool longest) const {
  Matcher m{text, mImpl->nocase, mImpl->multibyte};
  Positions ends = m.ends(mImpl->main);
  if (mImpl->negate) {
    // ^pat: the prefixes that pat does not match.
    Positions all;
    for (s::size_t p = 0, k = 0; p <= text.size(); ++p) {
      if (k < ends.size() and ends[k] == p) { ++k; continue; }
      if (m.boundary(p)) all.push_back(p);
    }
    ends = s::move(all);
  }
  auto ok = [&](s::size_t len) {
    Matcher pm{text.substr(0, len), mImpl->nocase, mImpl->multibyte};
    for (const auto& ex : mImpl->excludes)
      if (pm.whole(ex)) return false;
    return true;
  };
  if (longest) {
    for (auto it = ends.rbegin(); it != ends.rend(); ++it)
      if (ok(*it)) return *it;
  } else {
    for (s::size_t e : ends)
      if (ok(e)) return e;
  }
  return strv::npos;
}

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
    if (c == '*' or c == '?') return true;
    // `[` and `(` are pattern characters only when closed; a lone `[` is
    // the test command.
    if (c == '[') {
      PatternParser pp{s, false, i};
      Node n;
      if (pp.parse_class(n)) return true;
    }
    if (c == '(' and s.find(')', i + 1) != strv::npos) return true;
    if (c == '<') {
      PatternParser pp{s, false, i};
      Node n;
      if (pp.parse_range(n)) return true;
    }
    if (extended and (c == '^' or c == '#' or (c == '~' and i > 0))) return true;
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
  if (numeric) {
    auto digit = [](char c) { return s::isdigit(static_cast<unsigned char>(c)) != 0; };
    s::size_t i = 0;
    while (i < a.size() and i < b.size() and a[i] == b[i]) ++i;
    bool da = i < a.size() and digit(a[i]), db = i < b.size() and digit(b[i]);
    if (da or db) {
      while (i > 0 and digit(a[i - 1])) --i;
      s::size_t x = i, y = i;
      if (x < a.size() and digit(a[x]) and y < b.size() and digit(b[y])) {
        while (x < a.size() and a[x] == '0') ++x;
        while (y < b.size() and b[y] == '0') ++y;
        while (x < a.size() and y < b.size() and digit(a[x]) and a[x] == b[y]) { ++x; ++y; }
        bool mx = x < a.size() and digit(a[x]), my = y < b.size() and digit(b[y]);
        if (mx or my) {
          int cmp = mx and my ? static_cast<unsigned char>(a[x]) - static_cast<unsigned char>(b[y]) : 0;
          while (x < a.size() and y < b.size() and digit(a[x]) and digit(b[y])) { ++x; ++y; }
          mx = x < a.size() and digit(a[x]);
          my = y < b.size() and digit(b[y]);
          if (mx and not my) return 1;
          if (my and not mx) return -1;
          return cmp;
        }
      }
    }
  }
  if (int c = ::strcoll(a.c_str(), b.c_str())) return c;
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

struct Qualifiers {
  struct Test { char type; bool negate; };
  vec<Test> tests;
  bool nullglob = false;
  bool dots     = false;
  char sort_key = 0;       // n L m a c (0 = default name order)
  bool sort_desc = false;
  bool select   = false;   // [n] or [n,m]
  s::int64_t sel_lo = 1, sel_hi = 1;
};

// Splits zsh glob qualifiers `(…)` off the end of `raw`.  Only a final
// group with no `|` whose characters are all qualifier characters counts.
bool split_qualifiers(str& raw, Qualifiers& q) {
  if (raw.size() < 3 or raw.back() != ')' or raw[raw.size() - 2] == '\\')
    return true;
  s::size_t open = raw.rfind('(');
  if (open == str::npos or open == 0 or (open > 0 and raw[open - 1] == '\\'))
    return true;
  strv body(raw.data() + open + 1, raw.size() - open - 2);
  if (body.empty() or body.find('|') != strv::npos) return true;
  constexpr strv kQualChars = "./@*=p%rwxRWXNDoOnLmac^-[],0123456789";
  if (body.find_first_not_of(kQualChars) != strv::npos) return true;

  Qualifiers out;
  bool negate = false;
  for (s::size_t i = 0; i < body.size(); ++i) {
    char c = body[i];
    switch (c) {
      case '^': negate = not negate;
      break; case '-':                      // follow symlinks: stat() is used anyway
      break; case 'N': out.nullglob = true;
      break; case 'D': out.dots = true;
      break; case 'o': case 'O':
        if (i + 1 >= body.size()) return false;
        out.sort_key  = body[++i];
        out.sort_desc = c == 'O';
        if (strv("nLmac").find(out.sort_key) == strv::npos)
          return false;
      break; case '[': {
        s::size_t close = body.find(']', i);
        if (close == strv::npos) return false;
        str spec(body.substr(i + 1, close - i - 1));
        auto comma = spec.find(',');
        out.select = true;
        out.sel_lo = s::strtoll(spec.c_str(), nullptr, 10);
        out.sel_hi = comma == str::npos
            ? out.sel_lo : s::strtoll(spec.c_str() + comma + 1, nullptr, 10);
        i = close;
      }
      break; default:
        if (strv("./@*=p%rwxRWX").find(c) == strv::npos)
          return false;
        out.tests.push_back({c, negate});
    }
  }
  raw.resize(open);
  q = out;
  return true;
}

bool qual_test(char type, const str& path) {
  struct stat st{};
  if (type == '@') return ::lstat(path.c_str(), &st) == 0 and S_ISLNK(st.st_mode);
  if (::stat(path.c_str(), &st) != 0) return false;
  switch (type) {
    case '.': return S_ISREG(st.st_mode);
    case '/': return S_ISDIR(st.st_mode);
    case '*': return S_ISREG(st.st_mode) and (st.st_mode & 0111);
    case '=': return S_ISSOCK(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case '%': return S_ISCHR(st.st_mode) or S_ISBLK(st.st_mode);
    case 'r': return st.st_mode & S_IRUSR;
    case 'w': return st.st_mode & S_IWUSR;
    case 'x': return st.st_mode & S_IXUSR;
    case 'R': return st.st_mode & S_IROTH;
    case 'W': return st.st_mode & S_IWOTH;
    case 'X': return st.st_mode & S_IXOTH;
    default:  return false;
  }
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
    const OptionState& opts)
{
  str        raw = pat.raw;
  Qualifiers q;
  bool extended = opts.extended_glob() or (pat.flags & GLOB_EXTGLOB);
  if (opts.get(OPT_BAREGLOBQUAL) and not split_qualifiers(raw, q))
    return s::unexpected(ShellError{ErrorKind::BadPattern,
        "unknown file attribute: " + pat.raw});
  bool has_quals = raw.size() != pat.raw.size();

  // Under EXTENDED_GLOB, `a~b` excludes paths matching `b`; the exclusion
  // applies to the whole path, so split it off before walking segments.
  vec<CompiledPattern> excludes;
  if (extended) {
    int depth = 0;
    for (s::size_t i = 1; i < raw.size(); ++i) {
      char c = raw[i];
      if (c == '\\') { ++i; continue; }
      if (c == '(' or c == '[') ++depth;
      else if ((c == ')' or c == ']') and depth > 0) --depth;
      else if (c == '~' and depth == 0) {
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
  // An unclosed `[` is zsh's "bad pattern", except as the whole word;
  // under NO_BAD_PATTERN the word stays as it is.
  if (raw != "[")
    for (s::size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] == '\\') { ++i; continue; }
      if (raw[i] != '[') continue;
      PatternParser pp{raw, extended, i};
      Node n;
      if (not pp.parse_class(n)) {
        if (not opts.get(OPT_BADPATTERN)) return vec<str>{glob_unescape(pat.raw)};
        return s::unexpected(ShellError{ErrorKind::BadPattern,
            "bad pattern: " + glob_unescape(pat.raw)});
      }
      i = pp.i - 1;
    }
  if (not has_quals and not has_glob_chars(raw, extended))
    return vec<str>{glob_unescape(raw)};

  Walker w{q.dots or (pat.flags & GLOB_DOTFILES) or opts.get(OPT_GLOBDOTS),
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

  // Exclusions and qualifier tests.
  s::erase_if(cur, [&](const str& p) {
    for (const auto& ex : excludes)
      if (ex.matches(p)) return true;
    for (auto t : q.tests)
      if (qual_test(t.type, p) == t.negate) return true;
    return false;
  });

  bool numeric = opts.get(OPT_NUMERICGLOBSORT) or q.sort_key == 'n';
  if (q.sort_key == 0 or q.sort_key == 'n') {
    collate_sort(cur, numeric);
    if (q.sort_desc) s::reverse(cur.begin(), cur.end());
  } else {
    auto key = [&](const str& p) -> s::int64_t {
      struct stat st{};
      if (::stat(p.c_str(), &st) != 0) return 0;
      switch (q.sort_key) {
        case 'L': return static_cast<s::int64_t>(st.st_size);
        case 'm': return static_cast<s::int64_t>(st.st_mtime);
        case 'a': return static_cast<s::int64_t>(st.st_atime);
        default:  return static_cast<s::int64_t>(st.st_ctime);
      }
    };
    // zsh's `om` lists the newest first; `oL` the smallest first.
    bool newest_first = q.sort_key != 'L';
    s::stable_sort(cur.begin(), cur.end(), [&](const auto& a, const auto& b) {
      s::int64_t ka = key(a), kb = key(b);
      if (ka == kb) return collate(a, b, false) < 0;
      bool lt = newest_first ? ka > kb : ka < kb;
      return q.sort_desc ? not lt : lt;
    });
  }

  if (q.select and not cur.empty()) {
    auto       n  = static_cast<s::int64_t>(cur.size());
    s::int64_t lo = q.sel_lo < 0 ? n + q.sel_lo + 1 : q.sel_lo;
    s::int64_t hi = q.sel_hi < 0 ? n + q.sel_hi + 1 : q.sel_hi;
    lo = s::max<s::int64_t>(lo, 1);
    hi = s::min(hi, n);
    if (lo > hi) cur.clear();
    else cur = vec<str>(cur.begin() + (lo - 1), cur.begin() + hi);
  }

  if (opts.get(OPT_MARKDIRS))
    for (auto& p : cur)
      if (p.back() != '/' and is_dir(p)) p += '/';

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
