#ifndef JFSH_MODULES_MAPFILE_H
#define JFSH_MODULES_MAPFILE_H
// jfsh/modules/mapfile.h — the `zsh/mapfile` module (M11).
//
// Contributes `$mapfile`, an associative array of files: reading
// `${mapfile[path]}` reads the file, `mapfile[path]=text` writes it and
// `unset 'mapfile[path]'` removes it.  Its keys are the regular files in
// the current directory.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_mapfile_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_MAPFILE_H
