// The backtracking matcher: (#aN), and the patterns whose zsh quirks the
// VM does not follow.

#include <glob/pattern_internal.h>

#include <cassert>

#include <algorithm>
#include <utility>

#include <jfsh/core/aliases.h>
#include <jfsh/core/stack.h>

namespace jfsh::pattern {

namespace {

using Cont = FnRef<bool(s::size_t)>;

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
  bool skip(const Node& n, s::size_t pos, Cont k) const {
    if (cur_approx <= errs) return false;
    ++errs;
    if (pos >= lim) return false;
    s::size_t next = 0;
    at(pos, next);
    return reached(n, next, k);
  }

  // A `?` or `[…]`: with (#aN) it never matches a wrong character, but
  // extra ones before it are errors.
  bool single(const Node& n, s::size_t pos, Cont k) const {
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
  bool exact(const Node* lits, s::size_t len, s::size_t j, s::size_t pos, Cont k) const {
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
  bool resume(const Node* lits, s::size_t len, s::size_t j, s::size_t pos, Cont k) const {
    if (exact_end > lits[0].run) {
      errs = s::max(errs, cur_approx);
      return false;
    }
    if (exact_end < lits[0].run) return k(pos);
    return exact(lits, len, j, pos, k);
  }

  bool node(const Node& n, s::size_t pos, Cont k) const {
    enter(n);
    return reached(n, pos, k);
  }

  // Node n from pos, once reached (its Node::reset done).
  bool reached(const Node& n, s::size_t pos, Cont k) const {
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

      case Kind::Range:
        // The longest number first.
        for (s::size_t e : range_ends(n, pos)) {
          if (tick()) break;
          if (k(e)) return true;
        }
        return skip(n, pos, k);


      case Kind::Repeat:
        return repeat(n, pos, k);

      default:
        assert(false);  // should never get here
    }
    return false;
  }

  // Alternative `a` of group `g` from pos, then k.
  bool alt(const Node& g, s::size_t a, s::size_t pos, Cont k) const {
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
  bool repeat(const Node& n, s::size_t pos, Cont k) const {
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
  bool loop(const Node& n, s::size_t pos, Cont k) const {
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
  bool counted(const Node& n, int cur, s::size_t pos, Cont k) const {
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

  bool seq(const Seq& q, s::size_t i, s::size_t pos, Cont k) const {
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

}  // namespace

Verdict backtrack(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
                  PatternMatch* info, bool tails, bool notstart, bool notend, bool* spent) {
  Matcher m{{text, e, impl.multibyte, notstart, notend, b}, impl};
  bool ok = m.whole(impl, b, tails);
  if (spent) *spent = m.spent;
  // Given up inside an exclusion's test, it may have found a match that
  // is none.
  if (m.deep) return Verdict::GaveUp;
  if (not ok) return Verdict::No;
  if (info) m.report(b, *info);
  return Verdict::Yes;
}

opt<s::size_t> backtrack_open(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                              PatternMatch* info, const PrefixSearch& how, bool& spent,
                              s::size_t lim) {
  Matcher m{{text, s::min(lim, text.size()), impl.multibyte, how.notstart, how.notend, b}, impl};
  s::size_t found = m.open(impl, b);
  spent = m.spent;
  if (m.deep) return s::nullopt;
  if (found != strv::npos and info) {
    info->groups.clear();
    if (impl.captures > 0) m.report(b, *info);
  }
  return found;
}

s::size_t repeat_once(const CompiledPattern::Impl& impl, const Subject& subject, const Node& n,
                      s::size_t e, const Node::FlagPoint*& passed) {
  Matcher m{subject, impl};
  m.watch  = &n;
  m.passed = nullptr;
  s::size_t after = strv::npos;
  m.node(*n.sub, e, [&](s::size_t x) {
    after = x;
    return true;
  });
  passed = m.passed;
  return after;
}

}  // namespace jfsh::pattern
