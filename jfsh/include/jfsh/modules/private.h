#ifndef JFSH_MODULES_PRIVATE_H
#define JFSH_MODULES_PRIVATE_H
// jfsh/modules/private.h — the `zsh/private` module (M11).
//
// Contributes `private [typeset options] name[=value]…`: a local
// parameter seen only by the function that declared it, not by the
// functions it calls (they see the outer parameter of that name).

#include <jfsh/modules/module.h>

namespace jfsh {

void register_private_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_PRIVATE_H
