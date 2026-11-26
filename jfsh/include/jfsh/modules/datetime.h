#ifndef JFSH_MODULES_DATETIME_H
#define JFSH_MODULES_DATETIME_H
// jfsh/modules/datetime.h — the `zsh/datetime` module (a port of zsh's
// Src/Modules/datetime.c).
//
// Features: b:strftime (`strftime [-nqr] [-s scalar] format [epochtime
// [nanoseconds]]`, -r parses with strptime), p:EPOCHSECONDS,
// p:EPOCHREALTIME (a float), p:epochtime (seconds and nanoseconds).  The
// parameters are read-only and come with the module: zsh -f has none
// until `zmodload zsh/datetime`.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_datetime_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_DATETIME_H
