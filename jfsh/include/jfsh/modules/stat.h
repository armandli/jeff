#ifndef JFSH_MODULES_STAT_H
#define JFSH_MODULES_STAT_H
// jfsh/modules/stat.h — the `zsh/stat` module (M10 Phase 2c).
//
// Contributes the `zstat` builtin: a shell-level wrapper over stat(2) /
// lstat(2) that writes file metadata into a hash, an array or stdout.
//
//   zstat [-glLnoNrstT] [-H hash | -A array] [+element] file…
//
// Elements zsh exposes (and we do too): device, inode, mode, nlink, uid,
// gid, rdev, size, atime, mtime, ctime, blksize, blocks, link, type.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_stat_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_STAT_H
