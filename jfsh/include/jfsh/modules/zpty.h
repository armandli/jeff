#ifndef JFSH_MODULES_ZPTY_H
#define JFSH_MODULES_ZPTY_H
// jfsh/modules/zpty.h — the `zsh/zpty` module (M11).
//
// Contributes `zpty`, commands run on pseudo-terminals:
//
//   zpty [-e] [-b] name cmd…        — start; $REPLY = the master fd
//   zpty -d [name…]                 — end one, or all
//   zpty -w [-n] name [string…]     — write (a newline after, without -n)
//   zpty -r [-mt] name [param [pat]] — read a line, or until `pat` matches
//   zpty -t name                    — whether it's still running
//   zpty [-L]                       — list

#include <jfsh/modules/module.h>

namespace jfsh {

void register_zpty_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ZPTY_H
