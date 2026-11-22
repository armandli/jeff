// The arithmetic evaluator in this file is derived from zsh 5.9's
// Src/math.c (its lexer, operator tables, mathparse and op) and zstrtol from
// Src/utils.c.  zsh's copyright and licence:
//
// The Z Shell is copyright (c) 1992-2017 Paul Falstad, Richard Coleman,
// Zoltán Hidvégi, Andrew Main, Peter Stephenson, Sven Wischnowsky, and
// others.  All rights reserved.  Individual authors, whether or not
// specifically named, retain copyright in all changes; in what follows, they
// are referred to as `the Zsh Development Group'.  This is for convenience
// only and this body has no legal status.  The Z shell is distributed under
// the following licence; any provisions made in individual files take
// precedence.
//
// Permission is hereby granted, without written agreement and without
// licence or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall the Zsh Development Group be liable to any party for
// direct, indirect, special, incidental, or consequential damages arising out
// of the use of this software and its documentation, even if the Zsh
// Development Group have been advised of the possibility of such damage.
//
// The Zsh Development Group specifically disclaim any warranties, including,
// but not limited to, the implied warranties of merchantability and fitness
// for a particular purpose.  The software provided hereunder is on an "as is"
// basis, and the Zsh Development Group have no obligation to provide
// maintenance, support, updates, enhancements, or modifications.

#include <jfsh/builtins/math.h>

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/stack.h>
#include <jfsh/core/utf8.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/module.h>

// TODO(math/output): output bases `$(( [#16] x ))` and C_BASES.

namespace jfsh {

s::int64_t MathNum::as_int() const noexcept {
  if (not is_float) return i;
  if (s::isnan(d)) return 0;
  constexpr double LIMIT = 9223372036854775807.0;
  if (d >= LIMIT)  return s::numeric_limits<s::int64_t>::max();
  if (d <= -LIMIT) return s::numeric_limits<s::int64_t>::min();
  return static_cast<s::int64_t>(d);
}

MathOptions math_options(const OptionState& opts) noexcept {
  MathOptions m;
  m.force_float        = opts.get(OPT_FORCEFLOAT);
  m.octal_zeroes       = opts.get(OPT_OCTALZEROES);
  m.c_precedences      = opts.get(OPT_CPRECEDENCES);
  m.ksh_arrays         = opts.get(OPT_KSH_ARRAYS);
  m.ksh_zero_subscript = opts.get(OPT_KSH_ZERO_SUBSCRIPT);
  m.no_unset           = not opts.get(OPT_UNSET);
  m.multibyte          = opts.multibyte();
  return m;
}

MathOptions math_options(const ExecContext& ctx) noexcept {
  MathOptions m = math_options(ctx.opts);
  m.shell = &ctx;
  return m;
}

str format_math(const MathNum& n) {
  if (not n.is_float) return s::to_string(n.i);
  if (s::isnan(n.d)) return "NaN";
  if (s::isinf(n.d)) return n.d < 0 ? "-Inf" : "Inf";
  char buf[64];
  s::snprintf(buf, sizeof buf, "%.17g", n.d);
  str s = buf;
  if (s.find_first_of(".e") == str::npos) s += '.';
  return s;
}

bool is_subscript_flags(strv sub) noexcept {
  if (sub.empty() or sub[0] != '(') return false;
  for (s::size_t k = 1; k < sub.size(); ++k) {
    char c = sub[k];
    if (c == ')') return true;
    if (strv("wpfrRkKiIe").find(c) != strv::npos) continue;
    if (strv("snb").find(c) == strv::npos or k + 1 >= sub.size()) return false;
    // s:sep:, n:num:, b:num:, with any delimiter, or a bracket pair.
    char open = sub[++k];
    char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}'
               : open == '<' ? '>' : open;
    auto end = sub.find(close, k + 1);
    if (end == strv::npos) return false;
    k = end;
  }
  return false;
}

