#ifndef JFSH_BUILTINS_MATH_H
#define JFSH_BUILTINS_MATH_H
// jfsh/builtins/math.h — integer arithmetic for $((…)), `let` and (( )).
//
// Mirrors zsh's math.c, integer part only.  Operators, by zsh's precedence
// (not C's: bitwise operators bind tighter than comparisons):
//   unary + - ! ~ ++ --, postfix ++ --
//   **  (right-assoc)
//   * / %
//   + -
//   << >>
//   &
//   ^
//   |
//   < <= > >=
//   == !=
//   &&
//   || ^^
//   ?:
//   = += -= *= /= %= **= <<= >>= &= ^= |= &&= ||=
//   ,
// Names evaluate without `$`; an unset name is 0, and a name holding an
// expression is evaluated recursively.  Literals: decimal, 0x hex, 0b
// binary and zsh's base#digits.

#include <string_view>

#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

Result<long long> arith_eval(std::string_view expr, ParamTable& params);

}  // namespace jfsh

#endif  // JFSH_BUILTINS_MATH_H
