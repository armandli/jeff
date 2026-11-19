#ifndef JFSH_MODULES_CAP_H
#define JFSH_MODULES_CAP_H
// jfsh/modules/cap.h — the `zsh/cap` module (M11).
//
// Contributes `cap [caps]`, `getcap file…` and `setcap caps file…`
// for POSIX.1e capabilities (Linux with libcap).  Elsewhere the builtins
// exist but report that capabilities aren't supported.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_cap_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_CAP_H