namespace {

// A character as zsh shows it in this message: `^A` for a control
// character, `\M-` and the low seven bits for a byte with the top bit set,
// where zsh 5.9 leaves the `^` out (0x81 is `\M-A`).
str nice_char(char ch) {
  auto c = static_cast<unsigned char>(ch);
  str out;
  bool meta = c & 0x80;
  if (meta) {
    out = "\\M-";
    c &= 0x7f;
  }
  if (c == 0x7f) out += meta ? "?" : "^?";
  else if (c < 0x20) out += str(meta ? "" : "^") + static_cast<char>(c | 0x40);
  else out += static_cast<char>(c);
  return out;
}

// Integer arithmetic wraps around on overflow, as in zsh, instead of being
// undefined behaviour: do it in unsigned and convert back.
using U = s::uint64_t;
s::int64_t wrap(U v) noexcept { return static_cast<s::int64_t>(v); }
s::int64_t wadd(s::int64_t a, s::int64_t b) noexcept { return wrap(static_cast<U>(a) + static_cast<U>(b)); }
s::int64_t wsub(s::int64_t a, s::int64_t b) noexcept { return wrap(static_cast<U>(a) - static_cast<U>(b)); }
s::int64_t wmul(s::int64_t a, s::int64_t b) noexcept { return wrap(static_cast<U>(a) * static_cast<U>(b)); }
s::int64_t wneg(s::int64_t a) noexcept { return wrap(U{0} - static_cast<U>(a)); }
// C leaves shifts by a negative count or by 64 or more undefined; zsh gets
// what the hardware does, which on arm64 and x86-64 uses the count mod 64.
s::int64_t wshl(s::int64_t a, s::int64_t n) noexcept { return wrap(static_cast<U>(a) << (n & 63)); }
s::int64_t wshr(s::int64_t a, s::int64_t n) noexcept { return a >> (n & 63); }

s::int64_t ipow(s::int64_t b, s::int64_t e) noexcept {
  s::int64_t r = 1;
  for (; e > 0; e >>= 1, b = wmul(b, b))
    if (e & 1) r = wmul(r, b);
  return r;
}

bool is_blank(char c) noexcept { return c == ' ' or c == '\t' or c == '\n'; }
bool is_digit(char c) noexcept { return c >= '0' and c <= '9'; }

// zsh's tokens, in its order (the tables below are indexed by them).
enum Tok : s::uint8_t {
  T_INPAR, T_OUTPAR, T_NOT, T_COMP, T_POSTPLUS,
  T_POSTMINUS, T_UPLUS, T_UMINUS, T_AND, T_XOR,
  T_OR, T_MUL, T_DIV, T_MOD, T_PLUS,
  T_MINUS, T_SHLEFT, T_SHRIGHT, T_LES, T_LEQ,
  T_GRE, T_GEQ, T_DEQ, T_NEQ, T_DAND,
  T_DOR, T_DXOR, T_QUEST, T_COLON, T_EQ,
  T_PLUSEQ, T_MINUSEQ, T_MULEQ, T_DIVEQ, T_MODEQ,
  T_ANDEQ, T_XOREQ, T_OREQ, T_SHLEFTEQ, T_SHRIGHTEQ,
  T_DANDEQ, T_DOREQ, T_DXOREQ, T_COMMA, T_EOI,
  T_PREPLUS, T_PREMINUS, T_NUM, T_ID, T_POWER,
  T_CID, T_POWEREQ, T_FUNC,
  T_COUNT,
};

// Associativity, and what an operator takes and gives (zsh's type table).
constexpr s::uint16_t LR      = 0x0000;
constexpr s::uint16_t RL      = 0x0001;
constexpr s::uint16_t BOOL    = 0x0002;
constexpr s::uint16_t OP_A2   = 0x0004;   // two operands
constexpr s::uint16_t OP_A2IR = 0x0008;   // … giving an integer
constexpr s::uint16_t OP_A2IO = 0x0010;   // … taken and given as integers
constexpr s::uint16_t OP_E2   = 0x0020;   // an assignment
constexpr s::uint16_t OP_E2IO = 0x0040;   // … of integers
constexpr s::uint16_t OP_OP   = 0x0080;   // comes where an operator is expected
constexpr s::uint16_t OP_OPF  = 0x0100;   // an operator follows it
constexpr s::uint16_t BINARY  = OP_A2 | OP_A2IR | OP_A2IO | OP_E2 | OP_E2IO;

constexpr int mtype(s::uint16_t t) noexcept { return t & 3; }

constexpr s::array<s::uint16_t, T_COUNT> TYPE = {
  LR, LR | OP_OP | OP_OPF, RL, RL, RL | OP_OP | OP_OPF,
  RL | OP_OP | OP_OPF, RL, RL, LR | OP_A2IO, LR | OP_A2IO,
  LR | OP_A2IO, LR | OP_A2, LR | OP_A2, LR | OP_A2, LR | OP_A2,
  LR | OP_A2, LR | OP_A2IO, LR | OP_A2IO, LR | OP_A2IR, LR | OP_A2IR,
  LR | OP_A2IR, LR | OP_A2IR, LR | OP_A2IR, LR | OP_A2IR, BOOL | OP_A2IO,
  BOOL | OP_A2IO, LR | OP_A2IO, RL | OP_OP, RL | OP_OP, RL | OP_E2,
  RL | OP_E2, RL | OP_E2, RL | OP_E2, RL | OP_E2, RL | OP_E2,
  RL | OP_E2IO, RL | OP_E2IO, RL | OP_E2IO, RL | OP_E2IO, RL | OP_E2IO,
  BOOL | OP_E2IO, BOOL | OP_E2IO, RL | OP_A2IO, RL | OP_A2, RL | OP_OP,
  RL, RL, LR | OP_OPF, LR | OP_OPF, RL | OP_A2,
  LR | OP_OPF, RL | OP_E2, LR | OP_OPF,
};

// Precedences, lower binding tighter: zsh's default and C_PRECEDENCES.
constexpr s::array<s::uint8_t, T_COUNT> Z_PREC = {
  1, 137, 2, 2, 2,
  2, 2, 2, 4, 5,
  6, 8, 8, 8, 9,
  9, 3, 3, 10, 10,
  10, 10, 11, 11, 12,
  13, 13, 14, 15, 16,
  16, 16, 16, 16, 16,
  16, 16, 16, 16, 16,
  16, 16, 16, 17, 200,
  2, 2, 0, 0, 7,
  0, 16, 0,
};
constexpr s::array<s::uint8_t, T_COUNT> C_PREC = {
  1, 137, 2, 2, 2,
  2, 2, 2, 9, 10,
  11, 4, 4, 4, 5,
  5, 6, 6, 7, 7,
  7, 7, 8, 8, 12,
  14, 13, 15, 16, 17,
  17, 17, 17, 17, 17,
  17, 17, 17, 17, 17,
  17, 17, 17, 18, 200,
  2, 2, 0, 0, 3,
  0, 17, 0,
};

// zsh's limits: values pending on one evaluation's stack, and evaluations
// nested through parameter values, subscripts and function arguments.
constexpr int STACK_SIZE = 100;
constexpr int MAX_LEVEL  = 256;

// A vector taken from a pool and given back with its capacity, so
// repeated and nested evaluations don't allocate.
template <typename T>
struct Pooled {
  explicit Pooled(vec<vec<T>>& p) : pool(p) {
    if (not p.empty()) {
      v = s::move(p.back());
      p.pop_back();
    }
  }
  ~Pooled() {
    v.clear();
    // A vector grown by one very deep expression is let go.
    if (v.capacity() > 4096) v = {};
    pool.push_back(s::move(v));
  }
  Pooled(const Pooled&) = delete;
  Pooled& operator=(const Pooled&) = delete;

  vec<vec<T>>& pool;
  vec<T>       v;
};

// What a `name[sub]` lvalue was found to name, worked out once (so
// `a[i++] += 1` steps i once, as in zsh).
enum class Slot : s::uint8_t {
  None,      // not looked at yet
  Whole,     // no subscript
  All,       // name[@], name[*]
  Hash,      // an associative array's key, the subscript text
  Index,     // element `first` of an array or scalar
  Range,     // elements first … last
  Flagged,   // name[(flags)…], read through MathOptions::flagged_element
};

// An entry of the value stack: a number, and for an lvalue the text
// `name` or `name[sub]` it came from.
struct Entry {
  MathNum    val;
  strv       lval;
  // A resolved subscript: zsh's 1-based indexes (KSH_ARRAYS undone).
  s::int64_t first = 0;
  s::int64_t last  = 0;
  Slot       slot  = Slot::None;
  bool       unset = false;   // val is still to be fetched from lval
};

// mathparse's pending work when it parses an operand at a new precedence:
// zsh recurses there, this keeps a stack.
enum class Cont : s::uint8_t { None, Paren, Binary, QuestMid, QuestElse };

struct Frame {
  int  pc;
  int  onoeval = 0;
  Tok  otok    = T_EOI;
  Cont cont    = Cont::None;
  bool q       = false;
};

// The pools of one thread, looked up once per math_eval (a thread_local
// costs a call on macOS).
struct Scratch {
  vec<vec<Entry>> entries;
  vec<vec<Frame>> frames;
};

struct Arith {
  Arith(strv text, ParamTable& p, const MathOptions& o, const MathFuncRegistry* f,
        Scratch& sc, int lvl)
    : s(text), params(p), opts(o), funcs(f),
      prec(o.c_precedences ? C_PREC : Z_PREC), scratch(sc), stack(sc.entries), level(lvl) {}

  int top_prec() const noexcept { return prec[T_COMMA] + 1; }
  int arg_prec() const noexcept { return prec[T_COMMA] - 1; }

  bool failed() const noexcept { return not err.empty(); }

  // Only the first error counts, as zsh's errflag stops the rest.
  void fail(str msg) {
    if (err.empty()) err = s::move(msg);
  }
  void bad(strv msg) { fail("bad math expression: " + str(msg)); }

  char at(s::size_t k) const noexcept { return k < s.size() ? s[k] : '\0'; }

  MathNum zero() const noexcept {
    return opts.force_float ? MathNum::of(0.0) : MathNum::of(s::int64_t{0});
  }

  // ---------------------------------------------------------------------
  // Nested evaluations: a parameter's value, a subscript, an argument.
  // ---------------------------------------------------------------------

  // zsh's matheval on `text`: an empty text is 0, and anything left over
  // is an illegal character.
  MathNum eval_text(strv text);

