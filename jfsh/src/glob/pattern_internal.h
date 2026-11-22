#ifndef JFSH_GLOB_PATTERN_INTERNAL_H
#define JFSH_GLOB_PATTERN_INTERNAL_H
// What the pattern compiler (pattern.cpp), the backtracker
// (pattern_backtrack.cpp) and the VM (pattern_vm.cpp) share: the parsed
// pattern, the subject they read, and their entry points.

#include <cctype>
#include <cwctype>

#include <algorithm>
#include <memory>
#include <type_traits>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>
#include <jfsh/glob/glob.h>

namespace jfsh {

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

namespace pattern {
struct VmProgram;
}  // namespace pattern

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
  sptr<const pattern::VmProgram> vm;
};

namespace pattern {

using Node = CompiledPattern::Node;
using Seq  = PatternSeq;
using Kind = Node::Kind;

// zsh reports the extents of the first nine (#b) groups.
constexpr int MAX_CAPTURES = 9;

// The ends of matches from a place, ascending.
using Positions = vec<s::size_t>;

inline bool single_char(const Node& n) {
  return n.kind == Kind::Lit or n.kind == Kind::Any or n.kind == Kind::Class;
}

// zsh's case-insensitive comparison of character `c` with pattern
// character `pc`: (#i) either case, (#l) a lower-case `pc` either case.
inline bool same_char(bool nocase, bool lower, char32_t c, char32_t pc, bool mb) {
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
inline bool class_has(const Node& n, char32_t c, bool mb) {
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
  // Whether a character may start at p: not inside one.
  bool boundary(s::size_t p) const {
    return not mb or p >= lim or (static_cast<unsigned char>(s[p]) & 0xc0) != 0x80
        or not continues(p);
  }
  // Whether the continuation byte at p is inside a character: the byte
  // that starts no run of them, at most three back and from `origin`,
  // starts one that reaches past p.  Else it is a character of its own.
  // (In pattern.cpp, out of the way of the matchers' loops.)
  bool continues(s::size_t p) const;
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

  // The ends of the numbers from p (runs of digits, any length) whose
  // value, saturating at INT64_MAX as zsh's, is in the <lo-hi> `n`: the
  // longest first.
  Positions range_ends(const Node& n, s::size_t p) const {
    Positions ends;
    s::uint64_t v = 0;
    for (s::size_t e = p; e < lim and s::isdigit(static_cast<unsigned char>(s[e])); ++e) {
      auto d = static_cast<unsigned>(s[e] - '0');
      v = v > (INT64_MAX - d) / 10 ? INT64_MAX : v * 10 + d;
      auto x = static_cast<s::int64_t>(v);
      if ((not n.has_lo or x >= n.lo) and (not n.has_hi or x <= n.hi)) ends.push_back(e + 1);
    }
    s::reverse(ends.begin(), ends.end());
    return ends;
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
  // Where the matchers start reading characters: no character before it
  // reaches past it.
  s::size_t origin   = 0;
  // The case flags a (#cN,M) leaked (Node::leak) for the literals of
  // this epoch, while the backtracker tries what follows it.
  mutable int  leak_epoch  = -1;
  mutable bool leak_nocase = false;
  mutable bool leak_lower  = false;
};

// A non-owning reference to a callable, for the backtracker's
// continuations: two pointers, where a std::function may allocate.  It must
// not outlive what it refers to.
template<typename F>
struct FnRef;

template<typename R, typename... A>
struct FnRef<R(A...)> {
  template<typename F>
    requires(not s::is_same_v<s::remove_cvref_t<F>, FnRef>)
  FnRef(F&& f) noexcept   // NOLINT(google-explicit-constructor): as std::function
      : obj{const_cast<void*>(static_cast<const void*>(s::addressof(f)))},
        fn{[](void* o, A... a) -> R {
          return (*static_cast<s::remove_reference_t<F>*>(o))(s::forward<A>(a)...);
        }} {}

  R operator()(A... a) const { return fn(obj, s::forward<A>(a)...); }

  void* obj;
  R (*fn)(void*, A...);
};

// --- pattern_backtrack.cpp

// What the backtracking matcher found.
enum class Verdict : s::uint8_t { No, Yes, GaveUp };

// The backtracking matcher on text[b, e), and the (#b) groups of a match
// into `info`.  With `tails`, as a whole match; else as a prefix match
// that ends at e.
// When it gives up, `spent` says whether on steps (else on the stack).
Verdict backtrack(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
                  PatternMatch* info, bool tails = true, bool notstart = false,
                  bool notend = false, bool* spent = nullptr);

// The backtracking matcher with the end left open, on the text cut at
// `lim`: the end zsh's search reaches first, npos when there is none, and
// nothing when it gave up (`spent` when on steps).
opt<s::size_t> backtrack_open(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                              PatternMatch* info, const PrefixSearch& how, bool& spent,
                              s::size_t lim = strv::npos);

// One repeat of (#cN,M) `n` from e on `subject`, as the backtracker first
// finds it, watching the changes of the case flags it passes: where it
// ends (npos when it fails), and in `passed` the last change it passed.
s::size_t repeat_once(const CompiledPattern::Impl& impl, const Subject& subject, const Node& n,
                      s::size_t e, const Node::FlagPoint*& passed);

// --- pattern_vm.cpp

// Compiles `impl`, parsed, for the VM.
void compile_vm(CompiledPattern::Impl& impl);

// The VM: whether text[b, e) matches (with `tails`, as a whole match).
bool vm_matches(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
                PatternMatch* info, bool tails = true, bool notstart = false, bool notend = false);

// The VM: every end of a match from b.
Positions vm_ends(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                  const PrefixSearch& how);

// The VM: CompiledPattern::find_start(), (#e) failing with `notend`.
s::size_t vm_find_start(const CompiledPattern::Impl& impl, strv text, s::size_t from,
                        s::size_t until, bool whole, bool last, bool notend, PatternMatch* info);

// The VM: the length of the prefix from b that match_prefix() takes.
s::size_t vm_prefix(const CompiledPattern::Impl& impl, strv text, s::size_t b, bool longest,
                    PatternMatch* info, const PrefixSearch& how);

}  // namespace pattern
}  // namespace jfsh

#endif  // JFSH_GLOB_PATTERN_INTERNAL_H
