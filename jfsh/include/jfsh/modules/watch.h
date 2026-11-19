#ifndef JFSH_MODULES_WATCH_H
#define JFSH_MODULES_WATCH_H
// jfsh/modules/watch.h — the `zsh/watch` module (M11).
//
// Reports logins and logouts of the users in $watch before each
// prompt, at most every $LOGCHECK seconds, formatted by $WATCHFMT; `log`
// lists the matching users logged in now.

#include <jfsh/modules/module.h>

namespace jfsh {

void register_watch_module(ModuleManager& mm);

}  // namespace jfsh

#endif  // JFSH_MODULES_WATCH_H
