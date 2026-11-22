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
#include <array>
#include <bit>
#include <functional>
#include <utility>

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
  // A place in a branch where the case flags change (see `leaks`): before
  // node `at` of `seq`.
  struct FlagPoint {
    const PatternSeq* seq;
    s::size_t         at;
    bool              nocase, lower;
  };

  Kind kind = Kind::Lit;
  // Lit: a byte, or under MULTIBYTE a character.  `bad` marks a byte that
  // starts no character: it matches only the same byte where that is no
  // character either.
  char32_t ch  = 0;
  bool     bad = false;

  bool class_negated = false;                            // Class
  vec<s::pair<char32_t, char32_t>> ranges;
  vec<s::pair<int (*)(int), wctype_t>> classes;

  vec<PatternSeq> alts;                          // Group
  // Under EXTENDED_GLOB, per alternative: its `~` exclusions.
  vec<vec<PatternSeq>> alt_excludes;
  bool negate  = false;   // ^x: anything but its one alternative
  int  capture = -1;      // (#b): its backreference
  // For (#aN), which the backtracker keeps as zsh's flags at run time:
  // per alternative, the (#aN) globbing flags at its end set (-1: none);
  // whether the group's end puts the flags back, its branches having
  // changed them; whether it is the whole pattern's, whose alternatives
  // after the first start without (#aN).
  vec<int> alt_end_reset;
  bool     restores = false;
  bool     top      = false;

  bool       has_lo = false, has_hi = false;              // Range <lo-hi>
  s::int64_t lo = 0, hi = 0;

  sptr<Node> sub;                             // Repeat x# / x## / x(#cN,M)
  int  min     = 0;
  int  max     = -1;
  bool counted = false;   // (#cN,M), which zsh runs with a counter
  // (#cN,M) of a one-branch group that changes the case flags: where in
  // the branch, or in one-branch groups inside it, they change, and to
  // what.  When a further repeat fails after a change, zsh leaves the
  // flags last set on for what follows, for the literals of `leak_epoch`
  // (see Subject::leak_epoch).
  vec<FlagPoint> leaks;
  int            leak_epoch  = -1;
  int            entry_epoch = 0;   // Group: the epoch where it begins

  // The globbing flags in force here: (#i) (#l) and (#aN).
  bool nocase = false;
  bool lower  = false;
  int  approx = 0;
  // zsh sets the flags to this node's right before it (P_GFLAGS): globbing
  // flags that change something come before it, or a group that put them
  // back.
  bool reset  = false;
  // Lit: which of zsh's literal strings (P_EXACTLY) it is in, numbered in
  // the order of the pattern.  A string is a run of literals in a branch,
  // ended by anything else, globbing flags included; the literal a `#` or
  // (#cN,M) repeats is one of its own.
  int  run    = -1;
  // Which stretch of the pattern with the same case flags this is in:
  // zsh changes its flags at run time only at a flag change in the
  // pattern and at the end of a group that changed them.
  int  epoch  = 0;
};

// A top-level alternative that ends in `*` (or a one-character x# or x##)
// then literal text, split there: zsh matches it as tail_matches() does.
struct TailLiteral {
  PatternSeq            head;    // what comes before the `*`
  CompiledPattern::Node rep;     // the `*` or the repeat
  PatternSeq            text;    // the literal characters
  s::size_t             bytes = 0;   // their length as written
};

struct VmProgram;

struct CompiledPattern::Impl {
  // One Group: the alternatives at the top level, each with its `~`
  // exclusions.
  PatternSeq main;
  // Per top-level alternative, its TailLiteral form where zsh uses one.
  vec<opt<TailLiteral>> tails;
  int  captures   = 0;       // groups under (#b), at most 9
  bool set_match  = false;   // (#m) seen
  bool approx     = false;   // (#aN) with N > 0 seen
  int  head_approx = 0;      // (#aN) right at the start, in zsh's header
  int  runs       = 0;       // literal strings, see Node::run
  bool pure       = false;   // see PatternParser::Branch::pure
  bool multibyte  = false;   // ? [...] and literals take a UTF-8 character
  bool bad        = false;   // zsh's "bad pattern": it matches nothing
  // A (#cN,M) whose operand can match in more than one way from a place.
  // zsh's counter keeps where the last repeat began even as attempts
  // fail, so it rejects some matches the VM finds; the backtracker
  // decides and the VM only filters.
  bool stale      = false;
  // A (#cN,M) whose flags leak (Node::leaks), and whether the VM follows
  // them (vm_follows_leaks()); where it does not, the backtracker decides
  // without it.
  bool leaky      = false;
  bool vm_leaks   = false;
  // A repeat of a group inside another repeat (see nested_repeats()),
  // where zsh's marks reject some matches the VM finds: the backtracker
  // decides and the VM only filters.
  bool marks      = false;
  int  nodes      = 0;       // the pattern's size, for the backtracker's budget
  // The pattern compiled for the VM (see VmProgram).
  sptr<const VmProgram> vm;
};

namespace {

using Node = CompiledPattern::Node;
using Seq  = PatternSeq;
using Kind = Node::Kind;

// zsh reports the extents of the first nine (#b) groups.
constexpr int MAX_CAPTURES = 9;

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

bool single_char(const Node& n) {
  return n.kind == Kind::Lit or n.kind == Kind::Any or n.kind == Kind::Class;
}

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

  // `(#…)` at p[i]: globbing flags, which change `fl`; an anchor, (#s) or
  // (#e); or (#cN,M), repeating the piece just before it (`can_count`).
  // Anything else is a bad pattern, as in zsh.
  void parse_flags(PatFlags& fl, Seq& seq, bool can_count) {
    s::size_t close = p.find(')', i);
    if (close == strv::npos) { bad = true; return; }
    strv f = p.substr(i + 2, close - i - 2);
    i = close + 1;
    if (not f.empty() and f[0] == 'c') {
      parse_count(f.substr(1), seq, can_count);
      return;
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
          if (f.size() != 1) { bad = true; return; }
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
          if (end == num.c_str() or v < 0 or v > 254) { bad = true; return; }
          nf.approx = static_cast<int>(v);
          k += static_cast<s::size_t>(end - num.c_str());
        }
        break; default:
          bad = true;
          return;
      }
    }
    if (nf.nocase != fl.nocase or nf.lower != fl.lower) {
      ++epoch;
      level_flags = true;
    }
    fl = nf;
    if (fl.approx > 0) impl.approx = true;
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
        parse_flags(fl, seq, can_count);
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

// zsh's case-insensitive comparison of character `c` with pattern
// character `pc`: (#i) either case, (#l) a lower-case `pc` either case.
bool same_char(bool nocase, bool lower, char32_t c, char32_t pc, bool mb) {
  if (c == pc) return true;
  if (mb) {
    auto wc = static_cast<wint_t>(c), wp = static_cast<wint_t>(pc);
    if (nocase)
      return (::iswupper(wc) ? ::towlower(wc) : wc) == (::iswupper(wp) ? ::towlower(wp) : wp);
    return lower and ::iswlower(wp) and ::towupper(wp) == wc;
  }
  if (nocase or (lower and s::islower(static_cast<int>(pc))))
    return s::tolower(static_cast<int>(c)) == s::tolower(static_cast<int>(pc));
  return false;
}

// Whether character `c` is in the class; with `mb` it is a UTF-8
// character's code point.  As in zsh, (#i) (#l) and NO_CASE_GLOB leave
// classes alone: (#i)[a] does not match `A`.
bool class_has(const Node& n, char32_t c, bool mb) {
  bool in = false;
  for (auto [lo, hi] : n.ranges)
    if (c >= lo and c <= hi) { in = true; break; }
  if (not in)
    for (auto [fn, wt] : n.classes)
      if (mb and c >= 0x80 ? ::iswctype(static_cast<wint_t>(c), wt) != 0
                           : c < 0x100 and fn(static_cast<int>(c)) != 0) {
        in = true;
        break;
      }
  return in != n.class_negated;
}

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

// The subject both matchers read: s[.., lim), with `s` the whole string so
// that (#s) and (#e) are its ends.
struct Subject {
  // The character at p (< lim) and the position after it.  Under `mb` a
  // UTF-8 character is one; a byte that starts none, or one cut off by
  // lim, is a character of its own.
  char32_t at(s::size_t p, s::size_t& next) const {
    bool bad = false;
    return decode(p, next, bad);
  }
  // The same, with `bad` set for a byte that is no character.
  char32_t decode(s::size_t p, s::size_t& next, bool& bad) const {
    auto b = static_cast<unsigned char>(s[p]);
    if (not mb or b < 0x80) { next = p + 1; return b; }
    s::size_t len = 1;
    char32_t c = utf8_decode(s, p, len);
    if (len == 1 or p + len > lim) { next = p + 1; bad = true; return b; }
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
      bool nocase = n.nocase, lower = n.lower;
      if (n.epoch == leak_epoch) {
        nocase = leak_nocase;
        lower  = leak_lower;
      }
      auto b = static_cast<unsigned char>(s[p]);
      if (b < 0x80 and n.ch < 0x80 and not nocase and not lower) {
        next = p + 1;
        return b == n.ch;
      }
      bool bad = false;
      char32_t c = decode(p, next, bad);
      return bad == n.bad and (c == n.ch or (not bad and same_char(nocase, lower, c, n.ch, mb)));
    }
    char32_t c = at(p, next);
    return n.kind == Kind::Any or class_has(n, c, mb);
  }
  // Whether `t` matches from p (where its head ended) to lim, as zsh's
  // matcher tries it: the `*` or repeat takes all it can, then the text
  // must be the last t.bytes bytes, which the repeat must reach and leave
  // room for.
  bool tail_matches(const TailLiteral& t, s::size_t p) const {
    s::size_t greedy = lim, first = p, next = 0;
    if (t.rep.kind == Kind::Repeat) {
      for (greedy = p; one(*t.rep.sub, greedy, next);) greedy = next;
      if (t.rep.min > 0) {
        if (p >= lim) return false;
        at(p, first);
      }
    }
    if (t.bytes > lim - first or t.bytes < lim - greedy) return false;
    s::size_t q = lim - t.bytes;
    for (const auto& n : t.text) {
      if (not one(n, q, next)) return false;
      q = next;
    }
    return q == lim;
  }

  // Takes the flags a VM Leak register holds (VmProgram::RegKind::Leak).
  void set_leak(s::int64_t v) const {
    leak_epoch = v <= 0 ? -1 : static_cast<int>((v - 1) >> 2);
    leak_nocase = v > 0 and ((v - 1) & 1) != 0;
    leak_lower  = v > 0 and ((v - 1) & 2) != 0;
  }

  strv      s;
  s::size_t lim = 0;
  bool      mb  = false;
  // (#s) fails even at the start, (#e) even at the end.
  bool      notstart = false;
  bool      notend   = false;
  // The case flags a (#cN,M) leaked (Node::leak) for the literals of
  // this epoch, while the backtracker tries what follows it.
  mutable int  leak_epoch  = -1;
  mutable bool leak_nocase = false;
  mutable bool leak_lower  = false;
};

using Cont = func<bool(s::size_t)>;

// A backtracking matcher over s[.., lim) that tries the ways to match in
// the order zsh's does: alternatives left to right, `*`, repeats and
// <lo-hi> longest first.  It runs (#aN), and decides the patterns whose
// zsh quirks the VM does not follow (see backtracked()); tests compare the
// VM with it.  It gives the (#b) groups of a match and the end zsh's
// search reaches first.  `errs` counts the (#aN) errors made so far,
// put back where zsh's does (alternatives, x#, `*`, the tries at one
// error) and not elsewhere: errors an attempt that failed made stay
// counted after a (#cN,M) or <lo-hi>; `caps` the (#b) groups' extents,
// put back as alternatives fail.  It recurses per node matched, so on a
// subject too long for the stack, or after so many steps that the search
// is exponential, it gives up (`deep`) and fails.
struct Matcher : Subject {
  // Per x# node: the positions a repeat was tried from, with the errors
  // + 1 then, and whether its outermost x# is trying a repeat.
  struct Marks {
    umap<s::size_t, int> tried;
    bool                 active = false;
  };
  // The errors made and the (#aN) in force (`errs`, `cur_approx`).
  struct Tally {
    int errs, approx;
  };

  // Ready to match the pattern in `impl` against `subject`.
  Matcher(Subject subject, const CompiledPattern::Impl& impl)
      : Subject{subject},
        caps(static_cast<s::size_t>(impl.captures), {strv::npos, strv::npos}),
        cur_approx{impl.head_approx},
        budget{steps_for(impl, lim)} {}

  // The steps before the search counts as exponential: linear in the
  // subject, but with (#aN), whose repeats zsh's search tries with each
  // error count from each place, quadratic up to 2^28.
  static s::size_t steps_for(const CompiledPattern::Impl& impl, s::size_t lim) {
    auto nodes = static_cast<s::size_t>(impl.nodes);
    s::size_t linear = (s::size_t{1} << 22) + 16 * (lim + 1) * nodes;
    if (not impl.approx) return linear;
    s::size_t cap = s::size_t{1} << 28, n = lim + 1;
    s::size_t square = n > cap / n ? cap : s::min(cap, n * n * s::max<s::size_t>(nodes, 1));
    return s::max(linear, square);
  }

