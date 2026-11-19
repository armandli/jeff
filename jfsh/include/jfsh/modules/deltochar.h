#ifndef JFSH_MODULES_DELTOCHAR_H
#define JFSH_MODULES_DELTOCHAR_H
// jfsh/modules/deltochar.h — the `zsh/deltochar` module (M11).
//
// Contributes the ZLE widgets delete-to-char and zap-to-char: read a
// character and kill up to and including (delete-to-char) or up to but
// not including (zap-to-char) its next occurrence; a numeric argument
// picks the nth one, a negative one searches backwards.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_deltochar_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_DELTOCHAR_H
