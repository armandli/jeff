#ifndef JFSH_MODULES_NEARCOLOR_H
#define JFSH_MODULES_NEARCOLOR_H
// jfsh/modules/nearcolor.h — the `zsh/nearcolor` module (M11).
//
// While loaded, a 24-bit `%F{#rrggbb}` / `%K{#rrggbb}` prompt colour
// is shown as the nearest colour of the 256-colour palette, or of the 16
// ANSI colours when $TERM doesn't mention 256 colours.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_nearcolor_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_NEARCOLOR_H