  // Whether to give up: the search has taken so many steps that it is
  // likely exponential (`spent`), or the stack is nearly used up.
  bool tick() const {
    if (++steps > budget) spent = deep = true;
    return deep;
  }
  bool low() const {
    if (stack_low()) deep = true;
    return tick();
  }

  // zsh's error count and (#aN) at run time, which it puts back after
  // an attempt that failed in some places only (see `errs`).
  Tally tally() const { return {errs, cur_approx}; }
  void put(Tally t) const {
    errs       = t.errs;
    cur_approx = t.approx;
  }
  // The (#aN) in force once n is reached, and reaching it.
  int approx(const Node& n) const { return n.reset ? n.approx : cur_approx; }
  void enter(const Node& n) const {
    if (n.reset) cur_approx = n.approx;
  }

  // With (#aN), a node other than a literal string that fails at pos
  // (zsh's last gasp): the input character there is an error, and the
  // node is tried again after it.  As in zsh, the error counts even at
  // the end, where there is no character to take.
  bool skip(const Node& n, s::size_t pos, const Cont& k) const {
    if (cur_approx <= errs) return false;
    ++errs;
    if (pos >= lim) return false;
    s::size_t next = 0;
    at(pos, next);
    return reached(n, next, k);
  }

  // A `?` or `[…]`: with (#aN) it never matches a wrong character, but
  // extra ones before it are errors.
  bool single(const Node& n, s::size_t pos, const Cont& k) const {
    s::size_t next = 0;
    if (one(n, pos, next)) return k(next);
    return skip(n, pos, k);
  }

  // zsh's P_EXACTLY: the literal string lits[0, len) (one Node::run) from
  // its character j at pos.  When it matches, what follows decides, with
  // no errors tried here; at the first character that fails, with errors
  // left, zsh tries in turn an extra input character, two characters the
  // wrong way round, a wrong one and a missing one, each as one error,
  // and goes on with the rest of the string.
  bool exact(const Node* lits, s::size_t len, s::size_t j, s::size_t pos, const Cont& k) const {
    if (low()) return false;
    s::size_t next = 0;
    while (j < len and one(lits[j], pos, next)) {
      ++j;
      pos = next;
    }
    if (j == len) return k(pos);
    // The string failed: zsh's exactend is its end now.
    exact_end = lits[0].run;
    if (cur_approx <= errs) return false;
    ++errs;
    // zsh puts its flags back before each try, the case flags a (#cN,M)
    // leaks too.
    Tally one_more = tally();
    const Node::FlagPoint* last = passed;
    auto again = [&] {
      put(one_more);
      passed = last;
    };
    if (pos < lim) {
      s::size_t in1 = 0;
      at(pos, in1);
      if (resume(lits, len, j, in1, k)) return true;
      again();
      // zsh compares with the end of the string that failed last, which
      // the tries before may have made another one: one before this
      // string leaves nothing to compare, one after it pattern bytes.
      s::size_t n1 = 0, n2 = 0;
      if (exact_end >= lits[0].run and j + 1 < len and in1 < lim
          and one(lits[j + 1], pos, n1) and one(lits[j], in1, n2)) {
        if (resume(lits, len, j + 2, n2, k)) return true;
        again();
      }
      if (resume(lits, len, j + 1, in1, k)) return true;
      again();
    }
    return resume(lits, len, j + 1, pos, k);
  }

  // The string resumed at its character j after an error, up to the end
  // of the string that failed last (see exact()).  When that is a later
  // string, zsh compares the compiled pattern's bytes up to its end with
  // the input and fails, as the 16 or more such bytes cannot be errors,
  // its tries at them using up the errors left.
  bool resume(const Node* lits, s::size_t len, s::size_t j, s::size_t pos, const Cont& k) const {
    if (exact_end > lits[0].run) {
      errs = s::max(errs, cur_approx);
      return false;
    }
    if (exact_end < lits[0].run) return k(pos);
    return exact(lits, len, j, pos, k);
  }

  bool node(const Node& n, s::size_t pos, const Cont& k) const {
    enter(n);
    return reached(n, pos, k);
  }

  // Node n from pos, once reached (its Node::reset done).
  bool reached(const Node& n, s::size_t pos, const Cont& k) const {
    if (low()) return false;
    switch (n.kind) {
      case Kind::Lit:
        return exact(&n, 1, 0, pos, k);

      case Kind::Any: case Kind::Class:
        return single(n, pos, k);

      case Kind::Star: {
        // Greedy, as zsh's: the longest run first, so (#b)(*)X(*) on
        // aXbXc gives aXb and c.  The errors are put back before each
        // shorter try, not after the last.
        Tally saved = tally();
        for (s::size_t e = lim + 1; e-- > pos and not tick();) {
          if (not boundary(e)) continue;
          put(saved);
          if (k(e)) return true;
        }
        return false;
      }

      case Kind::AtStart:
        if (pos == 0 and not notstart) return k(pos);
        return skip(n, pos, k);

      case Kind::AtEnd:
        // (#e): the end of the whole string; with (#aN) the characters
        // before it are errors.
        if (pos == s.size() and not notend) return k(pos);
        return skip(n, pos, k);

      case Kind::Group: {
        // zsh puts the errors back after each alternative that failed, but
        // runs a group of one branch with no `~` as part of what is around.
        bool choice = n.alts.size() > 1 or n.negate or not n.alt_excludes[0].empty();
        Tally saved = tally();
        for (s::size_t a = 0; a < n.alts.size(); ++a) {
          if (alt(n, a, pos, k)) return true;
          if (choice) put(saved);
        }
        return false;
      }

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
        for (s::size_t len = vals.size(); len > 0 and not tick(); --len) {
          s::int64_t x = vals[len - 1];
          if ((not n.has_lo or x >= n.lo) and (not n.has_hi or x <= n.hi) and k(pos + len))
            return true;
        }
        return skip(n, pos, k);
      }

      case Kind::Repeat:
        return repeat(n, pos, k);

      default:
        assert(false);  // should never get here
    }
    return false;
  }

  // Alternative `a` of group `g` from pos, then k.
  bool alt(const Node& g, s::size_t a, s::size_t pos, const Cont& k) const {
    if (g.top and a > 0) cur_approx = 0;
    auto finish = [&](s::size_t e) {
      // The flags its end sets, then those the group puts back.
      if (g.alt_end_reset[a] >= 0) cur_approx = g.alt_end_reset[a];
      if (g.restores) cur_approx = g.approx;
      for (const auto& ex : g.alt_excludes[a])
        if (spans(ex, pos, e)) return false;
      if (g.capture < 0) return k(e);
      auto c = static_cast<s::size_t>(g.capture);
      auto saved = caps[c];
      caps[c] = {pos, e};
      if (k(e)) return true;
      caps[c] = saved;
      return false;
    };
    if (g.negate) {
      Tally saved = tally();
      for (s::size_t e = lim + 1; e-- > pos and not tick(); put(saved))
        if (boundary(e) and not spans(g.alts[a], pos, e) and finish(e)) return true;
      return false;
    }
    return seq(g.alts[a], 0, pos, finish);
  }

  // Whether `q` matches exactly s[b, e); the captures of the match
  // around it are left alone.  (Its repeats are nodes of their own, so
  // their marks are too.)
  bool spans(const Seq& q, s::size_t b, s::size_t e) const {
    auto saved = caps;
    // zsh tests an exclusion with no errors made yet and no (#aN), and
    // only once the whole pattern matched, so not before what the string
    // that failed last means here.
    Tally outer = tally();
    int   end   = exact_end;
    put({0, 0});
    bool ok = seq(q, 0, b, [&](s::size_t x) { return x == e; });
    put(outer);
    exact_end = end;
    caps = s::move(saved);
    return ok;
  }

  // x#, x##, (#cN,M): greedy, as zsh's: one more repeat is tried before
  // what follows.
  bool repeat(const Node& n, s::size_t pos, const Cont& k) const {
    const Node& sub = *n.sub;
    if (single_char(sub) and sub.approx == 0 and not n.counted) {
      // One character at a time (zsh's P_ONEHASH): the run, then each end
      // from the longest.
      vec<s::size_t> ends{pos};
      for (s::size_t q = pos, next = 0;
           (n.max < 0 or ends.size() <= static_cast<s::size_t>(n.max)) and one(sub, q, next);)
        ends.push_back(q = next);
      Tally saved = tally();
      for (s::size_t c = ends.size(); c-- > static_cast<s::size_t>(n.min) and not tick();) {
        put(saved);
        if (k(ends[c])) return true;
      }
      return false;
    }
    if (n.counted) {
      // zsh's P_COUNTSTART: a count inside one of the same node, through
      // an enclosing repeat, leaves the outer one's start as it was.
      auto it = count_starts.find(&n);
      opt<s::size_t> outer = it == count_starts.end() ? opt<s::size_t>{} : it->second;
      bool ok = counted(n, 0, pos, k);
      if (outer) count_starts[&n] = *outer;
      return ok;
    }
    // x## is x then x#.
    if (n.min == 0) return loop(n, pos, k);
    return node(sub, pos, [&](s::size_t e) { return loop(n, e, k); });
  }

  // The optional repeats of x# (zsh's P_WBRANCH): while the outermost
  // x# on this node tries a repeat, another is tried from a position only
  // once, with no fewer errors since.  So a repeat that ends where it
  // began ends x#, its groups kept, and nested repeats stay polynomial.
  // As in zsh, the marks go before the outermost one tries what follows.
  bool loop(const Node& n, s::size_t pos, const Cont& k) const {
    Marks& m = marks[&n];
    bool owner = not m.active;
    if (owner) {
      // A map that grew large is dropped rather than cleared bucket by
      // bucket.
      if (m.tried.bucket_count() > 64 and m.tried.size() * 4 < m.tried.bucket_count())
        m.tried = {};
      else
        m.tried.clear();
      m.active = true;
    }
    // zsh marks the place with the errors now even when an attempt with
    // fewer was made there.
    bool ok = false;
    Tally saved = tally();
    auto [it, fresh] = m.tried.try_emplace(pos, errs + 1);
    bool again = fresh or errs + 1 < it->second;
    it->second = errs + 1;
    if (again) ok = node(*n.sub, pos, [&](s::size_t e) { return loop(n, e, k); });
    if (owner) m.active = false;
    if (ok) return true;
    put(saved);
    if (k(pos)) return true;
    put(saved);
    return false;
  }

  // (#cN,M) after `cur` repeats (zsh's P_COUNT): a repeat that matched
  // nothing ends the count once it reaches N.  As in zsh, where the last
  // repeat began is kept per node and not put back as attempts fail.
  bool counted(const Node& n, int cur, s::size_t pos, const Cont& k) const {
    auto& began = count_starts[&n];
    if (cur > 0 and cur >= n.min and pos == began) return k(pos);
    began = pos;
    bool tried = n.max < 0 or cur < n.max;
    // Watch which changes of the flags the repeat passes.
    bool watching = tried and not n.leaks.empty();
    const Node*            outer_watch  = watch;
    const Node::FlagPoint* outer_passed = passed;
    if (watching) {
      watch  = &n;
      passed = nullptr;
    }
    bool more = tried and node(*n.sub, pos, [&](s::size_t e) { return counted(n, cur + 1, e, k); });
    const Node::FlagPoint* last = watching ? passed : nullptr;
    if (watching) {
      // A repeat inside the outer one's attempt is a later part of it.
      watch  = outer_watch;
      passed = watch == &n and last ? last : outer_passed;
    }
    if (more) return true;
    if (cur < n.min) return false;
    // The repeat that failed passed a change of the case flags, and in
    // one-branch groups nothing put them back: the last change it passed
    // stays on.
    if (not last) return k(pos);
    int  epoch  = leak_epoch;
    bool nocase = leak_nocase, lower = leak_lower;
    leak_epoch  = n.leak_epoch;
    leak_nocase = last->nocase;
    leak_lower  = last->lower;
    bool ok = k(pos);
    leak_epoch  = epoch;
    leak_nocase = nocase;
    leak_lower  = lower;
    return ok;
  }

  // Branch q of the watched count got to node i.
  void note(const Seq& q, s::size_t i) const {
    for (const auto& fp : watch->leaks)
      if (fp.seq == &q and fp.at == i) passed = &fp;
  }
  bool watched(const Seq& q) const {
    if (not watch) return false;
    for (const auto& fp : watch->leaks)
      if (fp.seq == &q) return true;
    return false;
  }

  bool plain(const Seq& q, s::size_t j) const {
    return j < q.size() and approx(q[j]) == 0 and single_char(q[j]);
  }

  bool seq(const Seq& q, s::size_t i, s::size_t pos, const Cont& k) const {
    if (low()) return false;
    bool notes = watched(q);
    // Plain one-character nodes (no (#aN) errors allowed) are matched in a
    // loop, without a continuation each: they have one way to match.
    for (s::size_t next = 0; plain(q, i); ++i, pos = next) {
      if (notes) note(q, i);
      enter(q[i]);
      if (not one(q[i], pos, next)) {
        if (q[i].kind == Kind::Lit) exact_end = q[i].run;
        return false;
      }
    }
    if (notes) note(q, i);
    if (i == q.size()) return k(pos);
    enter(q[i]);
    // A literal string with (#aN), as one.
    if (q[i].kind == Kind::Lit) {
      s::size_t r = i + 1;
      while (r < q.size() and q[r].kind == Kind::Lit and q[r].run == q[i].run) ++r;
      return exact(&q[i], r - i, 0, pos, [&](s::size_t e) { return seq(q, r, e, k); });
    }
    // `*` then a plain character: only where that character is (zsh looks
    // ahead so only without (#aN)).
    if (q[i].kind == Kind::Star and cur_approx == 0 and plain(q, i + 1)) {
      for (s::size_t e = lim; e-- > pos and not tick();) {
        s::size_t next = 0;
        if (boundary(e) and one(q[i + 1], e, next) and seq(q, i + 1, e, k)) return true;
      }
      return false;
    }
    return node(q[i], pos, [&](s::size_t e) { return seq(q, i + 1, e, k); });
  }

  // Whether the characters in s[e, lim) are at most `room`.
  bool within(s::size_t e, int room) const {
    for (s::size_t next = 0; e < lim; e = next, --room) {
      if (room <= 0) return false;
      at(e, next);
    }
    return true;
  }

  // All of s[b, lim); with (#aN) still in force at the end of the
  // alternative, extra characters there are errors too.  With `tails`,
  // the alternatives that have a TailLiteral form match as zsh's do in a
  // whole match.
  bool whole(const CompiledPattern::Impl& impl, s::size_t b, bool tails) const {
    const Node& top = impl.main.front();
    for (s::size_t a = 0; a < top.alts.size(); ++a) {
      // Alternatives after the first start without (#aN), as in alt().
      put({0, a > 0 ? 0 : impl.head_approx});
      if (const auto& t = impl.tails[a]; tails and t) {
        if (seq(t->head, 0, b, [&](s::size_t x) { return tail_matches(*t, x); })) return true;
        continue;
      }
      // zsh's P_END takes the extra characters one error at a time; it
      // compares a plain string (PAT_PURES) as a string.
      auto end = [&](s::size_t e) {
        int approx = impl.pure ? 0 : cur_approx;
        if (within(e, approx - errs)) return true;
        errs = s::max(errs, approx);
        return false;
      };
      if (alt(top, a, b, end)) return true;
    }
    return false;
  }

  // The end zsh's search reaches first with the end left open (its
  // PAT_NOANCH): npos when there is none.
  s::size_t open(const CompiledPattern::Impl& impl, s::size_t b) const {
    s::size_t found = strv::npos;
    node(impl.main.front(), b, [&](s::size_t e) {
      found = e;
      return true;
    });
    return found;
  }

  // The groups of the match found, counting from b.
  void report(s::size_t b, PatternMatch& info) const {
    info.groups = caps;
    for (auto& [gb, ge] : info.groups)
      if (gb != strv::npos) {
        gb -= b;
        ge -= b;
      }
  }

  mutable vec<s::pair<s::size_t, s::size_t>> caps;
  mutable int errs = 0;
  // The (#aN) at run time: zsh sets its flags where Node::reset says and
  // at the ends of branches and groups (Node::alt_end_reset, restores),
  // and puts them back with the errors.
  mutable int cur_approx = 0;
  // The Node::run of the literal string that failed last (zsh's
  // exactend), which a string resumed after an error compares up to.
  mutable int exact_end = -1;
  mutable umap<const Node*, Marks> marks;
  // The (#cN,M) whose repeat is being tried, and the last change of the
  // flags that repeat passed, for flags it leaks.
  mutable const Node*            watch  = nullptr;
  mutable const Node::FlagPoint* passed = nullptr;
  // Per (#cN,M), where its last repeat began.
  mutable umap<const Node*, s::size_t> count_starts;
  s::size_t         budget;
  mutable s::size_t steps = 0;
  mutable bool      deep  = false;
  mutable bool      spent = false;
};

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

