#ifndef JFSH_MODULES_TERMCAP_H
#define JFSH_MODULES_TERMCAP_H
// jfsh/modules/termcap.h — the `zsh/termcap` module (M11).
//
// Contributes `echotc cap [args…]` and the read-only `$termcap`
// associative array, both over the termcap emulation in (n)curses for
// $TERM.  Without curses the builtin says the module isn't available.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_termcap_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_TERMCAP_H