  // The integer `text` gives as a subscript.
  s::int64_t eval_index(strv text) { return eval_text(text).as_int(); }

  // ---------------------------------------------------------------------
  // Parameters.
  // ---------------------------------------------------------------------

  static strv lval_name(strv lval) noexcept { return lval.substr(0, lval.find('[')); }

  static strv lval_sub(strv lval) noexcept;

  // A subscript index as zsh's 1-based one.
  s::int64_t from_ksh(s::int64_t i, bool single) const noexcept {
    if (opts.ksh_arrays and i >= 0) return wadd(i, 1);
    if (single and i == 0 and opts.ksh_zero_subscript) return 1;
    return i;
  }

  // Works out what `e.lval` names.  `write`: a flagged subscript is found
  // as an index to assign to (`a[(r)pat]` as `a[(i)pat]`).
  bool resolve(Entry& e, bool write = false);

  // $ARGC and $argv, which live in the shell rather than the table.
  opt<str> shell_param(strv name) const;

  // The text `e` names; nullopt when the parameter is not set.  As zsh's
  // getvalue, the subscript of a name that isn't set is not evaluated.
  opt<str> param_text(Entry& e);

  // zsh's getmathparam: the number an lvalue holds.
  MathNum getmathparam(Entry& e);

  // zsh's setmathvar: stores `v` in the lvalue of `e`, and gives it back as
  // the type of the parameter it went to.  `plain`: a plain `=`, which
  // replaces an array or associative array with a number.
  MathNum setmathvar(Entry& e, MathNum v, bool plain = false);

  // ---------------------------------------------------------------------
  // The lexer (zsh's zzlex).
  // ---------------------------------------------------------------------

  // `##c` and `#\c`: the character's code; `^c` is a control character
  // and `\C-c`, `\M-c` and the C escapes work as in $'…'.
  s::int64_t char_code();

  // The end of the identifier at `k` (zsh's itype_end with IIDENT): ASCII
  // letters, digits and `_`, and multibyte letters and digits.
  s::size_t ident_end(s::size_t k) const noexcept;

  MathNum integer_value(s::int64_t v) const noexcept {
    return opts.force_float ? MathNum::of(static_cast<double>(v)) : MathNum::of(v);
  }

  // A number at pos (a digit or `.`).
  Tok lexconstant();

  Tok lex();

  // Checks that an operand or an operator came, whichever was expected.
  void checkunary(Tok tok, s::size_t optr);

  // ---------------------------------------------------------------------
  // The value stack and the operators.
  // ---------------------------------------------------------------------

  void push(MathNum val, strv lval, bool getme);

  MathNum pop(bool noget);

  // Fetches the top entry's value if it is still to be read.
  MathNum& top_value();

  // Before the right side of && || &&= ||=: skips it when it can't matter.
  void bop(Tok tk);

  // Integer division by zero is an error; a float one gives Inf or NaN.
  bool notzero(const MathNum& n) {
    if (not n.is_float and n.i == 0) {
      fail("division by zero");
      return false;
    }
    return true;
  }

  static MathNum to_int(MathNum n) noexcept { return n.is_float ? MathNum::of(n.as_int()) : n; }
  static MathNum to_float(MathNum n) noexcept {
    return n.is_float ? n : MathNum::of(static_cast<double>(n.i));
  }

  void op(Tok what);

  // ---------------------------------------------------------------------
  // Function calls.
  // ---------------------------------------------------------------------

  // `name(args)`, as the lexer took it in.
  MathNum callmathfunc(strv text);

  // `#name`: the code of the first character of $name.
  MathNum getcvar(strv name);

  // ---------------------------------------------------------------------
  // The parser (zsh's mathparse), with its recursion on a heap stack.
  // ---------------------------------------------------------------------

  void mathparse(int top_pc);

  // zsh's mathevall: the expression from pos at precedence `pc`.
  MathNum mathevall(int pc);

  // zsh's matheval: the whole text, with nothing left over.
  MathNum matheval();