// Whether n may match nothing.
bool nullable(const Node& n) {
  switch (n.kind) {
    case Kind::Lit: case Kind::Any: case Kind::Class: case Kind::Range:
      return false;
    case Kind::Repeat:
      return n.min == 0 or nullable(*n.sub);
    case Kind::Group:
      if (n.negate) return true;
      for (const auto& alt : n.alts)
        if (s::all_of(alt.begin(), alt.end(), [](const Node& x) { return nullable(x); }))
          return true;
      return false;
    default:
      return true;
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

}  // namespace

// ---------------------------------------------------------------------------
// The pattern VM, for patterns without (#aN): a Pike VM.  The pattern is
// compiled to instructions and run over the subject one character at a
// time with all its threads at once, so time is the subject's length
// times the program's, and the stack grows with neither.  The threads are
// kept in the order zsh's backtracking tries them (alternatives left to
// right; `*`, x#, (#cN,M) and <lo-hi> longest first), so the first thread
// to match is the match zsh finds, with its (#b) groups, and with the end
// left open the first to get to the end of the pattern gives the end
// zsh's ##, / and // take.  Two threads at the same instruction with the
// same state go on alike, so only the first is kept.
//
// `^x` and the exclusions of `x~y` run as programs of their own (contexts)
// beside the one they are in, as sets: each of their states carries the
// starts it was reached from, so the starts that pile up after a `*`
// share one run, and *(^*(^*b))c stays a low power of the subject's
// length.
// ---------------------------------------------------------------------------

struct VmProgram {
  enum class Op : s::uint8_t {
    Char,        // a character `node` (a Lit, Any or Class) matches, then x
    AnyChar,     // any character, then x
    Digit,       // a digit of a <lo-hi>, read by range x in state y
    Split,       // x, then y
    Loop,        // x#, `*`, ^'s body: x (another repeat), then y; in reg (if
                 // >= 0) where the repeat began.  As zsh's marks (P_WBRANCH),
                 // arriving again at a position goes to y.
    Jmp,         // x
    Boundary,    // a character may start here
    Save,        // group register x = the position
    AtStart,     // (#s)
    AtEnd,       // (#e)
    CountInit,   // (#cN,M) of `node`, counter in reg: no repeats yet
    CountTest,   // another repeat (x) or what follows (y)
    CountInc,    // a repeat matched; then x
    CtxStart,    // starts context x here, its start in reg
    NegExit,     // ^: a boundary where context x from reg does not match
    ExclCheck,   // ~: context x from reg does not match up to here
    Tail,        // TailLiteral x matches the rest: a whole match
    Match,
  };
  // What a state register holds: a (#cN,M)'s count, where its (or an
  // x#'s) repeat being tried began, where a context started, or the case
  // flags a (#cN,M) leaked (0 none; else 1 + 4 * its leak_epoch + 1 for
  // (#i) + 2 for (#l)).
  enum class RegKind : s::uint8_t { Count, Iter, Ctx, Leak };

  struct Inst {
    Op          op    = Op::Match;
    s::int32_t  x     = 0;
    s::int32_t  y     = 0;
    s::int32_t  reg   = 0;
    const Node* node  = nullptr;
    // The state registers that matter here: live[live, live + nlive).
    s::uint32_t live  = 0;
    s::uint32_t nlive = 0;
  };
  struct Reg {
    RegKind    kind = RegKind::Count;
    int        min  = 0;    // Count: its (#cN,M)'s bounds
    int        max  = -1;
    s::int32_t ctx  = -1;   // Ctx: which context
  };
  // A <lo-hi> as an automaton over digits: [pc0 + 2s] decides whether
  // state s may end the number, [pc0 + 2s + 1] reads another digit.
  struct Range {
    vec<s::array<s::int32_t, 10>> next;   // -1: no number in range follows
    vec<char>                     accepting;
    s::int32_t                    pc0 = 0;
  };
  // A program run beside the one it is in: the x of `^x`, or the
  // exclusions of a branch.
  struct Context {
    s::int32_t entry = 0;
  };

  vec<Inst>       code;
  vec<s::int32_t> live;
  vec<Reg>        regs;       // the state registers
  vec<Range>      ranges;
  vec<Context>    ctxs;         // inner ones after those they are in
  s::int32_t      whole  = 0;   // a whole match, with the TailLiteral forms
  s::int32_t      prefix = 0;   // a match of a prefix
  int             caps   = 0;   // the group registers, two per group
  bool            tails  = false;  // `whole` has TailLiteral forms
  s::int32_t      leak   = -1;  // the Leak register, live everywhere, if any
  // ASCII characters that every match has as a literal without case flags:
  // where one is missing there is nothing to run.
  vec<char>       needs;
};

namespace {

using Op   = VmProgram::Op;
using Inst = VmProgram::Inst;

// The automaton of `<lo-hi>`: the digits of a number, with any leading
// zeros, whose value (saturating at INT64_MAX, as zsh's) is in range.
VmProgram::Range range_automaton(const Node& n) {
  // A saturated value is in range of an upper bound of INT64_MAX, and no
  // value is below a lower bound of 0.
  bool has_hi = n.has_hi and n.hi < INT64_MAX;
  bool has_lo = n.has_lo and n.lo > 0;
  str lo = has_lo ? s::to_string(n.lo) : str();
  str hi = has_hi ? s::to_string(n.hi) : str();
  int cap = static_cast<int>(s::max(lo.size(), hi.size())) + 1;
  // A state: whether a digit was read, how many significant digits were,
  // and how they compare with as many digits of lo and of hi.
  struct State {
    bool any = false;
    int  k   = 0;
    int  clo = 0, chi = 0;
  };
  auto cmp = [](int k, int c, const str& v) {
    int len = static_cast<int>(v.size());
    return k < len ? -1 : k > len ? 1 : c;
  };
  auto id_of = [&](const State& st) {
    return ((static_cast<int>(st.any) * 64 + st.k) * 3 + st.clo + 1) * 3 + st.chi + 1;
  };
  VmProgram::Range r;
  vec<State> states{State{}};
  umap<int, s::int32_t> ids{{id_of(State{}), 0}};
  for (s::size_t i = 0; i < states.size(); ++i) {
    State st = states[i];
    s::array<s::int32_t, 10> next{};
    for (int d = 0; d < 10; ++d) {
      // A leading zero leaves the value 0.
      State t;
      t.any = true;
      if (st.k > 0 or d > 0) {
        auto digit_cmp = [&](int c, const str& v) {
          if (c != 0 or st.k >= static_cast<int>(v.size())) return c;
          int w = v[static_cast<s::size_t>(st.k)] - '0';
          return d < w ? -1 : d > w ? 1 : 0;
        };
        t.k   = s::min(st.k + 1, cap);
        t.clo = digit_cmp(st.clo, lo);
        t.chi = digit_cmp(st.chi, hi);
      }
      if (has_hi and cmp(t.k, t.chi, hi) > 0) {
        next[static_cast<s::size_t>(d)] = -1;
        continue;
      }
      auto [it, fresh] = ids.try_emplace(id_of(t), static_cast<s::int32_t>(states.size()));
      if (fresh) states.push_back(t);
      next[static_cast<s::size_t>(d)] = it->second;
    }
    r.next.push_back(next);
  }
  for (const auto& st : states)
    r.accepting.push_back(st.any and (not has_lo or cmp(st.k, st.clo, lo) >= 0)
                          and (not has_hi or cmp(st.k, st.chi, hi) <= 0));
  return r;
}

// Compiles a pattern's tree for the VM.
struct VmCompiler {
  // A context's program, compiled after the code it is started from.
  struct Pending {
    s::int32_t      ctx;
    vec<const Seq*> seqs;   // it matches when one of them does
  };
  // How long the operand of x## may be and still be compiled twice.
  static constexpr s::int32_t MAX_CODE = 1 << 15;

  s::int32_t at() const { return static_cast<s::int32_t>(p.code.size()); }

  s::int32_t emit(Op op, s::int32_t x = 0, s::int32_t y = 0, s::int32_t reg = 0,
                  const Node* node = nullptr) {
    Inst in;
    in.op    = op;
    in.x     = x;
    in.y     = y;
    in.reg   = reg;
    in.node  = node;
    in.live  = static_cast<s::uint32_t>(p.live.size());
    in.nlive = static_cast<s::uint32_t>(live.size());
    p.live.insert(p.live.end(), live.begin(), live.end());
    p.code.push_back(in);
    return at() - 1;
  }

  // The first of `targets` that matches, in order.
  s::int32_t chain(const vec<s::int32_t>& targets) {
    if (targets.size() == 1) return targets[0];
    s::int32_t first = at();
    for (s::size_t a = 0; a + 1 < targets.size(); ++a) {
      s::int32_t pc = at();
      emit(Op::Split, targets[a], a + 2 < targets.size() ? pc + 1 : targets[a + 1]);
    }
    return first;
  }

  s::int32_t reg(VmProgram::Reg r) {
    p.regs.push_back(r);
    return static_cast<s::int32_t>(p.regs.size() - 1);
  }

  // A context that matches when one of `seqs` does, and its start's
  // register.
  s::pair<s::int32_t, s::int32_t> context(vec<const Seq*> seqs) {
    auto c = static_cast<s::int32_t>(p.ctxs.size());
    p.ctxs.push_back({0});
    pending.push_back({c, s::move(seqs)});
    return {c, reg({VmProgram::RegKind::Ctx, 0, -1, c})};
  }

  void seq(const Seq& q, bool caps) {
    for (const auto& n : q) node(n, caps);
  }

  // Node n; with `caps`, saving its groups.
  void node(const Node& n, bool caps) {
    switch (n.kind) {
      case Kind::Lit: case Kind::Any: case Kind::Class: {
        s::int32_t pc = emit(Op::Char, 0, 0, 0, &n);
        p.code[static_cast<s::size_t>(pc)].x = pc + 1;
      }

      break; case Kind::Star: {
        // Greedy: another character first, and an end only where a
        // character may start.
        s::int32_t l = emit(Op::Loop, 0, 0, -1);
        emit(Op::AnyChar, l);
        emit(Op::Boundary);
        p.code[static_cast<s::size_t>(l)].x = l + 1;
        p.code[static_cast<s::size_t>(l)].y = l + 2;
      }

      break; case Kind::AtStart: emit(Op::AtStart);

      break; case Kind::AtEnd: emit(Op::AtEnd);

      break; case Kind::Range: range(n);

      break; case Kind::Group: group(n, caps);

      break; case Kind::Repeat: repeat(n, caps);

      break; default:
        assert(false);  // should never get here
    }
  }

  void range(const Node& n) {
    auto r = range_automaton(n);
    r.pc0 = at();
    auto states = static_cast<s::int32_t>(r.next.size());
    s::int32_t exit = r.pc0 + 2 * states;
    auto idx = static_cast<s::int32_t>(p.ranges.size());
    for (s::int32_t st = 0; st < states; ++st) {
      s::int32_t read = r.pc0 + 2 * st + 1;
      if (r.accepting[static_cast<s::size_t>(st)]) emit(Op::Split, read, exit);
      else emit(Op::Jmp, read);
      emit(Op::Digit, idx, st);
    }
    p.ranges.push_back(s::move(r));
  }

  void group(const Node& g, bool caps) {
    bool saves = caps and g.capture >= 0;
    if (saves) emit(Op::Save, 2 * g.capture);
    if (g.negate) {
      // Anything, longest first, but where the context of the one branch
      // matches.
      auto [c, r] = context({&g.alts[0]});
      emit(Op::CtxStart, c, 0, r);
      live.push_back(r);
      s::int32_t l = emit(Op::Loop, 0, 0, -1);
      emit(Op::AnyChar, l);
      emit(Op::NegExit, c, 0, r);
      live.pop_back();
      p.code[static_cast<s::size_t>(l)].x = l + 1;
      p.code[static_cast<s::size_t>(l)].y = l + 2;
    } else {
      vec<s::int32_t> jumps;
      for (s::size_t a = 0; a < g.alts.size(); ++a) {
        s::int32_t split = a + 1 < g.alts.size() ? emit(Op::Split) : -1;
        branch(g, a, caps);
        jumps.push_back(emit(Op::Jmp));
        if (split >= 0) {
          p.code[static_cast<s::size_t>(split)].x = split + 1;
          p.code[static_cast<s::size_t>(split)].y = at();
        }
      }
      for (s::int32_t j : jumps) p.code[static_cast<s::size_t>(j)].x = at();
    }
    if (saves) emit(Op::Save, 2 * g.capture + 1);
  }

  // Branch a of group g, with its exclusions.
  void branch(const Node& g, s::size_t a, bool caps) {
    const auto& ex = g.alt_excludes[a];
    if (ex.empty()) {
      seq(g.alts[a], caps);
      return;
    }
    vec<const Seq*> seqs;
    for (const auto& q : ex) seqs.push_back(&q);
    auto [c, r] = context(s::move(seqs));
    emit(Op::CtxStart, c, 0, r);
    live.push_back(r);
    seq(g.alts[a], caps);
    emit(Op::ExclCheck, c, 0, r);
    live.pop_back();
  }

  void repeat(const Node& n, bool caps) {
    const Node& sub = *n.sub;
    if (n.counted) {
      // zsh's P_COUNT: a counter, and where the repeat being tried began,
      // so that one that matched nothing ends the count.
      auto [it, fresh] = counters.try_emplace(&n, 0);
      if (fresh) {
        it->second = reg({VmProgram::RegKind::Count, n.min, n.max, -1});
        reg({VmProgram::RegKind::Iter, 0, -1, -1});
      }
      s::int32_t r = it->second;
      emit(Op::CountInit, 0, 0, r, &n);
      live.push_back(r);
      live.push_back(r + 1);
      s::int32_t t = emit(Op::CountTest, 0, 0, r, &n);
      p.code[static_cast<s::size_t>(t)].x = at();
      node(sub, caps);
      emit(Op::CountInc, t, 0, r, &n);
      live.pop_back();
      live.pop_back();
      p.code[static_cast<s::size_t>(t)].y = at();
      return;
    }
    // x## is x then x#.  Compiled twice, the first x and the repeats are
    // tried as zsh tries them: a repeat from where the first one ended
    // before the first one's other ways.
    s::int32_t first = at();
    if (n.min > 0) {
      node(sub, caps);
      if (at() + (at() - first) > MAX_CODE) {
        emit(Op::Loop, first, at() + 1, -1);
        return;
      }
    }
    // A repeat that may match nothing notes where it began: its states are
    // not those of a repeat that began before (zsh's are other frames),
    // and one that ends where it began arrives at the loop again.
    s::int32_t iter = -1;
    if (nullable(sub)) {
      auto [it, fresh] = counters.try_emplace(&n, 0);
      if (fresh) it->second = reg({VmProgram::RegKind::Iter, 0, -1, -1});
      iter = it->second;
    }
    s::int32_t l = emit(Op::Loop, 0, 0, iter);
    if (iter >= 0) live.push_back(iter);
    p.code[static_cast<s::size_t>(l)].x = at();
    node(sub, caps);
    emit(Op::Jmp, l);
    if (iter >= 0) live.pop_back();
    p.code[static_cast<s::size_t>(l)].y = at();
  }

  // The whole pattern: the main program's two entries, then the contexts.
  void top(const CompiledPattern::Impl& impl, bool leaks) {
    const Node& g = impl.main.front();
    p.caps = 2 * impl.captures;
    if (leaks) {
      p.leak = reg({VmProgram::RegKind::Leak, 0, -1, -1});
      live.push_back(p.leak);
    }
    vec<s::int32_t> alts, wholes, jumps;
    for (s::size_t a = 0; a < g.alts.size(); ++a) {
      alts.push_back(at());
      branch(g, a, true);
      jumps.push_back(emit(Op::Jmp));
    }
    for (s::size_t a = 0; a < g.alts.size(); ++a) {
      const auto& t = impl.tails[a];
      if (not t) {
        wholes.push_back(alts[a]);
        continue;
      }
      wholes.push_back(at());
      p.tails = true;
      seq(t->head, true);
      emit(Op::Tail, static_cast<s::int32_t>(a));
    }
    s::int32_t m = emit(Op::Match);
    for (s::int32_t j : jumps) p.code[static_cast<s::size_t>(j)].x = m;
    p.prefix = chain(alts);
    p.whole  = chain(wholes);
    // Contexts met while compiling these are added as they are met.
    for (s::size_t i = 0; i < pending.size(); ++i) {
      Pending d = pending[i];
      vec<s::int32_t> starts, ends;
      for (const Seq* q : d.seqs) {
        starts.push_back(at());
        seq(*q, false);
        ends.push_back(emit(Op::Jmp));
      }
      s::int32_t done = emit(Op::Match);
      for (s::int32_t j : ends) p.code[static_cast<s::size_t>(j)].x = done;
      p.ctxs[static_cast<s::size_t>(d.ctx)].entry = chain(starts);
    }
  }

  VmProgram&                    p;
  vec<s::int32_t>               live;      // the registers live where code goes
  umap<const Node*, s::int32_t> counters;  // per (#cN,M) and x#, its registers
  vec<Pending>                  pending;
};

// A set of keys of `width` words, emptied in constant time.
struct KeyTable {
  void reset(s::size_t w) {
    width = w;
    keys.clear();
    count = 0;
    if (++stamp == 0) {
      s::fill(marks.begin(), marks.end(), 0);
      stamp = 1;
    }
  }

  // The index of `key`; `fresh` when it was added just now.
  s::uint32_t find(const s::int64_t* key, bool& fresh) {
    if ((count + 1) * 2 > slots.size()) grow();
    s::size_t mask = slots.size() - 1;
    for (s::size_t h = hash(key) & mask;; h = (h + 1) & mask) {
      if (marks[h] != stamp) {
        marks[h] = stamp;
        slots[h] = count;
        keys.insert(keys.end(), key, key + width);
        fresh = true;
        return count++;
      }
      if (s::equal(key, key + width, keys.begin() + static_cast<s::ptrdiff_t>(slots[h] * width))) {
        fresh = false;
        return slots[h];
      }
    }
  }

  const s::int64_t* key(s::uint32_t i) const { return keys.data() + i * width; }

  s::size_t hash(const s::int64_t* key) const {
    s::uint64_t h = 0x9e3779b97f4a7c15u;
    for (s::size_t i = 0; i < width; ++i) {
      h ^= static_cast<s::uint64_t>(key[i]);
      h *= 0xff51afd7ed558ccdu;
      h ^= h >> 32;
    }
    return static_cast<s::size_t>(h);
  }

  void grow() {
    s::size_t n = s::max<s::size_t>(16, slots.size() * 2);
    slots.assign(n, 0);
    marks.assign(n, 0);
    stamp = 1;
    for (s::uint32_t i = 0; i < count; ++i) {
      s::size_t h = hash(key(i)) & (n - 1);
      while (marks[h] == stamp) h = (h + 1) & (n - 1);
      marks[h] = stamp;
      slots[h] = i;
    }
  }

  vec<s::uint32_t> slots, marks;
  vec<s::int64_t>  keys;
  s::size_t        width = 1;
  s::uint32_t      count = 0, stamp = 1;
};

// Starts of a context, as offsets from where the VM run began: inclusive
// ranges, ascending, apart.  Those that pile up after a `*` are one.
struct Span {
  s::uint32_t lo, hi;
};
using Spans = vec<Span>;

// out = a without b.
void spans_minus(const Spans& a, const Spans& b, Spans& out) {
  out.clear();
  s::size_t j = 0;
  for (Span x : a) {
    while (j < b.size() and b[j].hi < x.lo) ++j;
    s::uint32_t lo = x.lo;
    bool rest = true;
    for (s::size_t k = j; k < b.size() and b[k].lo <= x.hi; ++k) {
      if (b[k].lo > lo) out.push_back({lo, b[k].lo - 1});
      if (b[k].hi >= x.hi) {
        rest = false;
        break;
      }
      lo = s::max(lo, b[k].hi + 1);
    }
    if (rest) out.push_back({lo, x.hi});
  }
}

// out = a with b.
void spans_union(const Spans& a, const Spans& b, Spans& out) {
  out.clear();
  s::size_t i = 0, j = 0;
  while (i < a.size() or j < b.size()) {
    Span x = j == b.size() or (i < a.size() and a[i].lo <= b[j].lo) ? a[i++] : b[j++];
    if (not out.empty() and x.lo <= out.back().hi + 1) out.back().hi = s::max(out.back().hi, x.hi);
    else out.push_back(x);
  }
}

bool spans_have(const Spans& a, s::uint32_t q) {
  auto it = s::upper_bound(a.begin(), a.end(), q,
                           [](s::uint32_t v, const Span& x) { return v < x.lo; });
  return it != a.begin() and q <= (it - 1)->hi;
}

// Starts gathered at one position: kept in order when they come in order
// (a start here comes after all others), sorted when asked otherwise.
struct StartList {
  void clear() {
    spans.clear();
    sorted = true;
  }
  void add(const Spans& more) {
    for (Span x : more) {
      if (sorted and (spans.empty() or x.lo > spans.back().hi + 1)) {
        spans.push_back(x);
      } else if (sorted and x.lo >= spans.back().lo) {
        spans.back().hi = s::max(spans.back().hi, x.hi);
      } else {
        spans.push_back(x);
        sorted = false;
      }
    }
  }
  bool has(s::uint32_t q) { return spans_have(in_order(), q); }
  const Spans& in_order() {
    if (not sorted) {
      s::sort(spans.begin(), spans.end(), [](Span a, Span b) { return a.lo < b.lo; });
      s::size_t n = 0;
      for (Span x : spans) {
        if (n > 0 and x.lo <= spans[n - 1].hi + 1) spans[n - 1].hi = s::max(spans[n - 1].hi, x.hi);
        else spans[n++] = x;
      }
      spans.resize(n);
      sorted = true;
    }
    return spans;
  }

  Spans spans;
  bool  sorted = true;
};

// A context's states at one position: (instruction, state registers),
// each with the starts it was reached from.
struct CtxStates {
  void clear(s::size_t width) {
    table.reset(width);
    set_of.clear();
    reading.clear();
    nsets = 0;
  }
  s::uint32_t new_set() {
    if (nsets == sets.size()) sets.emplace_back();
    sets[nsets].clear();
    return static_cast<s::uint32_t>(nsets++);
  }

  KeyTable         table;
  vec<s::uint32_t> set_of;    // per state, its starts' set
  vec<Spans>       sets;
  s::size_t        nsets = 0;
  vec<s::uint32_t> reading;   // the states that read a character
};

struct VmThread {
  s::int32_t pc;
  s::int32_t slot;   // its registers: groups, then state
};

// A thread the closure has yet to follow, with the group registers it
// set at this position.
struct VmPending {
  s::int32_t  pc;
  s::int32_t  slot;
  s::uint32_t saved;
};

// Where a thread a step added came from: the thread of the list read, and
// the group registers set to the new position (bits).
struct VmMade {
  s::uint32_t src;
  s::uint32_t saved;
};

// What VM runs reuse, so that once grown a step allocates nothing.
struct VmScratch {
  struct Ctx {
    // Starts lo..hi go on as `rep` does (Vm::rep()).
    struct Class {
      s::uint32_t lo, hi, rep;
    };

    CtxStates cur, old;
    // The starts it matched from up to here, and those it has a state
    // for here or matched from.
    StartList  acc, alive;
    s::int64_t injected = -1;   // the position it was last started at
    // Its starts' classes here, when there are few states (`few`), and
    // whether the states changed since they were made.
    vec<Class> classes;
    bool       few     = false;
    bool       changed = true;
  };
  // A state a context's closure has yet to follow.
  struct Work {
    s::int32_t  pc;
    s::uint32_t regs;   // in wregs
    s::uint32_t set;    // in tmp
  };

  vec<Ctx>              ctxs;
  vec<s::int64_t>       slab;
  vec<s::int32_t>       free_slots;
  vec<VmThread>         clist, nlist;
  vec<VmPending>        stack;
  vec<VmMade>           made;   // for nlist
  vec<VmPending>        deferred;   // Vm::cut_match()'s
  vec<VmThread>         forced;
  vec<s::uint32_t>      seen;
  s::uint32_t           gen = 0;
  KeyTable              keys;
  vec<s::int64_t>       key, keep;
  vec<Work>             work;
  vec<s::int64_t>       wregs;
  vec<Spans>            tmp;
  s::size_t             ntmp = 0;
  Spans                 merged;
  // Vm::classify()'s.
  vec<const Spans*>                         items;
  vec<s::uint32_t>                          cuts;
  vec<s::pair<s::uint32_t, s::uint32_t>>    reps;
  // The DFA of steady runs (Vm::cacheable()): per state its threads'
  // instructions, dfa_pcs[dfa_start[i], dfa_start[i + 1]), and its steps
  // on each ASCII byte (-1 unknown).
  vec<s::int32_t>                 dfa_pcs;
  vec<s::uint32_t>                dfa_start;
  vec<char>                       dfa_matches;   // per state, a thread matches
  vec<s::int32_t>                 dfa_next;
  vec<s::uint32_t>                dfa_made_at;   // per step, where in dfa_made
  vec<VmMade>                     dfa_made;
  vec<char>                       taken;   // Vm::cached_step()'s
  // The states by their instructions' hash: open addressing, -1 free,
  // four times as many slots as states can be.
  vec<s::int32_t>                 dfa_index;
  bool                  busy = false;
};

// The scratch of this thread, or one of its own for a run inside another.
struct ScratchLease {
  ScratchLease() {
    static thread_local VmScratch shared;
    if (shared.busy) {
      own = s::make_unique<VmScratch>();
      sc  = own.get();
    } else {
      sc = &shared;
    }
    sc->busy = true;
  }
  ~ScratchLease() { sc->busy = false; }
  ScratchLease(const ScratchLease&) = delete;
  ScratchLease& operator=(const ScratchLease&) = delete;

  VmScratch*       sc = nullptr;
  uptr<VmScratch>  own;
};

struct Vm : Subject {
  enum class Mode : s::uint8_t {
    Whole,      // s[b, lim) matches
    First,      // the match zsh's search finds first, the end left open
    Shortest,   // the shortest prefix of s[b, lim) that matches
    Ends,       // every end of a match from b
  };
  // See count_test().
  struct CountChoice {
    bool more, done, empty;
  };
  // What leaked() was asked: the count, its repeats so far, where, and
  // the flags leaked before.
  struct LeakKey {
    const Node* n;
    s::int64_t  c;
    s::size_t   e;
    s::int64_t  leak;
    bool operator==(const LeakKey&) const = default;
  };
  struct LeakHash {
    s::size_t operator()(const LeakKey& k) const {
      s::size_t h = s::hash<const void*>{}(k.n);
      for (auto v : {static_cast<s::size_t>(k.c), k.e, static_cast<s::size_t>(k.leak)})
        h = h * 0x100000001b3u ^ v;
      return h;
    }
  };

  Vm(Subject subject, const CompiledPattern::Impl& impl, VmScratch& scratch, Mode how,
     bool want_caps)
      : Subject{subject}, p{*impl.vm}, impl{impl}, sc{scratch}, mode{how}, want{want_caps},
        nstate{p.regs.size()}, stride{static_cast<s::size_t>(p.caps) + p.regs.size()} {
    if (stride == 0) stride = 1;
  }

  // Runs the program from `entry` at b.  Whether it matched; the match
  // ends at `end`, with the group registers in `caps`.
  bool run(s::size_t b, s::int32_t entry) {
    base = here = b;
    steady  = nstate == 0 and not (entry == p.whole and p.tails);
    cutting = mb and p.ctxs.empty() and (mode == Mode::Shortest or mode == Mode::Ends);
    sc.slab.clear();
    sc.free_slots.clear();
    sc.clist.clear();
    sc.nlist.clear();
    sc.stack.clear();
    sc.deferred.clear();
    sc.key.assign(1 + nstate, 0);
    sc.keep.assign(nstate + 1, 0);
    zeros.assign(nstate + 1, 0);
    if (sc.seen.size() < p.code.size()) sc.seen.resize(p.code.size(), 0);
    if (sc.ctxs.size() < p.ctxs.size()) sc.ctxs.resize(p.ctxs.size());
    for (s::size_t c = 0; c < p.ctxs.size(); ++c) {
      auto& cx = sc.ctxs[c];
      cx.cur.clear(1 + nstate);
      cx.old.clear(1 + nstate);
      cx.injected = -1;
    }
    position();
    s::int32_t slot = alloc();
    s::int64_t* r = regs(slot);
    s::fill(r, r + p.caps, -1);
    s::fill(r + p.caps, r + stride, 0);
    add(sc.clist, entry, slot);
    if (cutting and cut_match() and cut_found()) return true;
    clear_cache();
    // Short runs (most of those of ${x//…}) do not pay for the cache.
    static constexpr int UNCACHED_STEPS = 8;
    for (int steps = 0;; ++steps) {
      if (sc.clist.empty()) break;
      // The threads as a cached state, and the steps cached from it.
      s::int32_t from = -1;
      if (steady and steps >= UNCACHED_STEPS) {
        from = intern(sc.clist);
        // Without the groups, the states alone, the threads made at the end.
        bool bare = not want or p.caps == 0, moved = false;
        while (from >= 0 and here < lim and cacheable(here)) {
          s::size_t k = static_cast<s::size_t>(from) * 128 + static_cast<unsigned char>(s[here]);
          s::int32_t to = sc.dfa_next[k];
          if (to < 0) break;
          if (not bare) {
            if (auto done = cached_step(from, to, sc.dfa_made_at[k])) return *done;
          } else {
            if (not moved) {
              for (const auto& t : sc.clist) release(t.slot);
              sc.clist.clear();
              moved = true;
            }
            if (state_matched(from, here)) return true;
            ++here;
            auto at = static_cast<s::size_t>(to);
            if (sc.dfa_start[at] == sc.dfa_start[at + 1]) return found;
          }
          from = to;
        }
        if (moved) {
          for (s::uint32_t i = sc.dfa_start[static_cast<s::size_t>(from)];
               i < sc.dfa_start[static_cast<s::size_t>(from) + 1]; ++i) {
            s::int32_t fresh = alloc();
            s::fill(regs(fresh), regs(fresh) + stride, 0);
            sc.clist.push_back({sc.dfa_pcs[i], fresh});
          }
        }
        if (sc.clist.empty()) break;
      }
      s::size_t pos = here, next = pos;
      bool more = pos < lim;
      if (more) {
        at(pos, next);
        here = next;
        position();
        // Inner contexts first: the outer ones read what they found.
        for (s::size_t c = p.ctxs.size(); c-- > 0;) step(c, pos);
      }
      sc.nlist.clear();
      sc.made.clear();
      for (s::size_t i = 0; i < sc.clist.size(); ++i) {
        VmThread t = sc.clist[i];
        const Inst& in = p.code[static_cast<s::size_t>(t.pc)];
        if (in.op == Op::Match or in.op == Op::Tail) {
          if (matched(t.slot, pos)) return true;
          release(t.slot);
          if (mode == Mode::Ends) continue;
          // The threads after it come after it in zsh's search.
          for (s::size_t j = i + 1; j < sc.clist.size(); ++j) release(sc.clist[j].slot);
          break;
        }
        s::int32_t target = 0;
        if (p.leak >= 0) set_leak(state(t.slot)[p.leak]);
        if (more and consume(in, pos, target))
          add(sc.nlist, target, t.slot, static_cast<s::uint32_t>(i));
        else
          release(t.slot);
      }
      if (not more) break;
      if (cutting and cut_match() and cut_found()) return true;
      // A step that depended on nothing but the threads' instructions and
      // the byte read is cached, with where each thread came from.
      if (from >= 0 and cacheable(pos)) {
        s::int32_t to = intern(sc.nlist);
        if (to >= 0) {
          s::size_t k = static_cast<s::size_t>(from) * 128 + static_cast<unsigned char>(s[pos]);
          sc.dfa_next[k]    = to;
          sc.dfa_made_at[k] = static_cast<s::uint32_t>(sc.dfa_made.size());
          sc.dfa_made.insert(sc.dfa_made.end(), sc.made.begin(), sc.made.end());
        }
      }
      s::swap(sc.clist, sc.nlist);
    }
    return found;
  }

  // zsh tries the lengths of a shortest match on the text cut there, whose
  // end counts as a place where a character may start; the run reads on,
  // where a byte that continues no character follows and `*` may not end.
  // So for those places the threads a Boundary stopped are followed again
  // with it open, reading nothing: whether that matches here.  (With `^`
  // or `~`, whose programs a cut text changes too, vm_prefix() tries the
  // cut text instead.)
  bool cut_match() {
    if (sc.deferred.empty()) return false;
    forcing = true;
    position();
    s::size_t made = sc.made.size();
    sc.forced.clear();
    for (const auto& d : sc.deferred) add(sc.forced, d.pc, d.slot);
    sc.deferred.clear();
    sc.made.resize(made);
    forcing = false;
    bool hit = false;
    for (const auto& t : sc.forced) {
      hit = hit or p.code[static_cast<s::size_t>(t.pc)].op == Op::Match;
      release(t.slot);
    }
    return hit;
  }

  // A cut text matched here: notes it as a match, and whether the run is
  // over.
  bool cut_found() { return matched(-1, here); }

  // The thread at `slot` matched at pos: notes it, and whether the run is
  // over (a shortest match, or any whole one when its groups are not
  // wanted).
  bool matched(s::int32_t slot, s::size_t pos) {
    found = true;
    if (mode == Mode::Ends) {
      if (ends.empty() or ends.back() != pos) ends.push_back(pos);
      return false;
    }
    end = pos;
    // A match a cut text or a cached state found has no thread (-1);
    // those come only where the groups are not wanted.
    assert(slot >= 0 or not want);
    if (want and slot >= 0) caps.assign(regs(slot), regs(slot) + p.caps);
    return mode == Mode::Shortest or (mode == Mode::Whole and not want);
  }

  // Steady runs (no state registers, no TailLiteral) depend on the
  // threads' instructions alone, but for the group registers, which only
  // ever take the position: their lists are the states of a DFA built as
  // the run goes, whose steps on ASCII bytes are cached where the position
  // does not matter (not at lim, and where a character starts after the
  // byte), each with where its threads come from and which group
  // registers it sets.
  bool cacheable(s::size_t pos) const {
    return static_cast<unsigned char>(s[pos]) < 0x80 and pos + 1 < lim and boundary(pos + 1);
  }

  void clear_cache() {
    if (sc.dfa_start.empty()) return;
    sc.dfa_pcs.clear();
    sc.dfa_start.clear();
    sc.dfa_matches.clear();
    sc.dfa_next.clear();
    sc.dfa_made_at.clear();
    sc.dfa_made.clear();
    s::fill(sc.dfa_index.begin(), sc.dfa_index.end(), -1);
  }

  // The cached state of the threads in `list`, -1 when there are too many.
  s::int32_t intern(const vec<VmThread>& list) {
    s::uint64_t h = 0x9e3779b97f4a7c15u;
    for (const auto& t : list) h = (h ^ static_cast<s::uint64_t>(t.pc)) * 0xff51afd7ed558ccdu;
    auto same = [&](s::size_t id) {
      s::size_t b = sc.dfa_start[id], e = sc.dfa_start[id + 1];
      if (e - b != list.size()) return false;
      for (s::size_t i = 0; i < list.size(); ++i)
        if (sc.dfa_pcs[b + i] != list[i].pc) return false;
      return true;
    };
    static constexpr s::size_t MAX_STATES = 256;
    static constexpr s::size_t SLOTS      = 1024;
    if (sc.dfa_index.size() != SLOTS) sc.dfa_index.assign(SLOTS, -1);
    s::size_t at = static_cast<s::size_t>(h) & (SLOTS - 1);
    for (; sc.dfa_index[at] >= 0; at = (at + 1) & (SLOTS - 1))
      if (same(static_cast<s::size_t>(sc.dfa_index[at]))) return sc.dfa_index[at];
    if (sc.dfa_start.size() > MAX_STATES) return -1;
    if (sc.dfa_start.empty()) sc.dfa_start.push_back(0);
    auto id = static_cast<s::int32_t>(sc.dfa_start.size() - 1);
    bool any = false;
    for (const auto& t : list) {
      sc.dfa_pcs.push_back(t.pc);
      Op op = p.code[static_cast<s::size_t>(t.pc)].op;
      any = any or op == Op::Match or op == Op::Tail;
    }
    sc.dfa_matches.push_back(any);
    sc.dfa_start.push_back(static_cast<s::uint32_t>(sc.dfa_pcs.size()));
    sc.dfa_next.resize(sc.dfa_next.size() + 128, -1);
    sc.dfa_made_at.resize(sc.dfa_made_at.size() + 128, 0);
    sc.dfa_index[at] = id;
    return id;
  }

  // The matches of cached state `id` at pos, as a step there notes them
  // when the groups are not wanted; whether the run is over.
  bool state_matched(s::int32_t id, s::size_t pos) {
    if (not sc.dfa_matches[static_cast<s::size_t>(id)]) return false;
    for (s::uint32_t i = sc.dfa_start[static_cast<s::size_t>(id)];
         i < sc.dfa_start[static_cast<s::size_t>(id) + 1]; ++i) {
      Op op = p.code[static_cast<s::size_t>(sc.dfa_pcs[i])].op;
      if (op != Op::Match and op != Op::Tail) continue;
      if (matched(-1, pos)) return true;
      if (mode != Mode::Ends) break;
    }
    return false;
  }

  // The cached step from the threads in clist, at here, to state `to`,
  // whose threads came as dfa_made[made, ...) says.  A run that ends
  // there, or a shortest or whole match found, gives its answer.
  opt<bool> cached_step(s::int32_t from_state, s::int32_t to, s::uint32_t made) {
    s::size_t pos = here++;
    // The step finds the first match; those after it it leaves out.
    if (sc.dfa_matches[static_cast<s::size_t>(from_state)])
      for (const auto& t : sc.clist) {
        Op op = p.code[static_cast<s::size_t>(t.pc)].op;
        if (op != Op::Match and op != Op::Tail) continue;
        if (matched(t.slot, pos)) return true;
        if (mode != Mode::Ends) break;
      }
    s::uint32_t b = sc.dfa_start[static_cast<s::size_t>(to)];
    s::uint32_t e = sc.dfa_start[static_cast<s::size_t>(to) + 1];
    sc.nlist.clear();
    // A thread's registers go to the first thread made from it, copies to
    // the others.
    if (sc.taken.size() < sc.clist.size()) sc.taken.resize(sc.clist.size());
    s::fill_n(sc.taken.begin(), sc.clist.size(), 0);
    for (s::uint32_t i = b; i < e; ++i) {
      const VmMade& m = sc.dfa_made[made + (i - b)];
      s::int32_t slot = sc.clist[m.src].slot;
      if (sc.taken[m.src]) slot = clone(slot);
      sc.taken[m.src] = 1;
      sc.nlist.push_back({sc.dfa_pcs[i], slot});
    }
    // Then the group registers set here, the copies made.
    for (s::uint32_t i = b; i < e; ++i) {
      s::int64_t* r = regs(sc.nlist[i - b].slot);
      for (s::uint32_t bits = sc.dfa_made[made + (i - b)].saved; bits != 0; bits &= bits - 1)
        r[s::countr_zero(bits)] = static_cast<s::int64_t>(here);
    }
    for (s::size_t i = 0; i < sc.clist.size(); ++i)
      if (not sc.taken[i]) release(sc.clist[i].slot);
    s::swap(sc.clist, sc.nlist);
    if (sc.clist.empty()) return found;
    return s::nullopt;
  }

  s::int64_t* regs(s::int32_t slot) {
    return sc.slab.data() + static_cast<s::size_t>(slot) * stride;
  }
  // The state registers, after the group ones.
  s::int64_t* state(s::int32_t slot) { return regs(slot) + p.caps; }

  s::int32_t alloc() {
    if (not sc.free_slots.empty()) {
      s::int32_t slot = sc.free_slots.back();
      sc.free_slots.pop_back();
      return slot;
    }
    sc.slab.resize(sc.slab.size() + stride);
    return static_cast<s::int32_t>(sc.slab.size() / stride - 1);
  }
  s::int32_t clone(s::int32_t slot) {
    s::int32_t copy = alloc();
    // A few registers: a loop, not a call.
    const s::int64_t* from = regs(slot);
    s::int64_t* to = regs(copy);
    for (s::size_t k = 0; k < stride; ++k) to[k] = from[k];
    return copy;
  }
  void release(s::int32_t slot) { sc.free_slots.push_back(slot); }

  // A new position: nothing visited there yet.
  void position() {
    if (++sc.gen == 0) {
      s::fill(sc.seen.begin(), sc.seen.end(), 0);
      sc.gen = 1;
    }
    sc.keys.reset(1 + nstate);
    for (s::size_t c = 0; c < p.ctxs.size(); ++c) {
      sc.ctxs[c].acc.clear();
      sc.ctxs[c].alive.clear();
      sc.ctxs[c].changed = true;
    }
  }

  // Whether context c started at `from` matched up to here, and whether
  // it still may.
  bool accepted(s::int32_t c, s::int64_t from) {
    return from >= 0 and sc.ctxs[static_cast<s::size_t>(c)].acc.has(start(from));
  }
  bool alive(s::int32_t c, s::int64_t from) {
    return from >= 0 and sc.ctxs[static_cast<s::size_t>(c)].alive.has(start(from));
  }

  // Context c's start `from` as the first of the starts that go on alike
  // from here: in the same states, and matching up to here or not alike
  // (so `*(^*b)c` keeps one thread in the `^`, not one per start); -1 when
  // it can no longer match.  With many states, `from` itself.
  s::int64_t rep(s::int32_t c, s::int64_t from) {
    if (from < 0) return -1;
    auto& cx = sc.ctxs[static_cast<s::size_t>(c)];
    if (cx.changed) classify(cx);
    if (not cx.few) return alive(c, from) ? from : -1;
    s::uint32_t q = start(from);
    auto it = s::upper_bound(cx.classes.begin(), cx.classes.end(), q,
                             [](s::uint32_t v, const auto& k) { return v < k.lo; });
    if (it == cx.classes.begin() or q > (it - 1)->hi) return -1;
    return static_cast<s::int64_t>(base) + (it - 1)->rep;
  }

  // Splits context cx's starts into the classes rep() gives: ranges
  // between the ends of its states' starts, by the states they are in.
  void classify(VmScratch::Ctx& cx) {
    static constexpr s::size_t MAX_ITEMS = 16;
    static constexpr s::size_t MAX_CUTS  = 128;
    cx.changed = false;
    cx.few     = false;
    cx.classes.clear();
    auto& items = sc.items;
    items.clear();
    for (s::uint32_t st : cx.cur.reading) items.push_back(&cx.cur.sets[cx.cur.set_of[st]]);
    items.push_back(&cx.acc.in_order());
    if (items.size() > MAX_ITEMS) return;
    auto& cuts = sc.cuts;
    cuts.clear();
    for (const Spans* set : items)
      for (Span x : *set) {
        cuts.push_back(x.lo);
        cuts.push_back(x.hi + 1);
      }
    if (cuts.size() > MAX_CUTS) return;
    s::sort(cuts.begin(), cuts.end());
    cuts.erase(s::unique(cuts.begin(), cuts.end()), cuts.end());
    auto& reps = sc.reps;
    reps.clear();
    for (s::size_t k = 0; k + 1 < cuts.size(); ++k) {
      s::uint32_t in = 0;
      for (s::size_t i = 0; i < items.size(); ++i)
        if (spans_have(*items[i], cuts[k])) in |= s::uint32_t{1} << i;
      if (in == 0) continue;
      auto r = s::find_if(reps.begin(), reps.end(), [&](const auto& x) { return x.first == in; });
      if (r == reps.end()) r = reps.insert(reps.end(), {in, cuts[k]});
      cx.classes.push_back({cuts[k], cuts[k + 1] - 1, r->second});
    }
    cx.few = true;
  }
  // A context's start at position `from`, as its runs keep it.
  s::uint32_t start(s::int64_t from) const {
    return static_cast<s::uint32_t>(from - static_cast<s::int64_t>(base));
  }

  // State registers `r` at `in` in their canonical form, so that states
  // that go on alike compare equal: those not live there are cleared, a
  // count beyond what matters is clamped, where a repeat began and a
  // context started are forgotten when it no longer matters.
  void canon(const Inst& in, s::int64_t* r) {
    s::int64_t* keep = sc.keep.data();
    for (s::uint32_t i = 0; i < in.nlive; ++i) keep[i] = r[p.live[in.live + i]];
    s::fill(r, r + nstate, 0);
    for (s::uint32_t i = 0; i < in.nlive; ++i) {
      auto reg = static_cast<s::size_t>(p.live[in.live + i]);
      const auto& ri = p.regs[reg];
      s::int64_t v = keep[i];
      switch (ri.kind) {
        case VmProgram::RegKind::Count:
          v = count_class(ri.min, ri.max, v, here);

        break; case VmProgram::RegKind::Iter:
          if (v != static_cast<s::int64_t>(here)) v = -1;

        break; case VmProgram::RegKind::Ctx:
          v = rep(ri.ctx, v);

        break; case VmProgram::RegKind::Leak:

        break; default:
          assert(false);  // should never get here
      }
      r[reg] = v;
    }
  }

  // Count v of a (#cN,M) at position `at`, as one of the counts that go on
  // alike: past N, one that can no longer reach M before the end (an
  // empty repeat counts too, and ends the count) is as good as any.
  s::int64_t count_class(int min, int max, s::int64_t v, s::size_t at) const {
    s::int64_t enough = s::max(min, 1);
    auto room = static_cast<s::int64_t>(lim - at) + 1;
    return v >= enough and (max < 0 or max - v > room) ? enough : v;
  }

  // Whether a thread at pc, with state registers r, is the first there.
  bool visit(s::int32_t pc, const Inst& in, s::int64_t* r) {
    if (nstate > 0) canon(in, r);
    if (in.nlive == 0) {
      auto& seen = sc.seen[static_cast<s::size_t>(pc)];
      if (seen == sc.gen) return false;
      seen = sc.gen;
      return true;
    }
    sc.key[0] = pc;
    s::copy_n(r, nstate, sc.key.data() + 1);
    bool fresh = false;
    sc.keys.find(sc.key.data(), fresh);
    return fresh;
  }

  // Whether `in` reads the character at pos (< lim), and where it goes.
  bool consume(const Inst& in, s::size_t pos, s::int32_t& target) const {
    switch (in.op) {
      case Op::Char: {
        s::size_t next = 0;
        if (not one(*in.node, pos, next)) return false;
        target = in.x;
        return true;
      }
      case Op::AnyChar:
        target = in.x;
        return true;
      case Op::Digit: {
        auto c = static_cast<unsigned char>(s[pos]);
        if (not s::isdigit(c)) return false;
        const auto& rg = p.ranges[static_cast<s::size_t>(in.x)];
        s::int32_t st = rg.next[static_cast<s::size_t>(in.y)][c - '0'];
        if (st < 0) return false;
        target = rg.pc0 + 2 * st;
        return true;
      }
      default:
        return false;
    }
  }

  // Adds the thread at pc with registers `slot` to `list`, here: follows
  // what reads no character, in zsh's order, and adds the threads that
  // read one or match.
  // The threads added go with (in sc.made) the thread of the list read
  // that made them (`src`) and the group registers set here.
  void add(vec<VmThread>& list, s::int32_t pc0, s::int32_t slot0, s::uint32_t src = 0) {
    auto& st = sc.stack;
    st.push_back({pc0, slot0, 0});
    while (not st.empty()) {
      auto [pc, slot, saved] = st.back();
      st.pop_back();
      for (bool go = true; go;) {
        const Inst& in = p.code[static_cast<s::size_t>(pc)];
        if (not visit(pc, in, state(slot))) {
          // As zsh's marks: a repeat tried from here already ends.
          if (in.op == Op::Loop) {
            pc = in.y;
            continue;
          }
          release(slot);
          break;
        }
        switch (in.op) {
          case Op::Char: case Op::AnyChar: case Op::Digit:
            keep(list, pc, slot, src, saved);
            go = false;

          break; case Op::Match:
            if (mode == Mode::Whole and here != lim) release(slot);
            else keep(list, pc, slot, src, saved);
            go = false;

          break; case Op::Tail:
            if (p.leak >= 0) set_leak(state(slot)[p.leak]);
            if (tail_matches(*impl.tails[static_cast<s::size_t>(in.x)], here))
              keep(list, pc, slot, src, saved);
            else
              release(slot);
            go = false;

          break; case Op::Jmp:
            pc = in.x;

          break; case Op::Split:
            st.push_back({in.y, clone(slot), saved});
            pc = in.x;

          break; case Op::Loop:
            st.push_back({in.y, clone(slot), saved});
            if (in.reg >= 0) state(slot)[in.reg] = static_cast<s::int64_t>(here);
            pc = in.x;

          break; case Op::Boundary:
            if (cutting and not forcing and not boundary(here)) {
              // A text cut here may yet match (see cut_match()).
              sc.deferred.push_back({pc + 1, slot, saved});
              go = false;
            } else {
              go = pass(forcing or boundary(here), slot);
              ++pc;
            }

          break; case Op::Save:
            regs(slot)[in.x] = static_cast<s::int64_t>(here);
            saved |= s::uint32_t{1} << in.x;
            ++pc;

          break; case Op::AtStart:
            go = pass(here == 0 and not notstart, slot);
            ++pc;

          break; case Op::AtEnd:
            go = pass(here == s.size() and not notend, slot);
            ++pc;

          break; case Op::CountInit: {
            s::int64_t* r = state(slot);
            r[in.reg]     = 0;
            r[in.reg + 1] = -1;
            ++pc;
          }

          break; case Op::CountTest: {
            auto [more, done, empty] = count_test(in, state(slot));
            if (empty) {
              pc = in.y;
            } else if (more) {
              if (done) {
                s::int32_t exit = clone(slot);
                if (p.leak >= 0 and not in.node->leaks.empty()) leak_past(in, state(exit));
                st.push_back({in.y, exit, saved});
              }
              state(slot)[in.reg + 1] = static_cast<s::int64_t>(here);
              pc = in.x;
            } else if (done) {
              pc = in.y;
            } else {
              release(slot);
              go = false;
            }
          }

          break; case Op::CountInc: {
            s::int64_t& n = state(slot)[in.reg];
            n = s::min<s::int64_t>(n + 1, INT32_MAX);
            pc = in.x;
          }

          break; case Op::CtxStart:
            inject(in.x);
            state(slot)[in.reg] = static_cast<s::int64_t>(here);
            ++pc;

          break; case Op::NegExit:
            go = pass(boundary(here) and not accepted(in.x, state(slot)[in.reg]), slot);
            ++pc;

          break; case Op::ExclCheck:
            go = pass(not accepted(in.x, state(slot)[in.reg]), slot);
            ++pc;

          break; default:
            assert(false);  // should never get here
        }
      }
    }
  }

  // What follows (#cN,M) `in` here, after its count in r, should another
  // repeat fail: zsh's counter leaves on the case flags that repeat
  // changed last (Node::leaks), in r's Leak register.
  void leak_past(const Inst& in, s::int64_t* r) {
    const Node& n = *in.node;
    if (const auto* fp = leaked(n, r[in.reg], here, r[p.leak]))
      r[p.leak] = 1 + 4 * s::int64_t{n.leak_epoch} + (fp->nocase ? 1 : 0) + (fp->lower ? 2 : 0);
  }

  // The change of the flags the repeats of n from e, after c, pass last
  // before they fail, with the flags leaked so far `leak` (zsh's `last` in
  // Matcher::counted()).  The operand matches in one way (calls_once()),
  // so the repeats are one chain, walked once ahead with the backtracker
  // and remembered for each repeat on it.
  const Node::FlagPoint* leaked(const Node& n, s::int64_t c, s::size_t e, s::int64_t leak) {
    struct Step {
      s::int64_t             c;
      s::size_t              e;
      const Node::FlagPoint* notes;   // the last change its own path passed
    };
    vec<Step> chain;
    const Node::FlagPoint* last = nullptr;
    Subject sub{*this};
    sub.set_leak(leak);
    for (;;) {
      if (auto it = leak_memo.find(LeakKey{&n, c, e, leak}); it != leak_memo.end()) {
        last = it->second;
        break;
      }
      // At the maximum no repeat is tried, nor changes watched.
      if (n.max >= 0 and c >= n.max) break;
      Matcher m{sub, impl};
      m.watch  = &n;
      m.passed = nullptr;
      s::size_t after = strv::npos;
      bool ok = m.node(*n.sub, e, [&](s::size_t x) {
        after = x;
        return true;
      });
      if (not ok) {
        last = m.passed;
        leak_memo[LeakKey{&n, c, e, leak}] = last;
        break;
      }
      chain.push_back({c, e, m.passed});
      s::int64_t next = count_class(n.min, n.max, s::min<s::int64_t>(c + 1, INT32_MAX), after);
      // A repeat that matched nothing ends the count, watching nothing.
      if (next >= n.min and after == e) break;
      c = next;
      e = after;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      if (not last) last = it->notes;
      leak_memo[LeakKey{&n, it->c, it->e, leak}] = last;
    }
    return last;
  }

  void keep(vec<VmThread>& list, s::int32_t pc, s::int32_t slot, s::uint32_t src,
            s::uint32_t saved) {
    list.push_back({pc, slot});
    sc.made.push_back({src, saved});
  }

  // Whether the thread at `slot` goes on (`ok`); else it ends.
  bool pass(bool ok, s::int32_t slot) {
    if (not ok) release(slot);
    return ok;
  }

  // zsh's P_COUNT after the count in r: whether another repeat is tried,
  // whether what follows may come, and whether the last repeat matched
  // nothing, which ends the count.
  CountChoice count_test(const Inst& in, const s::int64_t* r) const {
    const Node& n = *in.node;
    s::int64_t cur = r[in.reg];
    bool done = cur >= n.min;
    if (cur > 0 and done and r[in.reg + 1] == static_cast<s::int64_t>(here))
      return {false, true, true};
    return {n.max < 0 or cur < n.max, done, false};
  }

  // Context c's run from pos to here: its states that read the character
  // at pos.
  void step(s::size_t c, s::size_t pos) {
    auto& cx = sc.ctxs[c];
    s::swap(cx.cur, cx.old);
    cx.cur.clear(1 + nstate);
    for (s::uint32_t st : cx.old.reading) {
      const s::int64_t* key = cx.old.table.key(st);
      s::int32_t target = 0;
      if (not consume(p.code[static_cast<s::size_t>(key[0])], pos, target)) continue;
      s::size_t mark = sc.ntmp;
      s::uint32_t set = new_tmp();
      sc.tmp[set] = cx.old.sets[cx.old.set_of[st]];
      sub_add(c, target, key + 1, set);
      sc.ntmp = mark;
    }
  }

  // Starts context c here.
  void inject(s::int32_t c) {
    auto& cx = sc.ctxs[static_cast<s::size_t>(c)];
    if (cx.injected == static_cast<s::int64_t>(here)) return;
    cx.injected = static_cast<s::int64_t>(here);
    s::size_t mark = sc.ntmp;
    s::uint32_t set = new_tmp();
    s::uint32_t q = start(static_cast<s::int64_t>(here));
    sc.tmp[set].push_back({q, q});
    sub_add(static_cast<s::size_t>(c), p.ctxs[static_cast<s::size_t>(c)].entry, zeros.data(), set);
    sc.ntmp = mark;
  }

  s::uint32_t new_tmp() {
    if (sc.ntmp == sc.tmp.size()) sc.tmp.emplace_back();
    sc.tmp[sc.ntmp].clear();
    return static_cast<s::uint32_t>(sc.ntmp++);
  }

  // Adds to context c's states here those its program reaches from pc,
  // with state registers r, for the starts in tmp[set].
  void sub_add(s::size_t c, s::int32_t pc0, const s::int64_t* r0, s::uint32_t set0) {
    s::size_t wbase = sc.work.size(), rbase = sc.wregs.size(), tbase = sc.ntmp;
    // A state to follow, with a copy of the registers at wregs[from].
    auto push = [&](s::int32_t pc, s::uint32_t from, s::uint32_t set) {
      auto ro = static_cast<s::uint32_t>(sc.wregs.size());
      sc.wregs.resize(ro + nstate);
      s::copy_n(sc.wregs.data() + from, nstate, sc.wregs.data() + ro);
      sc.work.push_back({pc, ro, set});
    };
    auto ro = static_cast<s::uint32_t>(sc.wregs.size());
    sc.wregs.insert(sc.wregs.end(), r0, r0 + nstate);
    sc.work.push_back({pc0, ro, set0});
    while (sc.work.size() > wbase) {
      auto w = sc.work.back();
      sc.work.pop_back();
      s::int32_t  pc  = w.pc;
      s::uint32_t set = w.set;
      auto work_regs = [&] { return sc.wregs.data() + w.regs; };
      for (bool go = true; go;) {
        const Inst& in = p.code[static_cast<s::size_t>(pc)];
        if (nstate > 0) canon(in, work_regs());
        sc.key[0] = pc;
        s::copy_n(work_regs(), nstate, sc.key.data() + 1);
        auto& states = sc.ctxs[c].cur;
        bool fresh = false;
        s::uint32_t st = states.table.find(sc.key.data(), fresh);
        if (fresh) states.set_of.push_back(states.new_set());
        // The starts that get here for the first time.
        s::uint32_t d = new_tmp();
        {
          auto& have = states.sets[states.set_of[st]];
          auto& out = sc.tmp[d];
          spans_minus(sc.tmp[set], have, out);
          if (out.empty()) break;
          spans_union(have, out, sc.merged);
          s::swap(have, sc.merged);
        }
        set = d;
        switch (in.op) {
          case Op::Char: case Op::AnyChar: case Op::Digit:
            if (fresh) states.reading.push_back(st);
            mark(c, set, false);
            go = false;

          break; case Op::Match:
            mark(c, set, true);
            go = false;

          break; case Op::Tail:
            go = false;

          break; case Op::Jmp:
            pc = in.x;

          break; case Op::Split: case Op::Loop:
            push(in.y, w.regs, set);
            if (in.op == Op::Loop and in.reg >= 0)
              work_regs()[in.reg] = static_cast<s::int64_t>(here);
            pc = in.x;

          break; case Op::Boundary:
            go = boundary(here);
            ++pc;

          break; case Op::Save:
            ++pc;

          break; case Op::AtStart:
            go = here == 0 and not notstart;
            ++pc;

          break; case Op::AtEnd:
            go = here == s.size() and not notend;
            ++pc;

          break; case Op::CountInit:
            work_regs()[in.reg]     = 0;
            work_regs()[in.reg + 1] = -1;
            ++pc;

          break; case Op::CountTest: {
            auto [more, done, empty] = count_test(in, work_regs());
            if (empty) {
              pc = in.y;
            } else if (more) {
              if (done) push(in.y, w.regs, set);
              work_regs()[in.reg + 1] = static_cast<s::int64_t>(here);
              pc = in.x;
            } else if (done) {
              pc = in.y;
            } else {
              go = false;
            }
          }

          break; case Op::CountInc:
            work_regs()[in.reg] = s::min<s::int64_t>(work_regs()[in.reg] + 1, INT32_MAX);
            pc = in.x;

          break; case Op::CtxStart:
            inject(in.x);
            work_regs()[in.reg] = static_cast<s::int64_t>(here);
            ++pc;

          break; case Op::NegExit:
            go = boundary(here) and not accepted(in.x, work_regs()[in.reg]);
            ++pc;

          break; case Op::ExclCheck:
            go = not accepted(in.x, work_regs()[in.reg]);
            ++pc;

          break; default:
            assert(false);  // should never get here
        }
      }
    }
    sc.wregs.resize(rbase);
    sc.ntmp = tbase;
  }

  // The starts in tmp[set] have a state here in context c, or (`acc`)
  // match up to here.
  void mark(s::size_t c, s::uint32_t set, bool acc) {
    auto& cx = sc.ctxs[c];
    cx.alive.add(sc.tmp[set]);
    if (acc) cx.acc.add(sc.tmp[set]);
    cx.changed = true;
  }

  const VmProgram&                 p;
  const CompiledPattern::Impl&     impl;
  VmScratch&                       sc;
  Mode                             mode;
  bool                             want;    // the groups are wanted
  s::size_t                        nstate;  // state registers
  s::size_t                        stride;  // registers per thread
  s::size_t                        base = 0, here = 0;
  bool                             found = false;
  s::size_t                        end   = 0;
  vec<s::int64_t>                  caps;
  vec<s::size_t>                   ends;
  vec<s::int64_t>                  zeros;   // state registers all clear
  bool                             steady  = false;   // see cacheable()
  bool                             cutting = false;   // see cut_match()
  bool                             forcing = false;
  umap<LeakKey, const Node::FlagPoint*, LeakHash> leak_memo;   // leaked()'s answers
};

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

// The ASCII characters, compared exactly, that every match of n has: a
// bit each.
using CharSet = s::array<s::uint64_t, 2>;

CharSet needed(const Node& n) {
  CharSet out{};
  auto seq = [](const Seq& q) {
    CharSet all{};
    for (const auto& x : q) {
      CharSet c = needed(x);
      all[0] |= c[0];
      all[1] |= c[1];
    }
    return all;
  };
  switch (n.kind) {
    case Kind::Lit:
      if (not n.bad and n.ch < 0x80 and not n.nocase and not n.lower)
        out[n.ch >> 6] |= s::uint64_t{1} << (n.ch & 63);
      return out;
    case Kind::Repeat:
      return n.min > 0 ? needed(*n.sub) : out;
    case Kind::Group:
      if (n.negate) return out;
      out = {~s::uint64_t{0}, ~s::uint64_t{0}};
      for (const auto& alt : n.alts) {
        CharSet c = seq(alt);
        out[0] &= c[0];
        out[1] &= c[1];
      }
      return out;
    default:
      return out;
  }
}

// Compiles `impl`, parsed, for the VM.
void compile_vm(CompiledPattern::Impl& impl) {
  // (#aN) has only the backtracker.
  if (impl.approx) return;
  const Node& top = impl.main.front();
  impl.stale     = has_stale_count(top, impl.multibyte);
  impl.marks     = nested_repeats(top, false);
  impl.vm_leaks  = impl.leaky and vm_follows_leaks(top, false);
  auto prog = s::make_shared<VmProgram>();
  VmCompiler{*prog, {}, {}, {}}.top(impl, impl.vm_leaks);
  // Leaked flags may fold a literal's case.
  if (not impl.leaky) {
    CharSet c = needed(top);
    for (int ch = 0; ch < 128; ++ch)
      if ((c[static_cast<s::size_t>(ch >> 6)] >> (ch & 63)) & 1)
        prog->needs.push_back(static_cast<char>(ch));
  }
  impl.vm = s::move(prog);
}

}  // namespace

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
    compile_vm(*lit);
    cp.mImpl = s::move(lit);
    return cp;
  }
  for (s::size_t a = 0; a < top.alts.size(); ++a)
    impl->tails.push_back(top.alt_excludes[a].empty() ? tail_literal(top.alts[a], multibyte)
                                                      : s::nullopt);
  impl->main.push_back(s::move(top));
  compile_vm(*impl);
  cp.mImpl = s::move(impl);
  return cp;
}

