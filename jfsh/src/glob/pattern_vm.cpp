// The pattern VM, for patterns without (#aN).

#include <glob/pattern_internal.h>

#include <cassert>

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <utility>

#include <jfsh/core/aliases.h>

namespace jfsh::pattern {

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
    // The state registers that matter here: live[live, live + nlive),
    // and as bits when there are at most 64 state registers.
    s::uint32_t live  = 0;
    s::uint32_t nlive = 0;
    s::uint64_t live_bits = 0;
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
    for (s::int32_t r : live)
      if (r < 64) in.live_bits |= s::uint64_t{1} << r;
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
        s::size_t at = keys.size();
        keys.resize(at + width);
        for (s::size_t i = 0; i < width; ++i) keys[at + i] = key[i];
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
    // Its starts' classes here, when there are few states and start
    // ranges (`few`), and whether the states changed since they were made.
    vec<Class> classes;
    bool       few     = false;
    bool       changed = true;
    // Else, with a few states, the sets of starts of the states that read
    // (and of those that matched) as `items`, and per class, the states
    // its starts are in (bits of `items`) and the start that stands for it
    // (Vm::rep()).
    bool                                   by_items = false;
    vec<const Spans*>                      items;
    vec<s::pair<s::uint64_t, s::uint32_t>> reps;
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
  vec<s::uint32_t>      seen;
  s::uint32_t           gen = 0;
  KeyTable              keys;
  vec<s::int64_t>       key, keep;
  vec<s::int64_t>       zeros;   // state registers all clear, at least nstate + 1
  vec<s::int64_t>       caps;    // Vm::caps
  vec<Work>             work;
  vec<s::int64_t>       wregs;
  vec<Spans>            tmp;
  s::size_t             ntmp = 0;
  Spans                 merged;
  // Vm::classify()'s.
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
  // Per step, where in dfa_dead its threads that end are listed, up to a
  // NEW_START.
  vec<s::uint32_t>                dfa_dead_at;
  vec<s::uint32_t>                dfa_dead;
  vec<char>                       taken;   // Vm::record()'s
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

// Where a thread a step added came from when no thread made it.
constexpr s::uint32_t NEW_START = UINT32_MAX;
// In a cached step's VmMade::saved: the thread gets a copy of the
// registers of the one it came from, which another thread took.
constexpr s::uint32_t COPY = s::uint32_t{1} << 31;

struct Vm : Subject {
  enum class Mode : s::uint8_t {
    Whole,      // s[b, lim) matches
    First,      // the match zsh's search finds first, the end left open
    Shortest,   // the shortest prefix of s[b, lim) that matches
    Ends,       // every end of a match from b
  };
  // Runs with a start at each character (Vm::start_here()), for
  // vm_find_start(): their threads in the order of their starts, the
  // first first or the last.  The run finds the start first in that order
  // among those with a match: one that matches cuts the threads after it,
  // and the run goes on.  Threads of two starts at the same state go on
  // alike, so only that of the start first in the order is kept; it has a
  // match where the other one has.
  enum class Starts : s::uint8_t { One, Ascending, Descending };
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
     bool want_caps, Starts order = Starts::One)
      : Subject{subject}, p{*impl.vm}, impl{impl}, sc{scratch}, mode{how}, want{want_caps},
        starts{order}, nstate{p.regs.size()},
        ncaps{static_cast<s::size_t>(p.caps) + (order == Starts::One ? 0 : 1)},
        stride{ncaps + p.regs.size()} {
    if (stride == 0) stride = 1;
  }

