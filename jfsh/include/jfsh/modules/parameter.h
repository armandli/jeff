#ifndef JFSH_MODULES_PARAMETER_H
#define JFSH_MODULES_PARAMETER_H
// jfsh/modules/parameter.h — the `zsh/parameter` module (M10 Phase 2).
//
// Exposes the shell's own tables as live associative-array or array
// parameters that compsys and the shipped function library read:
//
//   $parameters, $commands, $functions, $aliases, $galiases, $saliases,
//   $builtins, $reswords, $modules, $options, $widgets,
//   $dis_functions, $dis_aliases, $dis_galiases, $dis_saliases,
//   $dis_builtins, $dis_reswords, $dis_patchars, $patchars.
//
// Each is `PM_HASH | PM_SPECIAL | PM_READONLY` (or `PM_ARRAY` for the
// reserved-word / pattern-char sets).  A ParamHandlers::get_hash /
// get_array runs on every lookup so a function defined between prompts
// shows up in the next `${(k)functions}` without a refresh call.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_parameter_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_PARAMETER_H
