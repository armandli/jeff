#ifndef JFSH_MODULES_DB_GDBM_H
#define JFSH_MODULES_DB_GDBM_H
// jfsh/modules/db_gdbm.h — the `zsh/db/gdbm` module (M11).
//
// Contributes `ztie -d db/gdbm -f file [-r] name`, `zuntie [-u]
// name…` and `zgdbmpath name`: an associative array whose keys and values
// live in a GDBM database (or, where GDBM is missing, the system's ndbm),
// read and written key by key.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_db_gdbm_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_DB_GDBM_H
