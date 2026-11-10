#include <jfsh/exec/executor.hh>

// REPL loop helpers (interactive prompt-read-execute cycle).
//
// TODO(loop/rc): Implement RC file loading sequence (mirrors zsh init.c):
//   1. /etc/zshenv  (always, unless --no-rcs)
//   2. ~/.zshenv
//   3. if login: /etc/zprofile → ~/.zprofile
//   4. if interactive: /etc/zshrc → ~/.zshrc
//   5. if login: /etc/zlogin → ~/.zlogin
//   Use OptionState::login() and interactive() flags to gate each file.
//   Source files via builtin_source() / exec_string() in the global scope.
//
// TODO(loop/sched): In each REPL iteration, check the sched queue (from
//   builtin_sched) and dispatch any overdue events before reading the next line.
//
// TODO(loop/mailcheck): If MAIL / MAILPATH is set and MAIL_WARN is on,
//   check for new mail each MAILCHECK seconds (similar to sched above).
//
// TODO(loop/periodic): If $PERIOD is set and $periodic function is defined,
//   call it every $PERIOD seconds in the REPL loop.
//
// TODO(loop/precmd): Call the precmd hook function (if defined) before each
//   primary prompt is displayed. Also call preexec before each command.

namespace jfsh {
}  // namespace jfsh
