#include <jfsh/builtins/builtins.h>

// [[ ]] conditional expression evaluator.
//
// TODO(cond/unary): Implement unary file/string tests:
//   -a/-e (exists), -f (regular), -d (directory), -s (non-empty),
//   -r/-w/-x (permissions), -L/-h (symlink), -b/-c/-p/-S (device/pipe/socket),
//   -z (empty string), -n (non-empty string), -o (option set), -v (var set).
//
// TODO(cond/binary): Implement binary comparison operators:
//   String: = == != < > (lexicographic)
//   Numeric: -eq -ne -lt -le -gt -ge
//   Regex: =~ (store captures in $match / ${BASH_REMATCH})
//   File age: -nt (newer than), -ot (older than), -ef (same inode)
//
// TODO(cond/logic): Implement &&, ||, ! inside [[ ]] (short-circuit evaluation).
//
// TODO(cond/parens): Implement ( expr ) grouping inside [[ ]].
//
// Note: [ and test (POSIX) are handled separately in builtin_test().
// [[ is a compound command parsed by par_cond() and executed by exec_cond().
// The evaluator here receives the WC_COND bytecode stream and returns 0/1.

namespace jfsh {
}  // namespace jfsh
