#ifndef JFSH_MODULES_NEWUSER_H
#define JFSH_MODULES_NEWUSER_H
// jfsh/modules/newuser.h — the `zsh/newuser` module (M11).
//
// Contributes `zsh-newuser-install [-f]`: the first-login helper that
// offers to write a starter ~/.jfshrc (with a choice of prompt).  main()
// loads the module and runs it for an interactive shell that found none
// of the user startup files.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_newuser_module(ModuleManager& mm);

// Whether a shell starting now would offer the new-user setup: none of
// .jfshenv, .jfsh_profile, .jfshrc, .jfsh_login is in $ZDOTDIR (or $HOME),
// a directory that exists.
bool newuser_needed(ExecContext& ctx);

}  // namespace jfsh

#endif  // JFSH_MODULES_NEWUSER_H
