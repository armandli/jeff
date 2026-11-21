#ifndef JFSH_MODULES_COMPUTIL_H
#define JFSH_MODULES_COMPUTIL_H
// jfsh/modules/computil.h — the `zsh/computil` module (M15 Phase 15b).
//
// The builtins compsys' shell functions are written with: compdescribe
// (_describe), compvalues (_values), comptags and comptry (_tags,
// _wanted, _requested), compfiles (_path_files) and compgroups.  Like
// compadd they work only inside a completion function.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_computil_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_COMPUTIL_H
