#ifndef JFSH_MODULES_ATTR_H
#define JFSH_MODULES_ATTR_H
// jfsh/modules/attr.h — the `zsh/attr` module (M11).
//
// Contributes zgetattr, zsetattr, zdelattr and zlistattr: extended
// file attributes through <sys/xattr.h> (macOS and Linux); -h acts on a
// symlink itself.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_attr_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ATTR_H
