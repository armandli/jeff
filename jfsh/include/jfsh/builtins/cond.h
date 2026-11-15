#ifndef JFSH_BUILTINS_COND_H
#define JFSH_BUILTINS_COND_H
// jfsh/builtins/cond.h — the tests shared by `[[ … ]]` and `test` / `[`.
//
// Mirrors zsh's cond.c.  The executor parses nothing here: `[[ ]]` arrives
// as a WC_COND tree (src/exec/executor.cpp expands the operands and does
// the pattern matching), and `test` (src/builtins/cond.cpp) applies POSIX's
// argument-count rules first.  Every function returns a cond status:
//   0 true, 1 false, 2 an error (unknown condition, bad integer), and
//   3 `-o` with a name that is no option.
// Errors are printed with `who` in front ("test: ", or "" for `[[ ]]`).

#include <string>
#include <string_view>

namespace jfsh {

struct ExecContext;

// -a -b -c -d -e -f -g -h -k -n -o -p -r -s -t -u -v -w -x -z -L -O -G -S -N
bool cond_is_unary(std::string_view op) noexcept;
// = == != < > -nt -ot -ef -eq -ne -lt -le -gt -ge (=~ is [[ ]] only).
bool cond_is_binary(std::string_view op) noexcept;
bool cond_is_numeric(std::string_view op) noexcept;   // -eq … -ge

int cond_unary(std::string_view op, const std::string& arg, ExecContext& ctx,
               std::string_view who);

// The file comparisons -nt -ot -ef; false unless both files exist.
int cond_file_binary(std::string_view op, const std::string& l, const std::string& r);

// `l =~ re` with a POSIX extended regex.  On a match, sets $MATCH, $MBEGIN,
// $MEND and the arrays $match, $mbegin, $mend (1-based character offsets;
// -1 for a group that did not take part).
int cond_regex(const std::string& l, const std::string& re, ExecContext& ctx,
               std::string_view who);

// -eq … -ge on already evaluated operands: `cmp` is <0, 0 or >0.
int cond_compare(std::string_view op, int cmp) noexcept;

}  // namespace jfsh

#endif  // JFSH_BUILTINS_COND_H
