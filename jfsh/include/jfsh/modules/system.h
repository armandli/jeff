#ifndef JFSH_MODULES_SYSTEM_H
#define JFSH_MODULES_SYSTEM_H
// jfsh/modules/system.h — the `zsh/system` module (a port of zsh's
// Src/Modules/system.c).
//
//   syserror [-e errvar] [-p prefix] [errno | errname]
//   sysopen  [-arw] [-m perms] [-o opts] -u fd file
//   sysread  [-c countvar] [-i infd] [-o outfd] [-s bufsize] [-t timeout] [param]
//   sysseek  [-u fd] [-w start|end|current] offset
//   syswrite [-c countvar] [-o outfd] data
//   zsystem  flock [-ef] [-i interval] [-r] [-t timeout] file; flock -u fd
//   zsystem  supports subcommand
//   systell(fd)  — math function
//   $errnos      — array of errno names by number
//   $sysparams   — pid, ppid, procsubstpid

#include <jfsh/modules/module.h>

namespace jfsh {

void register_system_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_SYSTEM_H
