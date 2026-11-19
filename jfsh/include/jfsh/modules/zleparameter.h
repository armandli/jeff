#ifndef JFSH_MODULES_ZLEPARAMETER_H
#define JFSH_MODULES_ZLEPARAMETER_H
// jfsh/modules/zleparameter.h — the `zsh/zleparameter` module (M11 Phase 11a).
//
// Contributes two live-view parameters that expose the ZLE line editor's
// state to shell scripts (compsys and theme helpers reach for these):
//
//   $widgets   — associative array: widget name → kind string, where
//                kind is "builtin", "user:FUNC" (zle -N FUNC), or
//                "alias:TARGET" (zle -A NAME TARGET).
//   $keymaps   — array of keymap names (`emacs`, `viins`, `vicmd`,
//                `main`, …).

#include <jfsh/modules/module.h>

namespace jfsh {

void register_zleparameter_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ZLEPARAMETER_H