  // Runs the program from `entry` at b (with `starts`, at each character
  // from b before `until` too).  Whether it matched; the match ends at
  // `end`, with the group registers in `caps` and its start in `match_start`.
  bool run(s::size_t b, s::int32_t entry, s::size_t until = 0) {
    base = here = origin = b;
    last_start = until;
    injecting  = starts != Starts::One;
    steady  = nstate == 0 and not (entry == p.whole and p.tails);
    bare    = starts == Starts::One and (not want or p.caps == 0);
    sc.slab.clear();
    sc.free_slots.clear();
    sc.clist.clear();
    sc.nlist.clear();
    sc.stack.clear();
    sc.key.assign(1 + nstate, 0);
    sc.keep.assign(nstate + 1, 0);
    if (sc.zeros.size() < nstate + 1) sc.zeros.assign(nstate + 1, 0);
    if (sc.seen.size() < p.code.size()) sc.seen.resize(p.code.size(), 0);
    if (sc.ctxs.size() < p.ctxs.size()) sc.ctxs.resize(p.ctxs.size());
    for (s::size_t c = 0; c < p.ctxs.size(); ++c) {
      auto& cx = sc.ctxs[c];
      cx.cur.clear(1 + nstate);
      cx.old.clear(1 + nstate);
      cx.injected = -1;
    }
    position();
    clear_cache();
    start_here(sc.clist, entry, 0);
    // Short runs (most of those of ${x//…}) do not pay for the cache.
    static constexpr int UNCACHED_STEPS = 8;
    for (int steps = 0;; ++steps) {
      if (sc.clist.empty() and not injecting) break;
      // The threads as a cached state, and the steps cached from it.
      s::int32_t from = -1;
      if (steady and steps >= UNCACHED_STEPS) {
        from = intern(sc.clist);
        // Without the groups or starts, the states alone, the threads made
        // at the end.
        bool moved = false;
        // A step where starts stop being added is not cached.
        s::size_t until_here = injecting ? last_start - 1 : lim;
        while (from >= 0 and here < until_here and cacheable(here)) {
          auto k = static_cast<s::size_t>(from) * 128 + static_cast<unsigned char>(s[here]);
          s::int32_t to = sc.dfa_next[k];
          if (to < 0) break;
          if (not bare) {
            if (auto done = cached_step(from, to, k)) return *done;
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
        if (sc.clist.empty() and not injecting) break;
      }
      s::size_t pos = here, next = pos;
      bool more = pos < lim;
      sc.nlist.clear();
      sc.made.clear();
      if (more) {
        at(pos, next);
        here = next;
        position();
        if (injecting and here >= last_start) stop_starts();
        // Inner contexts first: the outer ones read what they found.
        for (s::size_t c = p.ctxs.size(); c-- > 0;) step(c, pos);
        if (injecting and starts == Starts::Descending) start_here(sc.nlist, entry, NEW_START);
      }
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
      if (injecting and starts == Starts::Ascending) start_here(sc.nlist, entry, NEW_START);
      // A step that depended on nothing but the threads' instructions and
      // the byte read is cached, with where each thread came from.
      if (not s::exchange(uncached, false) and from >= 0 and cacheable(pos)) {
        s::int32_t to = intern(sc.nlist);
        if (to >= 0) {
          auto k = static_cast<s::size_t>(from) * 128 + static_cast<unsigned char>(s[pos]);
          sc.dfa_next[k] = to;
          if (not bare) record(k, sc.clist.size());
        }
      }
      s::swap(sc.clist, sc.nlist);
    }
    return found;
  }

  // A thread from here, its registers cleared, made by the thread `src`
  // of the list read (NEW_START: none).  With `starts`, its start is here.
  void start_here(vec<VmThread>& list, s::int32_t entry, s::uint32_t src) {
    s::int32_t slot = alloc();
    s::int64_t* r = regs(slot);
    s::fill(r, r + p.caps, -1);
    if (starts != Starts::One) r[p.caps] = static_cast<s::int64_t>(here);
    s::fill(r + ncaps, r + stride, 0);
    add(list, entry, slot, src);
  }

  // No more starts: the states cached with them are not those without.
  void stop_starts() {
    injecting = false;
    clear_cache();
    uncached  = true;
  }

  // The thread at `slot` matched at pos: notes it, and whether the run is
  // over (a shortest match, or any whole one when its groups are not
  // wanted).  With `starts` the run goes on (see Starts; a TailLiteral
  // form matches before the end), the threads after this one cut, and
  // when they are in ascending order no more starts come.
  bool matched(s::int32_t slot, s::size_t pos) {
    found = true;
    if (mode == Mode::Ends) {
      if (ends.empty() or ends.back() != pos) ends.push_back(pos);
      return false;
    }
    end = pos;
    // A match a cached state found has no thread (-1);
    // those come only where the groups and starts are not wanted.
    assert(slot >= 0 or (not want and starts == Starts::One));
    if (want and slot >= 0) caps.assign(regs(slot), regs(slot) + p.caps);
    if (starts != Starts::One) {
      match_start = static_cast<s::size_t>(regs(slot)[p.caps]);
      if (injecting and starts == Starts::Ascending) stop_starts();
      return false;
    }
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
    sc.dfa_dead_at.clear();
    sc.dfa_dead.clear();
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
    if (not bare) {
      sc.dfa_made_at.resize(sc.dfa_made_at.size() + 128, 0);
      sc.dfa_dead_at.resize(sc.dfa_dead_at.size() + 128, 0);
    }
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

  // Caches step k from `n` threads, whose new threads came as sc.made
  // says: a thread's registers go to the first thread made from it,
  // copies (COPY) to the others; the threads that made none end.
  void record(s::size_t k, s::size_t n) {
    sc.dfa_made_at[k] = static_cast<s::uint32_t>(sc.dfa_made.size());
    sc.dfa_dead_at[k] = static_cast<s::uint32_t>(sc.dfa_dead.size());
    sc.taken.assign(n, 0);
    for (VmMade m : sc.made) {
      if (m.src != NEW_START and s::exchange(sc.taken[m.src], 1)) m.saved |= COPY;
      sc.dfa_made.push_back(m);
    }
    for (s::uint32_t i = 0; i < n; ++i)
      if (not sc.taken[i]) sc.dfa_dead.push_back(i);
    sc.dfa_dead.push_back(NEW_START);
  }

  // The cached step k from the threads in clist, at here, to state `to`.
  // A run that ends there, or a shortest or whole match found, gives its
  // answer.
  opt<bool> cached_step(s::int32_t from_state, s::int32_t to, s::size_t k) {
    s::uint32_t made = sc.dfa_made_at[k];
    s::size_t pos = here++;
    // The step finds the first match; those after it it leaves out.  (No
    // step from a state with a match is cached while ascending starts are
    // added: the match stops them.)
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
    for (s::uint32_t i = b; i < e; ++i) {
      const VmMade& m = sc.dfa_made[made + (i - b)];
      s::int32_t slot = 0;
      if (m.src == NEW_START) {
        slot = alloc();
        s::int64_t* r = regs(slot);
        s::fill(r, r + p.caps, -1);
        r[p.caps] = static_cast<s::int64_t>(here);
        s::fill(r + ncaps, r + stride, 0);
      } else {
        slot = sc.clist[m.src].slot;
        if (m.saved & COPY) slot = clone(slot);
      }
      sc.nlist.push_back({sc.dfa_pcs[i], slot});
    }
    // Then the group registers set here, the copies made.
    for (s::uint32_t i = b; i < e; ++i) {
      s::int64_t* r = regs(sc.nlist[i - b].slot);
      for (s::uint32_t bits = sc.dfa_made[made + (i - b)].saved & ~COPY; bits != 0;
           bits &= bits - 1)
        r[s::countr_zero(bits)] = static_cast<s::int64_t>(here);
    }
    for (s::uint32_t j = sc.dfa_dead_at[k]; sc.dfa_dead[j] != NEW_START; ++j)
      release(sc.clist[sc.dfa_dead[j]].slot);
    s::swap(sc.clist, sc.nlist);
    if (sc.clist.empty() and not injecting) return found;
    return s::nullopt;
  }

  s::int64_t* regs(s::int32_t slot) {
    return sc.slab.data() + static_cast<s::size_t>(slot) * stride;
  }
  // The state registers, after the group ones and the start.
  s::int64_t* state(s::int32_t slot) { return regs(slot) + ncaps; }

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
    s::uint32_t q = start(from);
    if (cx.by_items) {
      // The first start asked for with the same states stands for them.
      s::uint64_t in = 0;
      for (s::size_t i = 0; i < cx.items.size(); ++i)
        if (spans_have(*cx.items[i], q)) in |= s::uint64_t{1} << i;
      if (in == 0) return -1;
      for (auto [k, r] : cx.reps)
        if (k == in) return static_cast<s::int64_t>(base) + r;
      cx.reps.emplace_back(in, q);
      return from;
    }
    if (not cx.few) return alive(c, from) ? from : -1;
    auto it = s::upper_bound(cx.classes.begin(), cx.classes.end(), q,
                             [](s::uint32_t v, const auto& k) { return v < k.lo; });
    if (it == cx.classes.begin() or q > (it - 1)->hi) return -1;
    return static_cast<s::int64_t>(base) + (it - 1)->rep;
  }

  // Splits context cx's starts into the classes rep() gives: ranges
  // between the ends of its states' starts, by the states they are in.
  // With many such ranges (the starts every other place of `*(^(ab)#c)`),
  // the classes are found by the states of each start asked for.
  void classify(VmScratch::Ctx& cx) {
    static constexpr s::size_t MAX_ITEMS = 16;
    static constexpr s::size_t MAX_CUTS  = 128;
    cx.changed  = false;
    cx.few      = false;
    cx.by_items = false;
    cx.classes.clear();
    auto& items = cx.items;
    items.clear();
    for (s::uint32_t st : cx.cur.reading) items.push_back(&cx.cur.sets[cx.cur.set_of[st]]);
    items.push_back(&cx.acc.in_order());
    auto by_items = [&] {
      cx.by_items = items.size() <= 64;
      cx.reps.clear();
    };
    if (items.size() > MAX_ITEMS) return by_items();
    auto& cuts = sc.cuts;
    cuts.clear();
    for (const Spans* set : items)
      for (Span x : *set) {
        cuts.push_back(x.lo);
        cuts.push_back(x.hi + 1);
      }
    if (cuts.size() > MAX_CUTS) return by_items();
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
    if (nstate <= 64) {
      // Each register in place: no copies, no clearing all of them first.
      for (s::size_t reg = 0; reg < nstate; ++reg)
        r[reg] = (in.live_bits >> reg) & 1 ? canon_one(reg, r[reg]) : 0;
      return;
    }
    s::int64_t* keep = sc.keep.data();
    for (s::uint32_t i = 0; i < in.nlive; ++i) keep[i] = r[p.live[in.live + i]];
    s::fill(r, r + nstate, 0);
    for (s::uint32_t i = 0; i < in.nlive; ++i) {
      auto reg = static_cast<s::size_t>(p.live[in.live + i]);
      r[reg] = canon_one(reg, keep[i]);
    }
  }

  // Live state register `reg`, holding v, in its canonical form.
  s::int64_t canon_one(s::size_t reg, s::int64_t v) {
    const auto& ri = p.regs[reg];
    switch (ri.kind) {
      case VmProgram::RegKind::Count:
        return count_class(ri.min, ri.max, v, here);

      case VmProgram::RegKind::Iter:
        return v == static_cast<s::int64_t>(here) ? v : -1;

      case VmProgram::RegKind::Ctx:
        return rep(ri.ctx, v);

      case VmProgram::RegKind::Leak:
        return v;

      default:
        assert(false);  // should never get here
    }
    return v;
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
            go = pass(boundary(here), slot);
            ++pc;

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
      const Node::FlagPoint* passed = nullptr;
      s::size_t after = repeat_once(impl, sub, n, e, passed);
      if (after == strv::npos) {
        last = passed;
        leak_memo[LeakKey{&n, c, e, leak}] = last;
        break;
      }
      chain.push_back({c, e, passed});
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
    sub_add(static_cast<s::size_t>(c), p.ctxs[static_cast<s::size_t>(c)].entry,
            sc.zeros.data(), set);
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
  Starts                           starts;
  s::size_t                        nstate;  // state registers
  s::size_t                        ncaps;   // group registers, then the start
  s::size_t                        stride;  // registers per thread
  s::size_t                        base = 0, here = 0;
  bool                             found = false;
  s::size_t                        end   = 0;
  s::size_t                        match_start = 0;   // with `starts`
  // With `starts`: whether they are still added, and before where.
  bool                             injecting  = false;
  s::size_t                        last_start = 0;
  bool                             uncached = false;   // the step is not to be cached
  vec<s::int64_t>&                 caps = sc.caps;   // the match's group registers
  vec<s::size_t>                   ends;
  bool                             steady  = false;   // see cacheable()
  // Cached steps follow the states alone, not the threads' registers.
  bool                             bare    = false;
  umap<LeakKey, const Node::FlagPoint*, LeakHash> leak_memo;   // leaked()'s answers
};

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

void no_groups(const CompiledPattern::Impl& impl, PatternMatch* info) {
  if (info) info->groups.assign(static_cast<s::size_t>(impl.captures), {strv::npos, strv::npos});
}

// A VM run from b on `subject`; `tails` takes the whole-match forms.  The
// groups of a match go to `info`, every end to `ends`.
// The groups the run `vm` from b found, into `info`.
void take_groups(const CompiledPattern::Impl& impl, const Vm& vm, s::size_t b,
                 PatternMatch& info) {
  info.groups.clear();
  for (int g = 0; g < impl.captures; ++g) {
    s::int64_t gb = vm.caps[static_cast<s::size_t>(2 * g)];
    s::int64_t ge = vm.caps[static_cast<s::size_t>(2 * g + 1)];
    if (gb < 0 or ge < 0) info.groups.emplace_back(strv::npos, strv::npos);
    else info.groups.emplace_back(static_cast<s::size_t>(gb) - b, static_cast<s::size_t>(ge) - b);
  }
}

opt<s::size_t> vm_run(const CompiledPattern::Impl& impl, Subject subject, Vm::Mode mode,
                      s::size_t b, bool tails, PatternMatch* info, Positions* ends = nullptr) {
  if (ends) ends->clear();
  strv rest = subject.s.substr(b, subject.lim - b);
  for (char c : impl.vm->needs)
    if (rest.find(c) == strv::npos) return s::nullopt;
  ScratchLease lease;
  Vm vm{subject, impl, *lease.sc, mode, info and impl.captures > 0};
  bool found = vm.run(b, tails ? impl.vm->whole : impl.vm->prefix);
  if (ends) *ends = s::move(vm.ends);
  if (not found) return s::nullopt;
  if (info) take_groups(impl, vm, b, *info);
  return vm.end;
}

}  // namespace

void compile_vm(CompiledPattern::Impl& impl) {
  // (#aN) has only the backtracker.
  if (impl.approx) return;
  const Node& top = impl.main.front();
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

bool vm_matches(const CompiledPattern::Impl& impl, strv text, s::size_t b, s::size_t e,
                PatternMatch* info, bool tails, bool notstart, bool notend) {
  Subject subject{text, e, impl.multibyte, notstart, notend};
  return vm_run(impl, subject, Vm::Mode::Whole, b, tails, info).has_value();
}

Positions vm_ends(const CompiledPattern::Impl& impl, strv text, s::size_t b,
                  const PrefixSearch& how) {
  Subject subject{text, text.size(), impl.multibyte, how.notstart, how.notend};
  Positions ends;
  vm_run(impl, subject, Vm::Mode::Ends, b, false, nullptr, &ends);
  return ends;
}

s::size_t vm_prefix(const CompiledPattern::Impl& impl, strv text, s::size_t b, bool longest,
                    PatternMatch* info, const PrefixSearch& how) {
  Subject subject{text, text.size(), impl.multibyte, how.notstart, how.notend};
  auto end = vm_run(impl, subject, longest ? Vm::Mode::First : Vm::Mode::Shortest, b, false,
                    longest ? info : nullptr);
  if (not end) return strv::npos;
  // The shortest end's groups are those of a whole match up to there.
  if (info and not longest) {
    info->groups.clear();
    if (impl.captures > 0 and not vm_matches(impl, text, b, *end, info, false, how.notstart,
                                             how.notend))
      no_groups(impl, info);
  }
  return *end - b;
}

s::size_t vm_find_start(const CompiledPattern::Impl& impl, strv text, s::size_t from,
                        s::size_t until, bool whole, bool last, bool notend, PatternMatch* info) {
  Subject subject{text, text.size(), impl.multibyte, false, whole ? false : notend};
  subject.origin = from;
  // No match starts after the last of a character every match has.
  until = s::min(until, text.size() + 1);
  for (char c : impl.vm->needs) {
    s::size_t at = text.rfind(c);
    if (at == strv::npos or at < from) return strv::npos;
    until = s::min(until, at + 1);
  }
  if (from >= until) return strv::npos;
  Vm::Mode mode = whole ? Vm::Mode::Whole : Vm::Mode::Shortest;
  s::int32_t entry = whole ? impl.vm->whole : impl.vm->prefix;
  bool groups = info and whole;
  ScratchLease lease;
  // The starts are tried one at a time while that has read no more than
  // a run with all of them reads, as the nearest ones usually decide.
  s::size_t read = 0, room = text.size() - from + 1;
  auto one = [&](s::size_t b) {
    Vm vm{subject, impl, *lease.sc, mode, groups and impl.captures > 0};
    bool found = vm.run(b, entry);
    read += vm.here - b;
    if (found and groups) take_groups(impl, vm, b, *info);
    return found;
  };
  // A run with a start at each character from b before `end`.
  auto all = [&](s::size_t b, s::size_t end, Vm::Starts order) {
    Vm vm{subject, impl, *lease.sc, mode, false, order};
    if (not vm.run(b, entry, end)) return strv::npos;
    if (groups and not vm_matches(impl, text, vm.match_start, text.size(), info))
      no_groups(impl, info);
    return vm.match_start;
  };
  if (not last) {
    for (s::size_t b = from, next = 0; b < until; b = next) {
      if (read > room) return all(b, until, Vm::Starts::Ascending);
      if (one(b)) return b;
      if (b == text.size()) break;
      subject.at(b, next);
    }
    return strv::npos;
  }
  // The starts from the last, as characters are read from `from`.
  for (s::size_t b = until - 1;; --b) {
    while (b > from and not subject.boundary(b)) --b;
    if (read > room) return all(from, b + 1, Vm::Starts::Descending);
    if (one(b)) return b;
    if (b == from) return strv::npos;
  }
}

}  // namespace jfsh::pattern