  strv                                 s;
  ParamTable&                          params;
  const MathOptions&                   opts;
  const MathFuncRegistry*              funcs;
  const s::array<s::uint8_t, T_COUNT>& prec;
  Scratch&                             scratch;
  s::size_t                            pos = 0;
  str                                  err;
  MathNum                              yyval;
  strv                                 yylval;
  Pooled<Entry>                        stack;
  int                                  level;
  int                                  lastbase = -1;
  int                                  sp     = -1;
  int                                  noeval = 0;     // > 0: parse, don't evaluate
  Tok                                  mtok   = T_EOI;
  bool                                 unary  = true;  // an operand comes next
};

MathNum Arith::eval_text(strv text) {
  if (text.empty()) return MathNum::of(s::int64_t{0});
  if (level + 1 >= MAX_LEVEL) {
    fail("math recursion limit exceeded: " + str(text));
    return {};
  }
  if (stack_low()) { fail("stack overflow"); return {}; }
  Arith sub{text, params, opts, funcs, scratch, level + 1};
  MathNum v = sub.matheval();
  if (sub.failed()) fail(s::move(sub.err));
  return v;
}

strv Arith::lval_sub(strv lval) noexcept {
  auto br = lval.find('[');
  if (br == strv::npos) return {};
  strv sub = lval.substr(br + 1);
  if (not sub.empty() and sub.back() == ']') sub.remove_suffix(1);
  return sub;
}

bool Arith::resolve(Entry& e, bool write) {
  if (e.slot != Slot::None and not (write and e.slot == Slot::Flagged)) return true;
  strv name = lval_name(e.lval);
  if (name.size() == e.lval.size()) { e.slot = Slot::Whole; return true; }
  strv sub = lval_sub(e.lval);
  if (sub == "@" or sub == "*") { e.slot = Slot::All; return true; }
  if (is_subscript_flags(sub) and opts.flagged_element) {
    if (not write) { e.slot = Slot::Flagged; return true; }
    // Through flags an associative array, like h[@], is left as it is.
    if (auto p = params.lookup(name); p and p->get().is_hash()) {
      e.slot = Slot::All;
      return true;
    }
    str flags(sub);
    for (s::size_t k = 1; k < flags.size() and flags[k] != ')'; ++k)
      flags[k] = flags[k] == 'r' ? 'i' : flags[k] == 'R' ? 'I' : flags[k];
    auto found = opts.flagged_element(str(name), flags);
    if (not found) {
      fail(found.error().message);
      return false;
    }
    e.first = e.last = s::strtoll(found->c_str(), nullptr, 10);
    e.slot = Slot::Index;
    return true;
  }
  if (auto p = params.lookup(name); p and p->get().is_hash()) {
    e.slot = Slot::Hash;
    return true;
  }
  // As zsh's getindex, a top-level comma makes a range.
  s::size_t comma = strv::npos;
  for (s::size_t k = 0, depth = 0; k < sub.size(); ++k) {
    char c = sub[k];
    if (c == '(' or c == '[') ++depth;
    else if ((c == ')' or c == ']') and depth > 0) --depth;
    else if (c == ',' and depth == 0) { comma = k; break; }
  }
  if (comma == strv::npos) {
    e.first = e.last = from_ksh(eval_index(sub), true);
    e.slot = Slot::Index;
  } else {
    e.first = from_ksh(eval_index(sub.substr(0, comma)), false);
    e.last  = from_ksh(eval_index(sub.substr(comma + 1)), false);
    e.slot = Slot::Range;
  }
  return not failed();
}

opt<str> Arith::shell_param(strv name) const {
  if (not opts.shell) return s::nullopt;
  const auto& pos_params = opts.shell->positional;
  if (name == "ARGC") return s::to_string(pos_params.size());
  if (name == "argv") {
    str text;
    for (const auto& w : pos_params) text += (text.empty() ? "" : " ") + w;
    return text;
  }
  return s::nullopt;
}

opt<str> Arith::param_text(Entry& e) {
  strv name = lval_name(e.lval);
  auto p = params.lookup(name);
  if (not p or not p->get().is_set()) {
    if (name.size() == e.lval.size()) return shell_param(name);
    return s::nullopt;
  }
  if (not resolve(e)) return str{};
  // Evaluating the subscript may have changed the table.
  p = params.lookup(name);
  if (not p or not p->get().is_set()) return s::nullopt;
  const Param& param = p->get();
  auto join = [](const vec<str>& words, s::size_t from, s::size_t to, strv sep) {
    str text;
    for (s::size_t k = from; k < to; ++k) {
      if (k > from) text += sep;
      text += words[k];
    }
    return text;
  };
  switch (e.slot) {
    case Slot::Whole: case Slot::All: {
      if (param.is_array()) return join(param.array_ref(), 0, param.array_ref().size(), " ");
      if (not param.is_hash()) return param.scalar();
      // An associative array is its values, in key order as $h gives them.
      str text;
      for (auto* kv : sorted_entries(param.hash_ref()))
        text += (text.empty() ? "" : " ") + kv->second;
      return text;
    }

    case Slot::Hash: {
      if (param.handlers and param.handlers->get_elem)
        return param.handlers->get_elem(str(lval_sub(e.lval))).value_or("");
      const auto& h = param.hash_ref();
      auto it = h.find(str(lval_sub(e.lval)));
      return it == h.end() ? str{} : it->second;
    }

    case Slot::Flagged: {
      auto r = opts.flagged_element(str(name), str(lval_sub(e.lval)));
      if (not r) {
        fail(r.error().message);
        return str{};
      }
      return s::move(*r);
    }

    case Slot::Index: case Slot::Range: {
      // zsh keeps subscripts in an int, so larger ones wrap around.
      auto wrap32 = [](s::int64_t i) {
        return static_cast<s::int64_t>(static_cast<s::int32_t>(static_cast<s::uint32_t>(i)));
      };
      s::int64_t f = wrap32(e.first), l = wrap32(e.slot == Slot::Index ? e.first : e.last);
      if (param.is_array()) {
        auto n = static_cast<s::int64_t>(param.array_ref().size());
        if (f < 0) f += n + 1;
        if (l < 0) l += n + 1;
        if (e.slot == Slot::Index and (f < 1 or f > n)) return str{};
        f = s::max<s::int64_t>(f, 1);
        l = s::min(l, n);
        if (f > l) return str{};
        return join(param.array_ref(), static_cast<s::size_t>(f - 1),
                    static_cast<s::size_t>(l), " ");
      }
      // A plain scalar is read in place; its characters are found by
      // walking it (code points under MULTIBYTE, else bytes), and counted
      // only for a negative index.
      const str* stored = param.handlers ? nullptr : s::get_if<str>(&param.value);
      str copy = stored ? str{} : param.scalar();
      strv text = stored ? strv(*stored) : strv(copy);
      auto offset = [&](strv in, s::int64_t k) {
        auto n = static_cast<s::size_t>(k);
        return opts.multibyte ? code_point_offset(in, n) : s::min(n, in.size());
      };
      if (f < 0 or l < 0) {
        auto n = static_cast<s::int64_t>(opts.multibyte ? code_point_count(text) : text.size());
        if (f < 0) f += n + 1;
        if (l < 0) l += n + 1;
      }
      if (f < 1) {
        if (e.slot == Slot::Index) return str{};
        f = 1;
      }
      if (f > l) return str{};
      auto b = offset(text, f - 1);
      auto end = b + offset(text.substr(b), l - f + 1);
      return str(text.substr(b, end - b));
    }

    default:
      return str{};
  }
}

MathNum Arith::getmathparam(Entry& e) {
  MathNum r;
  strv name = lval_name(e.lval);
  auto p = params.lookup(name);
  // A number has no elements: `i[2]` is all of i, as in zsh (the subscript
  // is still worked out, once).
  bool number = p and p->get().is_set() and (p->get().is_integer() or p->get().is_float());
  if (number and name.size() != e.lval.size()) {
    if (not resolve(e)) return {};
    p = params.lookup(name);
  }
  if (p and p->get().is_set() and p->get().is_integer()) {
    r = MathNum::of(p->get().integer());
  } else if (p and p->get().is_set() and p->get().is_float()) {
    r = MathNum::of(p->get().floatv());
  } else {
    auto text = param_text(e);
    if (failed()) return {};
    if (not text) {
      if (opts.no_unset) fail(str(e.lval) + ": parameter not set");
      return zero();
    }
    r = eval_text(*text);
  }
  if (opts.force_float and not r.is_float) r = MathNum::of(static_cast<double>(r.i));
  return r;
}

MathNum Arith::setmathvar(Entry& e, MathNum v, bool plain) {
  if (e.lval.empty()) {
    bad("lvalue required");
    return MathNum::of(s::int64_t{0});
  }
  if (noeval) return v;
  if (not resolve(e, true)) return v;
  strv name = lval_name(e.lval);
  auto p = params.lookup(name);
  if (p and p->get().is_readonly()) {
    fail("read-only variable: " + str(name));
    return v;
  }
  auto check = [&](Result<void> r) {
    if (not r) fail(r.error().message);
  };
  // A number has no elements: as in zsh, `i[2] = 9` assigns all of i.
  // `name[@]` on a scalar or a number is all of it too.
  Slot slot = e.slot;
  bool set = p and p->get().is_set();
  if ((slot == Slot::Index or slot == Slot::Range) and set
      and (p->get().is_integer() or p->get().is_float()))
    slot = Slot::Whole;
  if (slot == Slot::All and set and not p->get().is_array() and not p->get().is_hash())
    slot = Slot::Whole;
  switch (slot) {
    case Slot::Whole: {
      // An integer without a base of its own takes that of the constants.
      auto take_base = [&](Param& param) {
        if (param.is_integer() and param.base == 0 and lastbase >= 2) param.base = lastbase;
      };
      // As zsh's setnparam: an array or association.  `=` replaces an
      // ordinary one with a number; a tied or special array takes the
      // value as its one element, as does any array with `+=`, `++` …,
      // which leave an association as it is.
      if (set and (p->get().is_array() or p->get().is_hash())) {
        Param& param = p->get();
        bool special = param.handlers or (param.flags & PM_TIED);
        if (param.is_array() and (special or not plain)) {
          check(params.set_array(name, {format_math(v)}));
          return v;
        }
        if (param.is_hash() and (special or not plain)) return v;
        check(params.unset(name));
        if (failed()) return v;
        p = params.lookup(name);
      }
      if (not p or not p->get().is_set()) {
        // A new parameter takes the type of the value, as in zsh.
        if (auto r = params.set_scalar(name, {}); not r) { fail(r.error().message); return v; }
        Param& param = params.lookup(name)->get();
        if (v.is_float) param.set(v.d);
        else            param.set(v.i);
        take_base(param);
        return v;
      }
      Param& param = p->get();
      bool integer = param.is_integer(), fl = param.is_float();
      if (param.handlers)   // $SECONDS, $RANDOM, …: their setter decides
        check(params.set_scalar(name, format_math(v)));
      else if (integer) {
        param.set(v.as_int());
        take_base(param);
      }
      else if (fl)
        param.set(v.as_float());
      else
        check(params.set_scalar(name, format_math(v)));
      if (integer) return MathNum::of(v.as_int());
      if (fl)      return MathNum::of(v.as_float());
      return v;
    }

    case Slot::Hash: {
      str key(lval_sub(e.lval));
      if (not p) return v;
      Param& param = p->get();
      if (param.handlers and param.handlers->set_elem) {
        param.handlers->set_elem(key, format_math(v));
      } else if (auto* h = param.hash_ptr(); h and param.is_set()) {
        (*h)[key] = format_math(v);
      } else {
        auto h2 = param.hash();
        h2[key] = format_math(v);
        param.set(s::move(h2));
      }
      return v;
    }

    case Slot::All:
      // zsh leaves an associative array as it is.
      if (not (p and p->get().is_hash())) check(params.set_array(name, {format_math(v)}));
      return v;

    default:
      check(params.set_slice(name, e.first, e.last, {format_math(v)}, false, false,
                             opts.multibyte));
      return v;
  }
}

s::int64_t Arith::char_code() {
  bool ctl = false, meta = false;
  s::int64_t c = -1;
  while (pos < s.size() and c < 0) {
    if (s[pos] == '^' and pos + 1 < s.size()) {
      ctl = true;
      ++pos;
    } else if (s[pos] == '\\' and pos + 1 < s.size()) {
      char e = s[pos + 1];
      pos += 2;
      if ((e == 'C' or e == 'M') and pos < s.size()) {
        if (s[pos] == '-') ++pos;
        (e == 'C' ? ctl : meta) = true;
        continue;
      }
      constexpr strv FROM = "abefnrtv", TO = "\a\b\x1b\f\n\r\t\v";
      auto k = FROM.find(e);
      c = static_cast<unsigned char>(k == strv::npos ? e : TO[k]);
    } else {
      s::size_t len = 1;
      c = MB_CUR_MAX > 1 ? static_cast<s::int64_t>(utf8_decode(s, pos, len))
                         : static_cast<unsigned char>(s[pos]);
      pos += len;
    }
  }
  if (c < 0) return 0;
  if (ctl)  c = c == '?' ? 0x7f : (c & 0x9f);
  if (meta) c |= 0x80;
  return c;
}

s::size_t Arith::ident_end(s::size_t k) const noexcept {
  while (k < s.size()) {
    auto c = static_cast<unsigned char>(s[k]);
    if (c < 0x80) {
      if (not (s::isalnum(c) or c == '_')) break;
      ++k;
      continue;
    }
    if (MB_CUR_MAX <= 1) break;
    s::size_t len = 1;
    char32_t wc = utf8_decode(s, k, len);
    if (len == 1 or not s::iswalnum(static_cast<wint_t>(wc))) break;
    k += len;
  }
  return k;
}

Tok Arith::lexconstant() {
  s::size_t nptr = pos;
  if (at(nptr) == '0') {
    ++nptr;
    char low = static_cast<char>(s::tolower(static_cast<unsigned char>(at(nptr))));
    if (low == 'x' or low == 'b') {
      auto r = zstrtol(s, pos, 0, true);
      lastbase = low == 'b' ? 2 : 16;
      pos = r.end;
      yyval = integer_value(r.value);
      return T_NUM;
    }
    if (opts.octal_zeroes) {
      // A real octal constant, not a base (`010#1`) or a float.
      s::size_t p2 = nptr;
      while (is_digit(at(p2)) or at(p2) == '_') ++p2;
      char c = at(p2);
      if (p2 > nptr and c != '.' and c != 'e' and c != 'E' and c != '#') {
        auto r = zstrtol(s, pos, 0, true);
        lastbase = 8;
        pos = r.end;
        yyval = integer_value(r.value);
        return T_NUM;
      }
      nptr = p2;
    }
  }
  while (is_digit(at(nptr)) or at(nptr) == '_') ++nptr;
  if (at(nptr) == '.' or at(nptr) == 'e' or at(nptr) == 'E') {
    if (at(nptr) == '.') {
      ++nptr;
      while (is_digit(at(nptr)) or at(nptr) == '_') ++nptr;
    }
    if (at(nptr) == 'e' or at(nptr) == 'E') {
      ++nptr;
      if (at(nptr) == '+' or at(nptr) == '-') ++nptr;
      while (is_digit(at(nptr)) or at(nptr) == '_') ++nptr;
    }
    str text;
    for (s::size_t k = pos; k < nptr; ++k)
      if (s[k] != '_') text += s[k];
    char* end = nullptr;
    double d = s::strtod(text.c_str(), &end);
    auto used = static_cast<s::size_t>(end - text.c_str());
    // Back to a position in s, past the underscores strtod didn't see.
    s::size_t k = pos;
    for (s::size_t n = 0; n < used; ++k)
      if (s[k] != '_') ++n;
    if (used == 0 or at(k) == '.') {
      fail("bad floating point constant");
      return T_EOI;
    }
    pos = k;
    yyval = MathNum::of(d);
    return T_NUM;
  }
  auto r = zstrtol(s, pos, 10, true);
  pos = r.end;
  if (at(pos) == '#') {
    ++pos;
    auto base = static_cast<int>(r.value);
    lastbase = base;
    r = zstrtol(s, pos, base, true);
    if (r.bad_base) {
      fail("invalid base (must be 2 to 36 inclusive): " + s::to_string(base));
      return T_NUM;
    }
    pos = r.end;
  }
  yyval = integer_value(r.value);
  return T_NUM;
}

Tok Arith::lex() {
  yyval = MathNum::of(s::int64_t{0});
  auto next_is = [&](char c) {
    if (at(pos) != c) return false;
    ++pos;
    return true;
  };
  for (;;) {
    if (pos >= s.size()) return T_EOI;
    char c = s[pos++];
    switch (c) {
      case '+':
        if (next_is('+')) return unary ? T_PREPLUS : T_POSTPLUS;
        if (next_is('=')) return T_PLUSEQ;
        return unary ? T_UPLUS : T_PLUS;

      case '-':
        if (next_is('-')) return unary ? T_PREMINUS : T_POSTMINUS;
        if (next_is('=')) return T_MINUSEQ;
        if (not unary) return T_MINUS;
        // A negative constant: `-2 ** 2` is 4.
        if (is_digit(at(pos)) or at(pos) == '.') {
          Tok t = lexconstant();
          if (t == T_NUM) {
            if (yyval.is_float) yyval.d = -yyval.d;
            else                yyval.i = wneg(yyval.i);
          }
          return t;
        }
        return T_UMINUS;

      case '(': return T_INPAR;
      case ')': return T_OUTPAR;
      case '!': return next_is('=') ? T_NEQ : T_NOT;
      case '~': return T_COMP;

      case '&':
        if (next_is('&')) return next_is('=') ? T_DANDEQ : T_DAND;
        return next_is('=') ? T_ANDEQ : T_AND;

      case '|':
        if (next_is('|')) return next_is('=') ? T_DOREQ : T_DOR;
        return next_is('=') ? T_OREQ : T_OR;

      case '^':
        if (next_is('^')) return next_is('=') ? T_DXOREQ : T_DXOR;
        return next_is('=') ? T_XOREQ : T_XOR;

      case '*':
        if (next_is('*')) return next_is('=') ? T_POWEREQ : T_POWER;
        return next_is('=') ? T_MULEQ : T_MUL;

      case '/': return next_is('=') ? T_DIVEQ : T_DIV;
      case '%': return next_is('=') ? T_MODEQ : T_MOD;

      case '<':
        if (next_is('<')) return next_is('=') ? T_SHLEFTEQ : T_SHLEFT;
        return next_is('=') ? T_LEQ : T_LES;

      case '>':
        if (next_is('>')) return next_is('=') ? T_SHRIGHTEQ : T_SHRIGHT;
        return next_is('=') ? T_GEQ : T_GRE;

      case '=': return next_is('=') ? T_DEQ : T_EQ;

      case '$':
        yyval = MathNum::of(static_cast<s::int64_t>(opts.shell ? opts.shell->shell_pid : 0));
        return T_NUM;

      case '?':
        if (not unary) return T_QUEST;
        yyval = MathNum::of(static_cast<s::int64_t>(opts.shell ? opts.shell->last_exit : 0));
        return T_NUM;

      case ':': return T_COLON;
      case ',': return T_COMMA;

      // POSIX says to ignore double quotes.
      case ' ': case '\t': case '\n': case '"':

      break; default: {
        --pos;
        if (is_digit(c) or c == '.') return lexconstant();
        bool cct = false;
        if (c == '#') {
          ++pos;
          if (at(pos) == '\\' or at(pos) == '#') {
            ++pos;
            if (pos >= s.size()) {
              bad("character missing after ##");
              return T_EOI;
            }
            yyval = MathNum::of(char_code());
            return T_NUM;
          }
          cct = true;
        }
        s::size_t p = pos, ie = ident_end(pos);
        if (ie == p) {
          if (not cct) return T_EOI;   // an illegal character, left at pos
          yyval = MathNum::of(static_cast<s::int64_t>(
              opts.shell ? opts.shell->positional.size() : 0));
          return T_NUM;
        }
        pos = ie;
        if (ie - p == 3) {
          auto lower = [&](s::size_t k) {
            return static_cast<char>(s::tolower(static_cast<unsigned char>(s[k])));
          };
          if (lower(p) == 'n' and lower(p + 1) == 'a' and lower(p + 2) == 'n') {
            yyval = MathNum::of(s::numeric_limits<double>::quiet_NaN());
            return T_NUM;
          }
          if (lower(p) == 'i' and lower(p + 1) == 'n' and lower(p + 2) == 'f') {
            yyval = MathNum::of(s::numeric_limits<double>::infinity());
            return T_NUM;
          }
        }
        bool func = false;
        if (at(pos) == '[' or (not cct and at(pos) == '(')) {
          char open = s[pos], close = open == '[' ? ']' : ')';
          func = open == '(';
          int depth = 1;
          for (++pos; pos < s.size() and depth; ++pos) {
            if (s[pos] == open) ++depth;
            if (s[pos] == close) --depth;
            if (s[pos] == '\\' and pos + 1 < s.size()) ++pos;
          }
        }
        yylval = s.substr(p, pos - p);
        return func ? T_FUNC : cct ? T_CID : T_ID;
      }
    }
  }
}

void Arith::checkunary(Tok tok, s::size_t optr) {
  s::uint16_t tp = TYPE[tok];
  int errmsg = 0;
  if (tp & (BINARY | OP_OP)) {
    if (unary) errmsg = 1;
  } else if (not unary) {
    errmsg = 2;
  }
  if (errmsg) {
    strv kind = errmsg == 2 ? "operator" : "operand";
    while (optr < s.size() and is_blank(s[optr])) ++optr;
    if (optr >= s.size()) {
      bad(str(kind) + " expected at end of string");
    } else {
      // At most 10 characters of what is there.
      s::size_t end = optr, len = 1;
      int chars = 0;
      for (; end < s.size() and chars < 10; end += len, ++chars) (void)utf8_decode(s, end, len);
      bad(str(kind) + " expected at `" + str(s.substr(optr, end - optr))
          + (end < s.size() ? "...'" : "'"));
    }
  }
  unary = not (tp & OP_OPF);
}

void Arith::push(MathNum val, strv lval, bool getme) {
  if (sp == STACK_SIZE - 1) {
    fail("stack overflow");
    return;
  }
  ++sp;
  if (static_cast<s::size_t>(sp) == stack.v.size()) stack.v.emplace_back();
  Entry& e = stack.v[static_cast<s::size_t>(sp)];
  e = Entry{val, lval};
  e.unset = getme;
}

MathNum Arith::pop(bool noget) {
  Entry& e = stack.v[static_cast<s::size_t>(sp)];
  if (e.unset and not noget) {
    e.val = getmathparam(e);
    e.unset = false;
  }
  --sp;
  return failed() ? MathNum{} : e.val;
}

MathNum& Arith::top_value() {
  Entry& e = stack.v[static_cast<s::size_t>(sp)];
  if (e.unset) {
    e.val = getmathparam(e);
    e.unset = false;
  }
  return e.val;
}

void Arith::bop(Tok tk) {
  if (sp < 0) return;
  MathNum v = top_value();
  s::int64_t tst = v.is_float ? MathNum::of(v.d).as_int() : v.i;
  if ((tk == T_DAND or tk == T_DANDEQ) and not tst) ++noeval;
  if ((tk == T_DOR or tk == T_DOREQ) and tst) ++noeval;
}

void Arith::op(Tok what) {
  if (failed()) return;
  if (sp < 0) {
    bad("stack empty");
    return;
  }
  s::uint16_t tp = TYPE[what];
  if (tp & BINARY) {
    if (sp < 1) {
      bad("stack empty");
      return;
    }
    MathNum b = pop(false);
    MathNum a = pop(what == T_EQ);
    if (failed()) return;
    bool a_unset = stack.v[static_cast<s::size_t>(sp + 1)].unset;
    if (tp & (OP_A2IO | OP_E2IO)) {
      a = to_int(a);
      b = to_int(b);
    } else if (a.is_float != b.is_float and what != T_COMMA
               and not (a_unset and what == T_EQ)) {
      a = to_float(a);
      b = to_float(b);
    }
    MathNum c = MathNum::of(s::int64_t{0});
    if (not noeval) {
      bool fl = (tp & OP_A2IR) ? false : a.is_float;
      auto num = [&](s::int64_t i, double d) { return fl ? MathNum::of(d) : MathNum::of(i); };
      auto cmp = [&](auto f) {
        return MathNum::of(static_cast<s::int64_t>(a.is_float ? f(a.d, b.d) : f(a.i, b.i)));
      };
      switch (what) {
        case T_AND: case T_ANDEQ: c = MathNum::of(a.i & b.i);

        break; case T_XOR: case T_XOREQ: c = MathNum::of(a.i ^ b.i);

        break; case T_OR: case T_OREQ: c = MathNum::of(a.i | b.i);

        break; case T_MUL: case T_MULEQ: c = num(wmul(a.i, b.i), a.d * b.d);

        break; case T_DIV: case T_DIVEQ:
          if (not notzero(b)) return;
          // INT64_MIN / -1 overflows; zsh negates instead.
          if (fl) c = MathNum::of(a.d / b.d);
          else    c = MathNum::of(b.i == -1 ? wneg(a.i) : a.i / b.i);

        break; case T_MOD: case T_MODEQ:
          if (not notzero(b)) return;
          if (fl) c = MathNum::of(s::fmod(a.d, b.d));
          else    c = MathNum::of(b.i == -1 ? s::int64_t{0} : a.i % b.i);

        break; case T_PLUS: case T_PLUSEQ: c = num(wadd(a.i, b.i), a.d + b.d);

        break; case T_MINUS: case T_MINUSEQ: c = num(wsub(a.i, b.i), a.d - b.d);

        break; case T_SHLEFT: case T_SHLEFTEQ: c = MathNum::of(wshl(a.i, b.i));

        break; case T_SHRIGHT: case T_SHRIGHTEQ: c = MathNum::of(wshr(a.i, b.i));

        break; case T_LES: c = cmp([](auto x, auto y) { return x < y; });

        break; case T_LEQ: c = cmp([](auto x, auto y) { return x <= y; });

        break; case T_GRE: c = cmp([](auto x, auto y) { return x > y; });

        break; case T_GEQ: c = cmp([](auto x, auto y) { return x >= y; });

        break; case T_DEQ: c = cmp([](auto x, auto y) { return x == y; });

        break; case T_NEQ: c = cmp([](auto x, auto y) { return x != y; });

        break; case T_DAND: case T_DANDEQ:
          c = MathNum::of(static_cast<s::int64_t>(a.i and b.i));

        break; case T_DOR: case T_DOREQ:
          c = MathNum::of(static_cast<s::int64_t>(a.i or b.i));

        break; case T_DXOR: case T_DXOREQ:
          c = MathNum::of(static_cast<s::int64_t>((a.i != 0) != (b.i != 0)));

        break; case T_COMMA: case T_EQ: c = b;

        break; case T_POWER: case T_POWEREQ:
          if (not fl and b.i < 0) {   // a real result
            fl = true;
            a = to_float(a);
            b = to_float(b);
          }
          if (not fl) {
            c = MathNum::of(ipow(a.i, b.i));
          } else {
            if (a.d < 0 and static_cast<double>(MathNum::of(b.d).as_int()) != b.d) {
              bad("imaginary power");
              return;
            }
            c = MathNum::of(s::pow(a.d, b.d));
          }

        break; default:
          assert(false); // should never get here
      }
    }
    if (tp & (OP_E2 | OP_E2IO)) {
      Entry& lv = stack.v[static_cast<s::size_t>(sp + 1)];
      c = setmathvar(lv, c, what == T_EQ);
      // The result is still the lvalue, which keeps what it resolved to.
      Entry kept = lv;
      push(c, kept.lval, false);
      if (sp >= 0 and not failed()) {
        Entry& e = stack.v[static_cast<s::size_t>(sp)];
        e.first = kept.first;
        e.last  = kept.last;
        e.slot  = kept.slot;
      }
    } else {
      push(c, {}, false);
    }
    return;
  }

  Entry& e = stack.v[static_cast<s::size_t>(sp)];
  MathNum& v = top_value();
  switch (what) {
    case T_NOT:
      v = MathNum::of(static_cast<s::int64_t>(not v.truthy()));

    break; case T_COMP:
      v = MathNum::of(~v.as_int());

    break; case T_POSTPLUS: case T_POSTMINUS: {
      int step = what == T_POSTPLUS ? 1 : -1;
      MathNum n = v.is_float ? MathNum::of(v.d + step) : MathNum::of(wadd(v.i, step));
      (void)setmathvar(e, n);
    }

    break; case T_UPLUS:

    break; case T_UMINUS:
      v = v.is_float ? MathNum::of(-v.d) : MathNum::of(wneg(v.i));

    break; case T_QUEST: {
      if (sp < 2) {
        bad("stack empty");
        return;
      }
      MathNum c = pop(false);
      MathNum b = pop(false);
      MathNum a = pop(false);
      if (failed()) return;
      push(a.truthy() ? b : c, {}, false);
      return;
    }

    break; case T_COLON:
      bad("':' without '?'");
      return;

    break; case T_PREPLUS: case T_PREMINUS: {
      int step = what == T_PREPLUS ? 1 : -1;
      v = v.is_float ? MathNum::of(v.d + step) : MathNum::of(wadd(v.i, step));
      (void)setmathvar(e, v);
    }

    break; default:
      bad("out of integers");
      return;
  }
  // A unary operator's result is no lvalue.
  stack.v[static_cast<s::size_t>(sp)].lval = {};
  stack.v[static_cast<s::size_t>(sp)].slot = Slot::None;
}

MathNum Arith::callmathfunc(strv text) {
  auto paren = text.find('(');
  strv name = text.substr(0, paren);
  strv args = text.substr(paren + 1);
  // zsh drops the last character, taking it to be the `)`.
  if (not args.empty()) args.remove_suffix(1);
  const MathFuncDef* def = funcs ? funcs->find(name) : nullptr;
  if (not def) {
    fail("unknown function: " + str(name));
    return {};
  }
  vec<MathNum> argv;
  s::size_t a = 0;
  while (a < args.size() and is_blank(args[a])) ++a;
  while (a < args.size()) {
    if (level + 1 >= MAX_LEVEL) {
      fail("math recursion limit exceeded: " + str(args.substr(a)));
      return {};
    }
    Arith sub{args, params, opts, funcs, scratch, level + 1};
    sub.pos = a;
    argv.push_back(sub.mathevall(sub.arg_prec()));
    if (sub.failed()) {
      fail(s::move(sub.err));
      return {};
    }
    a = sub.pos;
    if (sub.mtok != T_COMMA) break;
  }
  if (a < args.size()) {
    bad("illegal character: " + nice_char(args[a]));
    return {};
  }
  auto argc = static_cast<int>(argv.size());
  if (argc < def->min_args or (def->max_args >= 0 and argc > def->max_args)) {
    fail("wrong number of arguments: " + str(text));
    return {};
  }
  auto r = def->fn(argv);
  if (not r) {
    fail(r.error().message);
    return {};
  }
  return *r;
}

MathNum Arith::getcvar(strv name) {
  Entry e{{}, name};
  auto text = param_text(e);
  if (not text or text->empty()) return MathNum::of(s::int64_t{0});
  s::size_t len = 1;
  return MathNum::of(MB_CUR_MAX > 1
      ? static_cast<s::int64_t>(utf8_decode(*text, 0, len))
      : static_cast<s::int64_t>(static_cast<unsigned char>((*text)[0])));
}

void Arith::mathparse(int top_pc) {
  enum class Step : s::uint8_t { Start, Loop, Next, Return };
  Pooled<Frame> frames{scratch.frames};
  frames.v.push_back({top_pc});
  Step step = Step::Start;
  auto call = [&](int pc) {
    frames.v.push_back({pc});
    step = Step::Start;
  };
  for (;;) {
    switch (step) {
      case Step::Start: {
        if (failed()) {
          step = Step::Return;
          break;
        }
        s::size_t optr = pos;
        mtok = lex();
        // An empty expression (or one starting with an illegal
        // character, which the caller reports).
        if (frames.v.back().pc == top_prec() and mtok == T_EOI) {
          step = Step::Return;
          break;
        }
        checkunary(mtok, optr);
        step = Step::Loop;
      }

      break; case Step::Loop: {
        Frame& f = frames.v.back();
        if (prec[mtok] > f.pc or failed()) {
          step = Step::Return;
          break;
        }
        step = Step::Next;
        switch (mtok) {
          case T_NUM:
            push(yyval, {}, false);

          break; case T_ID:
            push(MathNum::of(s::int64_t{0}), yylval, not noeval);

          break; case T_CID:
            push(noeval ? MathNum::of(s::int64_t{0}) : getcvar(yylval), yylval, false);

          break; case T_FUNC:
            push(noeval ? MathNum::of(s::int64_t{0}) : callmathfunc(yylval), {}, false);

          break; case T_INPAR:
            f.cont = Cont::Paren;
            call(top_prec());

          break; case T_QUEST:
            if (sp < 0) {
              bad("stack empty");
              step = Step::Return;
              break;
            }
            f.q = top_value().truthy();
            if (not f.q) ++noeval;
            f.cont = Cont::QuestMid;
            call(prec[T_COLON] - 1);

          break; default: {
            f.otok = mtok;
            f.onoeval = noeval;
            s::uint16_t tp = TYPE[mtok];
            if (mtype(tp) == BOOL) bop(mtok);
            f.cont = Cont::Binary;
            call(prec[mtok] - (mtype(tp) != RL ? 1 : 0));
          }
        }
      }

      break; case Step::Next: {
        s::size_t optr = pos;
        mtok = lex();
        checkunary(mtok, optr);
        step = Step::Loop;
      }

      break; case Step::Return: {
        frames.v.pop_back();
        if (frames.v.empty()) return;
        Frame& f = frames.v.back();
        step = Step::Loop;
        switch (f.cont) {
          case Cont::Paren:
            if (mtok != T_OUTPAR) {
              if (not failed()) bad("')' expected");
              step = Step::Return;
            } else {
              step = Step::Next;
            }

          break; case Cont::QuestMid:
            if (not f.q) --noeval;
            if (mtok != T_COLON) {
              if (not failed()) bad("':' expected");
              step = Step::Return;
              break;
            }
            if (f.q) ++noeval;
            f.cont = Cont::QuestElse;
            call(prec[T_QUEST]);

          break; case Cont::QuestElse:
            if (f.q) --noeval;
            op(T_QUEST);

          break; case Cont::Binary:
            noeval = f.onoeval;
            op(f.otok);

          break; default:
            assert(false); // should never get here
        }
      }

      break; default:
        assert(false); // should never get here
    }
  }
}

MathNum Arith::mathevall(int pc) {
  mathparse(pc);
  // Parentheses are parsed at top precedence, so one too many ends up
  // here.
  if (mtok == T_OUTPAR and not failed()) bad("unexpected ')'");
  if (failed() or sp < 0) return MathNum::of(s::int64_t{0});
  Entry& e = stack.v[0];
  if (e.unset) {
    e.val = getmathparam(e);
    e.unset = false;
  }
  return failed() ? MathNum{} : e.val;
}

MathNum Arith::matheval() {
  if (s.empty()) return MathNum::of(s::int64_t{0});
  MathNum v = mathevall(top_prec());
  if (pos < s.size() and not failed()) bad("illegal character: " + nice_char(s[pos]));
  return v;
}

}  // namespace

