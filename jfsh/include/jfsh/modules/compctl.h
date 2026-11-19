#ifndef JFSH_MODULES_COMPCTL_H
#define JFSH_MODULES_COMPCTL_H
// jfsh/modules/compctl.h — the `zsh/compctl` module (M11).
//
// Contributes `compctl`, zsh's old completion control, on top of
// jfsh's completion engine.  The whole syntax is parsed; -f -/ -c -v -a
// -A -B -F -o -u -e -b -k -g -K -P -S -X work, `compctl -L` lists and
// `compctl + cmd` deletes.  Extended (-x) and alternative (+ options)
// completion say `unsupported option`.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_compctl_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_COMPCTL_H
