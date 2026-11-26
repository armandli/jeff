#ifndef JFSH_MODULES_ZUTIL_H
#define JFSH_MODULES_ZUTIL_H
// jfsh/modules/zutil.h — the `zsh/zutil` module (a port of zsh's
// Src/Modules/zutil.c).
//
// Features: b:zformat (-f/-F/-a), b:zparseopts, b:zregexparse and
// b:zstyle, each autoloadable.  `is-at-least` is a shell function in
// $fpath, as in zsh; `zcompile` is zsh's core builtin, not this module's.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_zutil_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ZUTIL_H
