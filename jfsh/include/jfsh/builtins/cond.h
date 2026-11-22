#ifndef JFSH_BUILTINS_COND_H
#define JFSH_BUILTINS_COND_H
// jfsh/builtins/cond.h — the tests shared by `[[ … ]]` and `test` / `[`.
//
// Mirrors zsh's cond.c.  The executor parses nothing here: `[[ ]]` arrives
// as a WC_COND tree (src/exec/executor.cpp expands the operands and does
// the pattern matching), and `test` (src/builtins/cond.cpp) is zsh's
// bin_test, which parses its arguments as par_cond does under testlex.
// Every function returns a cond status:
//   0 true, 1 false, 2 an error (unknown condition, bad integer), and
//   3 `-o` with a name that is no option.
// Errors are printed with `who` in front ("test: ", or "" for `[[ ]]`).

#include <string>
#include <string_view>

#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>

namespace jfsh {

struct ExecContext;

// -a -b -c -d -e -f -g -h -k -n -o -p -r -s -t -u -v -w -x -z -L -O -G -S -N
bool cond_is_unary(strv op) noexcept;
bool cond_is_numeric(strv op) noexcept;   // -eq … -ge

// `-v name[sub]` and `-v 1` ask ${+…}; `-t` takes an arithmetic expression;
// `-o x` with one character is an option letter.
int cond_unary(strv op, const str& arg, ExecContext& ctx,
               strv who);

// The file comparisons -nt -ot -ef; false unless both files exist.
int cond_file_binary(strv op, const str& l, const str& r);

// `l =~ re` with a POSIX extended regex.  On a match, sets $MATCH, $MBEGIN,
// $MEND and the arrays $match, $mbegin, $mend (1-based character offsets;
// -1 for a group that did not take part).
int cond_regex(const str& l, const str& re, ExecContext& ctx,
               strv who);

// -eq … -ge on evaluated operands: 0 when the comparison holds, else 1.
int cond_numeric(strv op, const MathNum& l, const MathNum& r) noexcept;

}  // namespace jfsh

#endif  // JFSH_BUILTINS_COND_H
