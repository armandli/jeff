#ifndef JFSH_MODULES_STAT_H
#define JFSH_MODULES_STAT_H
// jfsh/modules/stat.h — the `zsh/stat` module (a port of zsh's
// Src/Modules/stat.c).
//
//   zstat (and stat) [-gnNolLtTrs] [-f fd] [-H hash] [-A array] [-F fmt]
//                    [+element] [file …]
//
// The elements: device inode mode nlink uid gid rdev size atime mtime ctime
// blksize blocks link.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_stat_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_STAT_H
