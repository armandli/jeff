#ifndef JFSH_MODULES_TERMINFO_H
#define JFSH_MODULES_TERMINFO_H
// jfsh/modules/terminfo.h — the `zsh/terminfo` module (M11).
//
// Contributes `echoti cap [args…]` and the read-only `$terminfo`
// associative array over the terminfo entry for $TERM (numbers as
// digits, booleans as yes/no, strings raw).

#include <jfsh/core/aliases.h>
#include <jfsh/modules/module.h>

namespace jfsh {

void register_terminfo_module(ModuleManager& mm);

// Loads the terminfo entry for `term` as the process's current terminal
// (shared by zsh/terminfo, zsh/termcap and zsh/nearcolor so they don't
// flip it between entries).  False without curses or without the entry.
bool term_setup(const str& term);

// The entry's number of colours (`colors`), -1 when unknown.
int term_colors(const str& term);

}  // namespace jfsh

#endif  // JFSH_MODULES_TERMINFO_H
