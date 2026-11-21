#ifndef JFSH_CORE_SORT_H
#define JFSH_CORE_SORT_H
// jfsh/core/sort.h — string order as zsh sorts (sort.c): by the locale's
// collation (LC_COLLATE), optionally with runs of digits compared by
// value.  Used by globbing, the (o) parameter flags and completion.

#include <jfsh/core/aliases.h>

namespace jfsh {

// <0, 0 or >0 as `a` sorts before, with or after `b` (zsh's zstrcmp).
// With `numeric`, at the first difference runs of digits compare by
// value; `backslashes` ignores backslashes (completion matches).
int zstrcmp(strv a, strv b, bool numeric = false, bool backslashes = false);

inline bool zstr_less(strv a, strv b, bool numeric = false) {
  return zstrcmp(a, b, numeric) < 0;
}

}  // namespace jfsh

#endif  // JFSH_CORE_SORT_H
