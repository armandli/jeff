#ifndef JFSH_MODULES_ZPROF_H
#define JFSH_MODULES_ZPROF_H
// jfsh/modules/zprof.h — the `zsh/zprof` module (M11).
//
// Profiles shell functions while loaded: calls, total and self time
// per function, and which function called which.  `zprof` prints the
// report, `zprof -c` clears it.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_zprof_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ZPROF_H
