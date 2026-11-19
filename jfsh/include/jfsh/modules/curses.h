#ifndef JFSH_MODULES_CURSES_H
#define JFSH_MODULES_CURSES_H
// jfsh/modules/curses.h — the `zsh/curses` module (M11).
//
// Contributes `zcurses`, windows on an (n)curses screen: init, end,
// addwin, delwin, refresh, touch, move, clear, position, char, string,
// border, attr, bg, scroll, input, timeout, mouse, querychar and resize;
// plus $ZCURSES_COLORS, $ZCURSES_COLOR_PAIRS and the $zcurses_windows,
// $zcurses_attrs and $zcurses_colors arrays.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_curses_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_CURSES_H
