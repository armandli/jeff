#ifndef JFSH_MODULES_FILES_H
#define JFSH_MODULES_FILES_H
// jfsh/modules/files.h — the `zsh/files` module (a port of zsh's
// Src/Modules/files.c).
//
// chgrp, chmod (octal modes), chown, ln, mkdir, mv, rm, rmdir and sync as
// builtins, and the same as zf_chgrp … zf_sync, which don't hide the
// commands of those names.  Loading the module (or `zmodload -F zsh/files
// b:zf_rm …`) brings them in.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_files_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_FILES_H
