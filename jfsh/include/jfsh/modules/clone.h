#ifndef JFSH_MODULES_CLONE_H
#define JFSH_MODULES_CLONE_H
// jfsh/modules/clone.h — the `zsh/clone` module (M11).
//
// Contributes `clone tty`: forks a copy of the shell whose standard
// input, output and error, and controlling terminal, are `tty`.  The
// parent gets the child's pid in $!; the child goes on running the same
// code with $TTY set.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_clone_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_CLONE_H
