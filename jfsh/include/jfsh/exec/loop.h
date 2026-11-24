#ifndef JFSH_EXEC_LOOP_H
#define JFSH_EXEC_LOOP_H
// jfsh/exec/loop.h — what the shell does around the commands it reads:
// startup and logout files, and the hooks the interactive loop runs before
// each prompt and each command (zsh's init.c and the preprompt() of
// utils.c).

#include <ctime>
#include <string>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/history/history.h>

#ifndef JFSH_SYSCONFDIR
#define JFSH_SYSCONFDIR "/etc"
#endif
// The default $fpath: the site directory, then the shipped function
// library.  A build that isn't installed falls back to the library in the
// source tree.
#ifndef JFSH_SITE_FUNCTIONS_DIR
#define JFSH_SITE_FUNCTIONS_DIR ""
#endif
#ifndef JFSH_FUNCTIONS_DIR
#define JFSH_FUNCTIONS_DIR ""
#endif
#ifndef JFSH_SOURCE_FUNCTIONS_DIR
#define JFSH_SOURCE_FUNCTIONS_DIR ""
#endif

namespace jfsh {

struct StartupConfig {
  str  sysconfdir = JFSH_SYSCONFDIR;   // where jfshenv, jfshrc, … live
  bool zsh_compat = false;             // also read ~/.zshenv, ~/.zshrc, …
};

// The parameters a new shell sets from its environment, as zsh's
// setupvals: $SHLVL one more than it was, $PWD (the home directory or the
// environment's $PWD when it names the current directory by an absolute
// path without `.` or `..`, else the real path) and $OLDPWD the same, all
// exported; $MAILCHECK 60; the tied pairs ($PATH and $path, $FPATH and
// $fpath, …), $fpath defaulting to the function library.
void init_shell_params(ExecContext& ctx);

// Reads the startup files, in zsh's order:
//   SYSCONFDIR/jfshenv           always
//   $ZDOTDIR/.jfshenv            with RCS
//   SYSCONFDIR/jfsh_profile      login, with RCS and GLOBAL_RCS
//   $ZDOTDIR/.jfsh_profile       login, with RCS
//   SYSCONFDIR/jfshrc            interactive, with RCS and GLOBAL_RCS
//   $ZDOTDIR/.jfshrc             interactive, with RCS
//   SYSCONFDIR/jfsh_login        login, with RCS and GLOBAL_RCS
//   $ZDOTDIR/.jfsh_login         login, with RCS
// $ZDOTDIR defaults to $HOME.  RCS and GLOBAL_RCS are looked at before each
// file, so a file can turn them off for the ones after it.  With
// zsh_compat, each $ZDOTDIR file is followed by zsh's own (.zshenv,
// .zprofile, .zshrc, .zlogin).  Stops early when a file exits the shell.
void run_startup_files(Executor& ex, const StartupConfig& cfg);

// A login shell's logout files: $ZDOTDIR/.jfsh_logout (and .zlogout with
// zsh_compat), then SYSCONFDIR/jfsh_logout.
void run_logout_files(Executor& ex, const StartupConfig& cfg);

// Calls the function `name`, then each function named in the array
// `${name}_functions`, with `args` as their positional parameters.  $? and
// $pipestatus are kept.  Returns whether any function was called.
bool run_hook(Executor& ex, strv name, const vec<str>& args = {});

// Before each primary prompt: precmd, then periodic if $PERIOD seconds have
// passed, then the `sched` events that are due, then the mail files when
// $MAILCHECK seconds have passed.
void before_prompt(Executor& ex, s::time_t now);

// Before a command line runs: preexec with $1 the line as read (after
// history expansion), $2 the parsed line on one line, $3 the parsed line
// in full, as zsh's loop passes them.
void before_exec(Executor& ex, const str& line, const CompiledProgram& prog);

// Runs the `sched` events due at `now`, earliest first.
void run_sched(Executor& ex, s::time_t now);

// Checks $mailpath (or $MAILPATH, or $MAIL) and reports files that changed
// since the last check, on stderr.
void check_mail(Executor& ex, s::time_t now);

// The history settings as the options and $HISTSIZE, $SAVEHIST,
// $HISTFILE and $histchars have them now.  Unset, $HISTSIZE is 30 and
// $SAVEHIST 0, zsh's defaults.
HistOptions history_options(const ExecContext& ctx);

}  // namespace jfsh

#endif  // JFSH_EXEC_LOOP_H
