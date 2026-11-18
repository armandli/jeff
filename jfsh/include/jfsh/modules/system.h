#ifndef JFSH_MODULES_SYSTEM_H
#define JFSH_MODULES_SYSTEM_H
// jfsh/modules/system.h — the `zsh/system` module (M10 Phase 2b).
//
// Contributes zsh's thin wrappers around the UNIX syscall surface:
//
//   syserror [-e var] [-p prefix] errno
//   sysopen  -o flags -u fd file
//   sysread  [-c count] [-i fd] [-o fd] [-s size] [-t timeout] [name]
//   sysseek  [-u fd] [-w start|end|current] offset
//   syswrite [-c count] [-o fd] data
//   zsystem  flock [-t secs] [-f fd] [-r] file
//   zsystem  supports feature
//   $errnos  — associative array: errno name → numeric value (EACCES → "13").
//
// Used by any script that needs raw fd I/O, timed reads or advisory locks.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_system_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_SYSTEM_H
