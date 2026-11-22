#ifndef JFSH_BUILTINS_MATH_H
#define JFSH_BUILTINS_MATH_H
// jfsh/builtins/math.h — arithmetic for $((…)), (( )), `let`, subscripts
// and `[[ … -eq … ]]`.
//
// A port of zsh's math.c: an operator-precedence parser that evaluates as
// it parses, with zsh's value stack (100 entries, "stack overflow" past
// that) and its recursion limit (256 nested evaluations of parameter
// values, subscripts and function arguments).  The parser itself keeps its
// pending operators on a heap stack, so nesting depth is not bounded by
// the C++ stack.
//
// Values are 64-bit integers or doubles: a float literal (`1.5`, `1e3`,
// `.5`), a float parameter, `Inf`/`NaN`, or an integer `**` a negative
// exponent makes the result a float, and an operator with a float operand
// gives a float (comparisons give integers; the bitwise and logical
// operators truncate floats to integers).  Integers wrap around on
// overflow.
//
// Operators, binding tightest first, by zsh's default precedence:
//   unary + - ! ~ ++ --, postfix ++ --
//   << >>
//   &
//   ^
//   |
//   **  (right-assoc)
//   * / %
//   + -
//   < <= > >=
//   == !=
//   &&
//   || ^^
//   ?:
//   = += -= *= /= %= **= <<= >>= &= ^= |= &&= ||= ^^=
//   ,
// With C_PRECEDENCES the binary operators follow C instead:
//   ** > * / % > + - > << >> > < <= > >= > == != > & > ^ > | > && > ^^ > ||
// As in zsh, `^^=` computes `a ^^ b` without assigning it.
//
// Names evaluate without `$`, and `name[sub]` indexes an array (or the
// characters of a scalar, `name[i,j]` a range) or an associative array;
// an unset or empty name is 0, and a name holding an expression is
// evaluated recursively.  `$` is $$, `?` in operand position is $?, `#`
// alone is $#, `#name` the code of $name's first character and `##c` the
// code of c.  Literals: decimal, 0x hex, 0b binary, zsh's base#digits
// (0#… picks the base from a 0, 0x or 0b prefix), `0` octal under
// OCTAL_ZEROES, with `_` allowed between digits.  FORCE_FLOAT makes
// integer constants floats.

#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct ExecContext;
struct MathFuncRegistry;  // forward — defined in jfsh/modules/module.h

// zsh's mnumber.
struct MathNum {
  bool       is_float = false;
  s::int64_t i        = 0;
  double     d        = 0.0;

  static MathNum of(s::int64_t v) noexcept { return {false, v, 0.0}; }
  static MathNum of(double v)     noexcept { return {true, 0, v}; }

  // Floats truncate toward zero, saturating at the integer range.
  s::int64_t as_int()   const noexcept;
  double     as_float() const noexcept { return is_float ? d : static_cast<double>(i); }
  bool       truthy()   const noexcept { return is_float ? d != 0.0 : i != 0; }
};

struct MathOptions {
  // Reads `name[(flags)…]`, a subscript with flags, through the expander
  // (words[(I)-x] in (( … ))), failing as the expander does; without it
  // such a subscript is arithmetic.
  func<Result<str>(const str& name, const str& sub)> flagged_element;
  // The shell whose $?, $$, $# and $ARGC/$argv `?`, `$`, `#` and those
  // names read; without one they are 0 (and the names unset).
  const ExecContext* shell = nullptr;
  // Gets zsh's lastbase: the base of the last constant written with one
  // (`16#ff`, `0x10`, an OCTAL_ZEROES `010`), else -1.  An integer
  // parameter with no base of its own takes it when assigned.
  int* last_base = nullptr;
  bool force_float        = false;   // FORCE_FLOAT
  bool octal_zeroes       = false;   // OCTAL_ZEROES
  bool c_precedences      = false;   // C_PRECEDENCES
  bool ksh_arrays         = false;   // KSH_ARRAYS: 0-based subscripts
  bool ksh_zero_subscript = false;   // KSH_ZERO_SUBSCRIPT: $a[0] = $a[1]
  bool no_unset           = false;   // NO_UNSET: an unset name is an error
  bool multibyte          = true;    // MULTIBYTE: subscripts count code points
};

MathOptions math_options(const OptionState& opts) noexcept;
// The options of `ctx`, with `shell` set to it.
MathOptions math_options(const ExecContext& ctx) noexcept;

// `funcs` is the loaded modules' math function registry (zsh/mathfunc,
// zsh/datetime, …).  Pass nullptr when the caller has no shell context.
Result<MathNum>    math_eval (strv expr, ParamTable& params, const MathOptions& opts = {},
                              const MathFuncRegistry* funcs = nullptr);
// math_eval converted to an integer, for counts, indexes and `typeset -i`.
Result<s::int64_t> arith_eval(strv expr, ParamTable& params, const MathOptions& opts = {},
                              const MathFuncRegistry* funcs = nullptr);

// The text of a result as $((…)) prints it: an integer in decimal, a float
// with 17 significant digits and always a `.` or exponent (`10.`, `1.5`,
// `1e+20`), or `Inf`, `-Inf`, `NaN`.
str format_math(const MathNum& n);

// Whether `sub` starts with subscript flags, `(…)` holding only zsh's
// flag letters (w p f r R k K i I e, and s n b with a delimited argument);
// any other `(` starts an arithmetic index (`a[(1+1)]`).
bool is_subscript_flags(strv sub) noexcept;

// zsh's zstrtol: from `pos`, blanks and a sign, then digits in `base` (0
// picks 8, 16 or 2 from a 0, 0x or 0b prefix, else 10), with `_` between
// them when `underscore`.  A number too long for 64 bits keeps the digits
// that fit, with zsh's warning, which shows `text` from the first digit
// on.  `end` is where the digits stop.  A base outside 2..36 reads
// nothing and sets `bad_base` (with `base` the one asked for).
struct ParsedLong {
  s::int64_t value    = 0;
  s::size_t  end      = 0;
  int        base     = 10;
  bool       bad_base = false;
};
ParsedLong zstrtol(strv text, s::size_t pos, int base, bool underscore = false);

}  // namespace jfsh

#endif  // JFSH_BUILTINS_MATH_H
