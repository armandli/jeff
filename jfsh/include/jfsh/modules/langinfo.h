#ifndef JFSH_MODULES_LANGINFO_H
#define JFSH_MODULES_LANGINFO_H
// jfsh/modules/langinfo.h — the `zsh/langinfo` module (M11 Phase 11a).
//
// Contributes `$langinfo`: a read-only associative array whose keys are
// nl_langinfo(3) item names (`CODESET`, `D_FMT`, `T_FMT`, `AM_STR`,
// `ABDAY_1`, …) and whose values are the live nl_langinfo result for the
// current locale.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_langinfo_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_LANGINFO_H
