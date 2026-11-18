#ifndef JFSH_MODULES_REGEX_H
#define JFSH_MODULES_REGEX_H
// jfsh/modules/regex.h — the `zsh/regex` module (M10 polish).
//
// Contributes the `-regex-match` binary condition for `[[ ]]` using POSIX
// Extended Regular Expressions (via `<regex.h>`).  On a match it sets
// `$MATCH` / `$match` the same way `=~` and `zsh/pcre` do.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_regex_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_REGEX_H
