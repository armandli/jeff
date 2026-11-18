#ifndef JFSH_MODULES_DATETIME_H
#define JFSH_MODULES_DATETIME_H
// jfsh/modules/datetime.h — the `zsh/datetime` module.
//
// Contributes:
//   - $EPOCHSECONDS, $EPOCHREALTIME, $epochtime array (already present as
//     specials; the module records them so `zmodload -L` lists the right
//     feature set);
//   - `strftime [-rs] fmt [epochtime]` builtin.  `-r` reverses: parse `fmt`
//     and print the epoch time.  `-s var` stores the result in `var`.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_datetime_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_DATETIME_H
