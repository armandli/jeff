#include <jfsh/builtins/builtins.hh>
// (( )) arithmetic expression evaluator.
//
// TODO(math/parser): Implement a Pratt/precedence-climbing parser for
//   zsh arithmetic expressions.  Operator precedence (high to low):
//     unary: + - ~ !
//     ** (right-assoc)
//     * / %
//     + -
//     << >>
//     < <= > >=
//     == !=
//     &  ^  |
//     &&  ||
//     ?: (ternary)
//     = += -= *= /= %= <<= >>= &= ^= |=  (assignment)
//     ,  (comma sequence)
//
// TODO(math/vars): Variable references inside (( )) expand without $.
//   Reading an unset variable returns 0 (not error when UNSET option is on).
//   Assignment operators must write back to the shell parameter table.
//
// TODO(math/bases): Support numeric literals in different bases:
//   0x1F (hex), 0777 (octal, only when OCTAL_ZEROES is set), 2#1010 (base#n).
//
// TODO(math/incr): Implement ++ and -- (prefix and postfix).
//
// TODO(math/let): Wire this evaluator into builtin_let() and exec_arith().
//   `let expr` returns 0 if result ≠ 0, 1 if result == 0.
//   `(( expr ))` as a compound command has the same return convention.
//
// TODO(math/float): When FORCE_FLOAT option is set, use double arithmetic
//   and expose math functions (sin, cos, sqrt, etc.) via zsh mathfunc module.

namespace jfsh { } // namespace jfsh
