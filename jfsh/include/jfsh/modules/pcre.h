#ifndef JFSH_MODULES_PCRE_H
#define JFSH_MODULES_PCRE_H
// jfsh/modules/pcre.h — the `zsh/pcre` module (M10 Phase 2c).
//
// Contributes zsh's PCRE2 bindings:
//
//   pcre_compile [-aimsx] pattern
//   pcre_study                           — no-op on PCRE2 (jit is cheap)
//   pcre_match   [-v var] [-a arr] [-n offset] string
//   $PCRE_MATCH   — whole-match text after a successful pcre_match
//   $pcre_match   — array of captured groups (1-based)
//
// Also arms `[[ string -pcre-match pattern ]]` as a module condition,
// though the `[[ ]]` wiring for arbitrary module conds lands with M14.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_pcre_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_PCRE_H