ParsedLong zstrtol(strv s, s::size_t pos, int base, bool underscore) {
  auto at = [&](s::size_t k) { return k < s.size() ? s[k] : '\0'; };
  ParsedLong r;
  while (is_blank(at(pos))) ++pos;
  bool neg = at(pos) == '-';
  if (neg or at(pos) == '+') ++pos;
  if (base == 0) {
    if (at(pos) != '0') {
      base = 10;
    } else if (at(++pos) == 'x' or at(pos) == 'X') {
      base = 16;
      ++pos;
    } else if (at(pos) == 'b' or at(pos) == 'B') {
      base = 2;
      ++pos;
    } else {
      base = 8;
    }
  }
  r.base = base;
  r.end  = pos;
  if (base < 2 or base > 36) {
    r.bad_base = true;
    return r;
  }
  s::size_t inp = pos, trunc = strv::npos;
  U calc = 0;
  for (; pos < s.size(); ++pos) {
    char c = s[pos];
    int d = is_digit(c) ? c - '0'
          : base > 10 and c >= 'a' and c < 'a' + base - 10 ? c - 'a' + 10
          : base > 10 and c >= 'A' and c < 'A' + base - 10 ? c - 'A' + 10
          : -1;
    if (d < 0 or d >= base) {
      if (underscore and c == '_') continue;
      break;
    }
    if (trunc != strv::npos) continue;
    // zsh's check for overflow, which misses some.
    U next = calc * static_cast<U>(base) + static_cast<U>(d);
    if (next < calc) {
      trunc = pos;
      continue;
    }
    calc = next;
  }
  // Too long for a signed number: drop the last digit (but the most
  // negative number fits).
  constexpr U TOP = U{1} << 63;
  if (trunc == strv::npos and (calc & TOP) and (not neg or (calc & ~TOP))) {
    trunc = pos - 1;
    calc /= static_cast<U>(base);
  }
  if (trunc != strv::npos)
    s::fprintf(stderr, "jfsh: number truncated after %zu digits: %.*s\n", trunc - inp,
               static_cast<int>(s.size() - inp), s.data() + inp);
  r.value = neg ? wneg(wrap(calc)) : wrap(calc);
  r.end   = pos;
  return r;
}

Result<MathNum> math_eval(strv expr, ParamTable& params, const MathOptions& opts,
                          const MathFuncRegistry* funcs) {
  thread_local Scratch scratch;
  Arith a{expr, params, opts, funcs, scratch, 0};
  MathNum v = a.matheval();
  if (opts.last_base) *opts.last_base = a.lastbase;
  if (a.failed())
    return s::unexpected(ShellError{ErrorKind::BadSubstitution, s::move(a.err)});
  return v;
}

Result<s::int64_t> arith_eval(strv expr, ParamTable& params, const MathOptions& opts,
                              const MathFuncRegistry* funcs) {
  auto v = math_eval(expr, params, opts, funcs);
  if (not v) return s::unexpected(v.error());
  return v->as_int();
}

}  // namespace jfsh
