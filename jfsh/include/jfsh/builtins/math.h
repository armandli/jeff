#ifndef JFSH_BUILTINS_MATH_H
#define JFSH_BUILTINS_MATH_H
// jfsh/builtins/math.h — arithmetic for $((…)), (( )), `let`, subscripts
// and `[[ … -eq … ]]`.
//
// Mirrors zsh's math.c.  Values are 64-bit integers or doubles: a float
// literal (`1.5`, `1e3`, `.5`), a float parameter, `Inf`/`NaN`, or an
// integer `**` a negative exponent makes the result a float, and an
// operator with a float operand gives a float (comparisons and the bitwise
// operators give integers).  Integers wrap around on overflow.
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
//
// Names evaluate without `$`, and `name[i]` indexes an array or
// associative array; an unset or empty name is 0, and a name holding an
// expression is evaluated recursively.  Literals: decimal, 0x hex, 0b
// binary, zsh's base#digits, `0` octal under OCTAL_ZEROES, with `_`
// allowed between digits.  FORCE_FLOAT makes integer constants floats.

#include <string>
#include <string_view>

#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

// zsh's mnumber.
struct MathNum {
  bool      is_float = false;
  long long i        = 0;
  double    d        = 0.0;

  static MathNum of(long long v) noexcept { return {false, v, 0.0}; }
  static MathNum of(double v)    noexcept { return {true, 0, v}; }

  // Floats truncate toward zero, saturating at the integer range.
  long long as_int()   const noexcept;
  double    as_float() const noexcept { return is_float ? d : static_cast<double>(i); }
  bool      truthy()   const noexcept { return is_float ? d != 0.0 : i != 0; }
};

struct MathOptions {
  bool force_float   = false;   // FORCE_FLOAT
  bool octal_zeroes  = false;   // OCTAL_ZEROES
  bool c_precedences = false;   // C_PRECEDENCES
};

MathOptions math_options(const OptionState& opts) noexcept;

Result<MathNum>   math_eval (std::string_view expr, ParamTable& params, MathOptions opts = {});
// math_eval converted to an integer, for counts, indexes and `typeset -i`.
Result<long long> arith_eval(std::string_view expr, ParamTable& params, MathOptions opts = {});

// The text of a result as $((…)) prints it: an integer in decimal, a float
// with 17 significant digits and always a `.` or exponent (`10.`, `1.5`,
// `1e+20`), or `Inf`, `-Inf`, `NaN`.
std::string format_math(const MathNum& n);

}  // namespace jfsh

#endif  // JFSH_BUILTINS_MATH_H
