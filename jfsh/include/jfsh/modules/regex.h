#ifndef JFSH_MODULES_REGEX_H
#define JFSH_MODULES_REGEX_H
// jfsh/modules/regex.h — the `zsh/regex` module (a port of zsh's
// Src/Modules/regex.c).
//
// `[[ string -regex-match re ]]`, a POSIX extended regex (case blind
// without CASE_MATCH), which is what `=~` runs: $MATCH, $MBEGIN, $MEND,
// $match, $mbegin and $mend, or $BASH_REMATCH with BASH_REMATCH.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_regex_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_REGEX_H
