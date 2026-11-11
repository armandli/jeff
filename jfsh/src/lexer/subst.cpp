#include <jfsh/lexer/lexer.hh>

// Parameter, command, and arithmetic substitution expansion.
//
// TODO(subst/param): Implement $var and ${...} expansion.
//   Forms to handle (mirrors zsh parse.c / subst.c):
//     $name             simple scalar lookup
//     ${name}           same with explicit delimiters
//     ${name:-word}     default if unset/null
//     ${name:=word}     assign default if unset/null
//     ${name:?word}     error if unset/null
//     ${name:+word}     word if set/non-null
//     ${#name}          string length / array count
//     ${name#pat}       strip shortest prefix matching pat
//     ${name##pat}      strip longest prefix matching pat
//     ${name%pat}       strip shortest suffix
//     ${name%%pat}      strip longest suffix
//     ${name/pat/repl}  first substitution
//     ${name//pat/repl} global substitution
//     ${name[idx]}      array element
//     ${name[@]}        all array elements
//     ${name[*]}        all array elements (IFS-joined)
//     ${(flags)name}    zsh parameter flags (U, L, q, Q, etc.)
//
// TODO(subst/cmd): Implement $(cmd) and `cmd` command substitution.
//   Fork a subshell, capture stdout via pipe, strip trailing newlines.
//   Also handle process substitution: <(cmd) >(cmd) =(cmd).
//
// TODO(subst/arith): Implement $((expr)) arithmetic substitution.
//   Delegate to the arithmetic evaluator in builtins/math.cpp.
//   Return the numeric result as a string.
//
// TODO(subst/tilde): Implement ~ and ~user tilde expansion.
//   ~ → $HOME; ~user → getpwnam(user)->pw_dir.
//   Also ~+ (PWD) and ~- (OLDPWD).
//
// TODO(subst/wordsplit): Implement IFS word splitting on expansion results.
//   Only applies to unquoted expansions; suppressed by double-quoting.
//
// TODO(subst/glob): Integrate glob_expand after word splitting.

namespace jfsh {
}  // namespace jfsh
