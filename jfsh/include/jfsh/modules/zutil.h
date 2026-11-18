#ifndef JFSH_MODULES_ZUTIL_H
#define JFSH_MODULES_ZUTIL_H
// jfsh/modules/zutil.h — the `zsh/zutil` module (M10 Phase 2).
//
// Contributes the four utility builtins that compsys and the shipped
// function library reach for most often:
//
//   zstyle         — the style table that shapes compsys' behaviour.
//   zparseopts     — argv parser used by every `_f*` function to pull
//                    `-O arr`, `-d desc`, etc. off its own argument list.
//   is-at-least    — zsh-version comparison, used by rc files.
//   zformat        — `-f var fmt args…`: printf-like format with zsh's
//                    `%key:value` arg syntax (M10 Phase 2a implements -f;
//                    `-a` is in a follow-up).
//
// `zregexparse` and `zcompile` are bigger pieces and land with the
// completion wiring work.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_zutil_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_ZUTIL_H
