#ifndef JFSH_MODULES_ZSELECT_H
#define JFSH_MODULES_ZSELECT_H
// jfsh/modules/zselect.h — the `zsh/zselect` module (M11 Phase 11a).
//
// Contributes `zselect`: a shell-level binding of pselect(2).
//
//   zselect [-A arr] [-t timeout] [-r fd…] [-w fd…] [-e fd…]
//
// Waits for any of the listed fds to be readable (`-r`), writable (`-w`),
// or to have an exceptional condition (`-e`).  `-t secs` sets a timeout
// (fractional seconds; 0 means poll).  With `-A arr`, the ready fds are
// stored as "r:fd" / "w:fd" / "e:fd" entries; without, they go to stdout.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_zselect_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ZSELECT_H
