#ifndef JFSH_MODULES_PARAMETER_H
#define JFSH_MODULES_PARAMETER_H
// jfsh/modules/parameter.h — the `zsh/parameter` module (a port of zsh's
// Src/Modules/parameter.c).
//
// The shell's own tables as special parameters, made when read: $aliases
// $builtins $commands $dirstack $dis_aliases $dis_builtins $dis_functions
// $dis_functions_source $dis_galiases $dis_patchars $dis_reswords
// $dis_saliases $funcfiletrace $funcsourcetrace $funcstack $functions
// $functions_source $functrace $galiases $history $historywords $jobdirs
// $jobstates $jobtexts $modules $nameddirs $options $parameters $patchars
// $reswords $saliases $userdirs $usergroups.  Those zsh lets you assign
// change the tables ($functions[f]=body defines f).  Each is marked for
// autoload at startup, so the first use loads the module, as in zsh.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_parameter_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_PARAMETER_H
