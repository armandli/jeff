#ifndef JFSH_MODULES_FILES_H
#define JFSH_MODULES_FILES_H
// jfsh/modules/files.h — the `zsh/files` module (M10 Phase 2b).
//
// Contributes zsh's shell-builtin implementations of the common
// file-system utilities:
//
//   chmod MODE file…
//   chown OWNER[:GROUP] file…
//   chgrp GROUP file…
//   ln [-fs] src dst
//   mkdir [-pm MODE] dir…
//   mv src dst
//   rm [-rf] file…
//   rmdir dir…
//   sync
//
// Loading the module replaces the usual external-command lookup for these
// names so scripts avoid a fork+exec for every file touch.  The external
// still runs if the user invokes it as `=rm`, `command rm`, or the module
// isn't loaded.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_files_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_FILES_H
