#ifndef JFSH_MODULES_PCRE_H
#define JFSH_MODULES_PCRE_H
// jfsh/modules/pcre.h — the `zsh/pcre` module (a port of zsh's
// Src/Modules/pcre.c to PCRE2).
//
//   pcre_compile [-aimxs] pattern
//   pcre_study
//   pcre_match   [-b] [-v var] [-a arr] [-n offset] string
//
// The match goes to $MATCH and $match (or -v and -a), with -b its byte
// offsets to $ZPCRE_OP.  Also `[[ string -pcre-match re ]]`, which `=~`
// uses under RE_MATCH_PCRE (the module comes in for it).  Without PCRE2 at
// build time the builtins say "not available on this system".

#include <jfsh/modules/module.h>

namespace jfsh {

void register_pcre_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_PCRE_H
