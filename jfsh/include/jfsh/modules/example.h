#ifndef JFSH_MODULES_EXAMPLE_H
#define JFSH_MODULES_EXAMPLE_H
// jfsh/modules/example.h — the `zsh/example` module (a port of zsh's
// Src/Modules/example.c), a module with each kind of feature:
//
//   b:example          prints its options, arguments and the parameters
//   C:ex               [[ a -ex b ]]: a and b make "example"
//   c:len              [[ -len str [n] ]]
//   f:length, f:sum    length(text) (a string function), sum(n …)
//   p:exarr p:exint p:exstr
//
// Functions named example* run with GLOB_DOTS set.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_example_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_EXAMPLE_H