// Without (#aN) the VM decides whether, and how far, a pattern matches,
// and finds its (#b) groups, except where zsh's counter keeps a stale
// start, its marks of a repeat inside another matter, or its flags leak
// where the VM does not follow (Impl::stale, Impl::marks, Impl::leaky;
// see backtracked()): there the backtracker decides, the VM filtering
// where it can and answering when the backtracker gives up.  With (#aN)
// the backtracker decides.
namespace {

// The ends of matches from a place, ascending.
using Positions = vec<s::size_t>;

// What the backtracking matcher found.
enum class Verdict : s::uint8_t { No, Yes, GaveUp };

// The backtracking matcher on text[b, e), and the (#b) groups of a match
// into `info`.  With `tails`, as a whole match; else as a prefix match
// that ends at e.
// When it gives up, `spent` says whether on steps (else on the stack).
Verdict backtrack(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
                  PatternMatch* info, bool tails = true, bool notstart = false,
                  bool notend = false, bool* spent = nullptr) {
  Matcher m{{text, e, impl.multibyte, notstart, notend}, impl};
  bool ok = m.whole(impl, b, tails);
  if (spent) *spent = m.spent;
  // Given up inside an exclusion's test, it may have found a match that
  // is none.
  if (m.deep) return Verdict::GaveUp;
  if (not ok) return Verdict::No;
  if (info) m.report(b, *info);
  return Verdict::Yes;
}

// The backtracking matcher with the end left open, on the text cut at
// `lim`: the end zsh's search reaches first, npos when there is none, and
// nothing when it gave up (`spent` when on steps).
opt<s::size_t> backtrack_open(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                              PatternMatch* info, const PrefixSearch& how, bool& spent,
                              s::size_t lim = strv::npos) {
  Matcher m{{text, s::min(lim, text.size()), impl.multibyte, how.notstart, how.notend}, impl};
  s::size_t found = m.open(impl, b);
  spent = m.spent;
  if (m.deep) return s::nullopt;
  if (found != strv::npos and info) {
    info->groups.clear();
    if (impl.captures > 0) m.report(b, *info);
  }
  return found;
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

void no_groups(const CompiledPattern::Impl& impl, PatternMatch* info) {
  if (info) info->groups.assign(static_cast<s::size_t>(impl.captures), {strv::npos, strv::npos});
}

// A VM run from b on `subject`; `tails` takes the whole-match forms.  The
// groups of a match go to `info`, every end to `ends`, the position where
// the run stopped to `reached`.
opt<s::size_t> vm_run(const CompiledPattern::Impl& impl, Subject subject, Vm::Mode mode,
                      s::size_t b, bool tails, PatternMatch* info, Positions* ends = nullptr,
                      s::size_t* reached = nullptr) {
  if (ends) ends->clear();
  if (reached) *reached = b;
  strv rest = subject.s.substr(b, subject.lim - b);
  for (char c : impl.vm->needs)
    if (rest.find(c) == strv::npos) return s::nullopt;
  ScratchLease lease;
  Vm vm{subject, impl, *lease.sc, mode, info and impl.captures > 0};
  bool found = vm.run(b, tails ? impl.vm->whole : impl.vm->prefix);
  if (ends) *ends = s::move(vm.ends);
  if (reached) *reached = vm.here;
  if (not found) return s::nullopt;
  if (info) {
    info->groups.clear();
    for (int g = 0; g < impl.captures; ++g) {
      s::int64_t gb = vm.caps[static_cast<s::size_t>(2 * g)];
      s::int64_t ge = vm.caps[static_cast<s::size_t>(2 * g + 1)];
      if (gb < 0 or ge < 0) info->groups.emplace_back(strv::npos, strv::npos);
      else info->groups.emplace_back(static_cast<s::size_t>(gb) - b, static_cast<s::size_t>(ge) - b);
    }
  }
  return vm.end;
}

// The VM: whether text[b, e) matches (with `tails`, as a whole match).
bool vm_matches(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
                PatternMatch* info, bool tails = true, bool notstart = false, bool notend = false) {
  Subject subject{text, e, impl.multibyte, notstart, notend};
  return vm_run(impl, subject, Vm::Mode::Whole, b, tails, info).has_value();
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

// zsh tries each length of a prefix as a match of the text cut there,
// whose end is a place where a character may start; the VM reads on.
// They differ only at the places, up to `until`, before a byte that
// continues no character, where `*` may end in the cut text, and so may
// an exclusion: those places.  Without `^` and `~` the VM's run decides
// them itself (Vm::cut_match()); with them, a match of each cut text,
// one run per place.
Positions cut_ends(const CompiledPattern::Impl& impl, const Subject& subject, s::size_t b,
                   s::size_t until) {
  Positions cut;
  if (not impl.multibyte) return cut;
  for (s::size_t e = b; e < until and e < subject.s.size();) {
    if (not subject.boundary(e)) cut.push_back(e);
    subject.at(e, e);
  }
  return cut;
}

// The VM: every end of a match from b, and (with `^` or `~`) the places
// of cut_ends() where a cut text may match.
Positions vm_ends(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                  const PrefixSearch& how) {
  Subject subject{text, text.size(), impl.multibyte, how.notstart, how.notend};
  Positions ends;
  s::size_t reached = b;
  vm_run(impl, subject, Vm::Mode::Ends, b, false, nullptr, &ends, &reached);
  if (impl.vm->ctxs.empty()) return ends;
  Positions cut = cut_ends(impl, subject, b, reached + 1);
  if (cut.empty()) return ends;
  Positions all;
  s::set_union(ends.begin(), ends.end(), cut.begin(), cut.end(), s::back_inserter(all));
  return all;
}

// The VM: the length of the prefix from b that match_prefix() takes.
s::size_t vm_prefix(const CompiledPattern::Impl& impl, strv text, s::size_t b, bool longest,
                    PatternMatch* info, const PrefixSearch& how) {
  Subject subject{text, text.size(), impl.multibyte, how.notstart, how.notend};
  if (longest) {
    auto end = vm_run(impl, subject, Vm::Mode::First, b, false, info);
    return end ? *end - b : strv::npos;
  }
  // The shortest end; its groups are those of a whole match up to there.
  s::size_t reached = b;
  auto end = vm_run(impl, subject, Vm::Mode::Shortest, b, false, nullptr, nullptr, &reached);
  Positions cut;
  if (not impl.vm->ctxs.empty()) cut = cut_ends(impl, subject, b, (end ? *end : reached) + 1);
  if (not cut.empty()) {
    // Where the cut text decides, a match of it there.
    Positions ends = vm_ends(impl, text, b, how);
    end = s::nullopt;
    for (s::size_t e : ends)
      if (subject.boundary(e)
          or vm_matches(impl, text, b, e, nullptr, false, how.notstart, how.notend)) {
        end = e;
        break;
      }
  }
  if (not end) return strv::npos;
  if (info) {
    info->groups.clear();
    if (impl.captures > 0 and not vm_matches(impl, text, b, *end, info, false, how.notstart,
                                             how.notend))
      no_groups(impl, info);
  }
  return *end - b;
}

// The characters in text[b, e) as the matcher counts them: under MULTIBYTE
// a UTF-8 character is one, and so is a byte that starts none.
s::size_t chars_in(strv text, s::size_t b, s::size_t e, bool mb) {
  if (not mb) return e - b;
  s::size_t n = 0;
  for (s::size_t i = b; i < e; ++n) {
    if (static_cast<unsigned char>(text[i]) < 0x80) {
      ++i;
      continue;
    }
    s::size_t len = 1;
    utf8_decode(text, i, len);
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
    // Given up: the VM's answer, there (where the VM found an end or the
    // cut text may match) or, without a filter, for the whole search.
    if (not filter) return vm_prefix(impl, text, b, false, info, how);
    if (vm_matches(impl, text, b, e, info, false, how.notstart, how.notend)) return e - b;
  }
  return strv::npos;
}

}  // namespace

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
    return static_cast<s::int64_t>(start_char + chars_in(text, start, start + off, mb)) + base;
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

  CompiledPattern compile(strv seg) const {
    return CompiledPattern::compile(seg, nocase, extended, multibyte);
  }

  // The names in `dir` that `pat`, compiled from `seg`, matches.
  void match_dir(const str& dir, strv seg, const CompiledPattern& pat, bool need_dir,
                 vec<str>& out) {
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (not d) return;
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
  // Each segment with pattern characters, compiled once.  A part zsh
  // cannot compile (`(#X)`, `*#`) makes the word a bad pattern, as an
  // unclosed `[` does.
  vec<opt<CompiledPattern>> seg_pats;
  bool bad = s::any_of(excludes.begin(), excludes.end(),
                       [](const CompiledPattern& ex) { return ex.bad(); });
  for (const auto& seg : segs) {
    seg_pats.emplace_back();
    if (not has_glob_chars(seg, extended)) continue;
    seg_pats.back() = w.compile(seg);
    bad = bad or seg_pats.back()->bad();
  }
  if (bad) {
    if (not opts.get(OPT_BADPATTERN)) return vec<str>{glob_unescape(pat.raw)};
    return s::unexpected(qual_error("bad pattern: " + glob_unescape(pat.raw)));
  }
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
    } else if (not seg_pats[s]) {
      str lit = glob_unescape(seg);
      for (const auto& dir : cur) {
        str path = seg.empty() ? dir : join_path(dir, lit);
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0 and (not need_dir or is_dir(path)))
          next.push_back(s::move(path));
      }
    } else {
      for (const auto& dir : cur) w.match_dir(dir, seg, *seg_pats[s], need_dir, next);
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
