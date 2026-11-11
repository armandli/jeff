#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <jfsh/builtins/builtins.hh>
#include <jfsh/exec/executor.hh>

namespace jfsh {

void BuiltinTable::register_all() {
  add({":",        builtin_colon,      BINF_SPECIAL});
  add({".",        builtin_dot,        BINF_SPECIAL, 1});
  add({"break",    builtin_break,      BINF_SPECIAL});
  add({"continue", builtin_continue,   BINF_SPECIAL});
  add({"eval",     builtin_eval,       BINF_SPECIAL});
  add({"exec",     builtin_exec,       BINF_SPECIAL});
  add({"exit",     builtin_exit,       BINF_SPECIAL});
  add({"export",   builtin_export,     BINF_SPECIAL | BINF_ASSIGN});
  add({"readonly", builtin_readonly,   BINF_SPECIAL | BINF_ASSIGN});
  add({"return",   builtin_return,     BINF_SPECIAL});
  add({"set",      builtin_set,        BINF_SPECIAL});
  add({"shift",    builtin_shift,      BINF_SPECIAL});
  add({"times",    builtin_times,      BINF_SPECIAL});
  add({"trap",     builtin_trap,       BINF_SPECIAL});
  add({"unset",    builtin_unset,      BINF_SPECIAL});

  add({"cd",       builtin_cd,         BINF_NONE, 0, 2});
  add({"pushd",    builtin_pushd,      BINF_NONE});
  add({"popd",     builtin_popd,       BINF_NONE});
  add({"dirs",     builtin_dirs,       BINF_NONE});
  add({"pwd",      builtin_pwd,        BINF_NONE});

  add({"bg",       builtin_bg,         BINF_NONE});
  add({"fg",       builtin_fg,         BINF_NONE});
  add({"jobs",     builtin_jobs,       BINF_NONE});
  add({"wait",     builtin_wait,       BINF_NONE});
  add({"disown",   builtin_disown,     BINF_NONE});
  add({"kill",     builtin_kill,       BINF_NONE});

  add({"typeset",  builtin_typeset,    BINF_TYPESET | BINF_ASSIGN});
  add({"local",    builtin_local,      BINF_TYPESET | BINF_ASSIGN});
  add({"declare",  builtin_declare,    BINF_TYPESET | BINF_ASSIGN});
  add({"integer",  builtin_integer,    BINF_TYPESET | BINF_ASSIGN});
  add({"float",    builtin_float,      BINF_TYPESET | BINF_ASSIGN});

  add({"echo",     builtin_echo,       BINF_PRINT | BINF_ECHOOPT});
  add({"print",    builtin_print,      BINF_PRINT});
  add({"printf",   builtin_printf,     BINF_PRINT});

  add({"read",     builtin_read,       BINF_NONE});

  add({"fc",       builtin_fc,         BINF_NONE});
  add({"history",  builtin_history,    BINF_NONE});
  add({"r",        builtin_r,          BINF_NONE});

  add({"alias",    builtin_alias,      BINF_NONE});
  add({"unalias",  builtin_unalias,    BINF_NONE});
  add({"builtin",  builtin_builtin,    BINF_NONE});
  add({"command",  builtin_command,    BINF_NONE});
  add({"source",   builtin_source,     BINF_NONE, 1});
  add({"hash",     builtin_hash,       BINF_NONE});
  add({"rehash",   builtin_rehash,     BINF_NONE});
  add({"true",     builtin_true,       BINF_NONE});
  add({"false",    builtin_false,      BINF_NONE});
  add({"[",        builtin_test,       BINF_NONE});
  add({"test",     builtin_test,       BINF_NONE});
  add({"ulimit",   builtin_ulimit,     BINF_NONE});
  add({"umask",    builtin_umask,      BINF_NONE});
  add({"zmodload", builtin_zmodload,   BINF_NONE});
  add({"autoload", builtin_autoload,   BINF_NONE});
  add({"functions",builtin_functions,  BINF_NONE});
  add({"whence",   builtin_whence,     BINF_NONE});
  add({"which",    builtin_which,      BINF_NONE});
  add({"type",     builtin_type,       BINF_NONE});
  add({"compinit", builtin_compinit,   BINF_NONE});
  add({"setopt",   builtin_setopt,     BINF_NONE});
  add({"unsetopt", builtin_unsetopt,   BINF_NONE});
  add({"bindkey",  builtin_bindkey,    BINF_NONE});
  add({"vared",    builtin_vared,      BINF_NONE});
  add({"zle",      builtin_zle,        BINF_NONE});
  add({"let",      builtin_let,        BINF_NONE});
  add({"sched",    builtin_sched,      BINF_NONE});
}

void BuiltinTable::add(BuiltinDef def) {
  mTable.emplace(def.name, std::move(def));
}

std::optional<std::reference_wrapper<const BuiltinDef>>
BuiltinTable::find(std::string_view name) const noexcept {
  auto it = mTable.find(std::string(name));
  if (it == mTable.end()) return std::nullopt;
  return std::cref(it->second);
}

void BuiltinTable::for_each(std::function<void(const BuiltinDef&)> fn) const {
  for (const auto& [name, def] : mTable) fn(def);
}

int builtin_colon     (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_break     (std::vector<std::string>&, ExecContext& ctx) {
  ctx.break_level = 1; return 0;
}
int builtin_continue  (std::vector<std::string>&, ExecContext& ctx) {
  ctx.cont_level = 1; return 0;
}
int builtin_dot(std::vector<std::string>& args, ExecContext&) {
  // TODO(builtin/dot): Source args[1] into the current shell environment.
  //   Open the file, read it line by line (or as a CompiledProgram), execute
  //   with executor.exec_string() in the current scope (no fork).
  //   Search $PATH if the filename has no slash (POSIX requirement).
  (void)args; return 0;
}
int builtin_eval(std::vector<std::string>& args, ExecContext&) {
  // TODO(builtin/eval): Join args[1..] with spaces, re-lex and re-parse,
  //   then execute in the current scope via executor.exec_string().
  //   EVAL_LINENO option: line number attributed to eval'd code.
  (void)args; return 0;
}
int builtin_exec(std::vector<std::string>& args, ExecContext&) {
  // TODO(builtin/exec): With args, replace the current process via execvp().
  //   Flags: -a name (set argv[0]), -c (clear environment), -l (login shell).
  //   Without args, just apply any redirections to the shell itself.
  (void)args; return 0;
}
int builtin_exit(std::vector<std::string>& args, ExecContext& ctx) {
  ctx.exiting  = true;
  ctx.exit_val = args.size() > 1 ? std::atoi(args[1].c_str()) : ctx.last_exit;
  return ctx.exit_val;
}
int builtin_export(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/export): For each arg:
  //   name=value → assign and mark PM_EXPORTED
  //   name       → mark existing param PM_EXPORTED
  //   -n name    → remove PM_EXPORTED flag
  //   -p         → print all exported vars in `export name=value` form
  (void)args; (void)ctx; return 0;
}
int builtin_readonly(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/readonly): Like export but sets PM_READONLY.
  //   Writing to a readonly param is an error.
  (void)args; (void)ctx; return 0;
}
int builtin_return(std::vector<std::string>& args, ExecContext& ctx) {
  ctx.returning  = true;
  ctx.return_val = args.size() > 1 ? std::atoi(args[1].c_str()) : ctx.last_exit;
  return ctx.return_val;
}
int builtin_set(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/set): Handle two roles:
  //   `set [±o option] …` — same as setopt/unsetopt; also single-letter flags
  //     (-e = errexit, -x = xtrace, -n = noexec, -u = nounset, etc.)
  //   `set -- word …` — replace positional parameters $1 $2 … with the words.
  //   `set` with no args — print all shell variables (like typeset -p).
  (void)args; (void)ctx; return 0;
}
int builtin_shift(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/shift): Remove the first N positional parameters (default 1).
  //   $1 becomes the old $2, etc.  Error if N > $#.
  //   Also supports named array: `shift arrayname N`.
  (void)args; (void)ctx; return 0;
}
int builtin_times(std::vector<std::string>&, ExecContext&) {
  std::printf("0m0.000s 0m0.000s\n0m0.000s 0m0.000s\n"); return 0;
}
int builtin_trap(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/trap): Parse `trap [action] [signal ...]`.
  //   action = ''  → reset to default; '-' → ignore; command-string → Command
  //   signal names via SignalTable::name_to_sig(); pseudo-signals EXIT/ZERR/DEBUG.
  //   `trap` with no args → print all traps in `trap -- 'cmd' SIG` form.
  //   Local traps: when LOCAL_TRAPS is set, traps set inside a function are
  //   restored on function return.
  (void)args; (void)ctx; return 0;
}
int builtin_unset(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/unset): Remove named variables and/or functions.
  //   -v → unset variable (default); -f → unset function.
  //   Error if the variable is readonly.
  //   For PM_SPECIAL params, call the unset handler rather than removing.
  (void)args; (void)ctx; return 0;
}
int builtin_cd(std::vector<std::string>& args, ExecContext&) {
  const char* dir = args.size() > 1 ? args[1].c_str() : getenv("HOME");
  return dir ? chdir(dir) : 1;
}
int builtin_pushd(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/pushd): Push directory onto the stack (stored in $dirstack).
  //   `pushd dir` → chdir(dir) and push old PWD.
  //   `pushd +N/-N` → rotate the stack.
  //   AUTOPUSHD: pushd called implicitly on every cd when option is set.
  //   PUSHD_MINUS, PUSHD_SILENT, PUSHD_TO_HOME options.
  (void)args; (void)ctx; return 0;
}
int builtin_popd(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/popd): Pop the top directory and chdir to it.
  (void)args; (void)ctx; return 0;
}
int builtin_dirs(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/dirs): Print the directory stack.
  //   -v: numbered, one per line; -l: expand ~ to full path; -p: one per line.
  (void)args; (void)ctx; return 0;
}
int builtin_pwd(std::vector<std::string>&, ExecContext&) {
  char buf[4096]; if (getcwd(buf, sizeof buf)) std::printf("%s\n", buf); return 0;
}
int builtin_bg(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/bg): Send SIGCONT to job's pgid; mark job ASYNC.
  //   `bg %N` or `bg` (current stopped job).
  (void)args; (void)ctx; return 0;
}
int builtin_fg(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/fg): tcsetpgrp() to give the job the terminal, send SIGCONT,
  //   then waitpid() (interruptible) until the job stops or finishes.
  (void)args; (void)ctx; return 0;
}
int builtin_jobs(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/jobs): Print live jobs from Jobs.for_each().
  //   -l: include pid; -p: only pid; -r: running only; -s: stopped only.
  //   LONG_LIST_JOBS option: always use long format.
  (void)args; (void)ctx; return 0;
}
int builtin_wait(std::vector<std::string>& args, ExecContext& ctx) {
  // Minimal: with no args, wait for every running job; with a pid, wait
  // for that one.  Returns the last reaped exit status.  Does not yet
  // support %jobid forms or -n.
  int rc = 0;
  if (args.size() <= 1) {
    for (;;) {
      int status = 0;
      pid_t pid = waitpid(-1, &status, 0);
      if (pid <= 0) break;
      ctx.jobs.update(pid, status);
      if (WIFEXITED(status))        rc = WEXITSTATUS(status);
      else if (WIFSIGNALED(status)) rc = 128 + WTERMSIG(status);
    }
    return rc;
  }
  for (std::size_t i = 1; i < args.size(); ++i) {
    pid_t pid = static_cast<pid_t>(std::atoi(args[i].c_str()));
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 and errno == EINTR) {}
    if (errno == ECHILD) continue;
    ctx.jobs.update(pid, status);
    if (WIFEXITED(status))        rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) rc = 128 + WTERMSIG(status);
  }
  return rc;
}
int builtin_disown(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/disown): Mark job JOB_DISOWNED so SIGHUP is not sent on exit.
  //   Also remove from job table so `jobs` doesn't show it.
  (void)args; (void)ctx; return 0;
}
int builtin_kill(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/kill): Send a signal to pid(s) or job(s).
  //   -s SIG / -SIG / -N; `kill -l` to list signal names.
  //   Accept %jobid notation.
  (void)args; (void)ctx; return 0;
}
int builtin_typeset(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/typeset): The central variable-declaration builtin.
  //   Flags: -i (integer), -f (float), -a (array), -A (hash/assoc),
  //          -l (lowercase), -u (uppercase), -r (readonly), -x (export),
  //          -g (global, suppress local), -p (print), -m (pattern match),
  //          -h (hide), -T (tie array/scalar), -U (unique array).
  //   Without a value, declare (create if needed); with =value, assign.
  //   `typeset -p` / `typeset` with no args → print all vars.
  (void)args; (void)ctx; return 0;
}
int builtin_local(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/local): Alias for typeset in a function scope.
  //   Calls params_.declare_local() for each name.
  (void)args; (void)ctx; return 0;
}
int builtin_declare(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/declare): Bash-compat alias for typeset (subset of flags).
  (void)args; (void)ctx; return 0;
}
int builtin_integer(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/integer): Shorthand for `typeset -i`.
  (void)args; (void)ctx; return 0;
}
int builtin_float(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/float): Shorthand for `typeset -F` (float with decimal).
  (void)args; (void)ctx; return 0;
}
int builtin_fc(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/fc): History editor and re-execution.
  //   `fc -l [first [last]]` → list history entries.
  //   `fc -e editor [first [last]]` → open entries in $editor and re-execute.
  //   `fc -s [pat=rep] [event]` → re-execute with optional substitution.
  //   `fc -p [file [size [savesize]]]` → push history file (LOCAL_HISTORY).
  //   `fc -P` → pop pushed history.
  (void)args; (void)ctx; return 0;
}
int builtin_history(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/history): Alias for `fc -l`; prints numbered history list.
  (void)args; (void)ctx; return 0;
}
int builtin_r(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/r): Alias for `fc -s`; re-execute the last command.
  (void)args; (void)ctx; return 0;
}
int builtin_alias(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/alias): Manage the alias table (a HashTable<std::string>
  //   that needs to be added to ExecContext or a global AliasTable).
  //   `alias name=value` → register; `alias name` → print; `alias` → print all.
  //   -g: global alias (expanded anywhere in line, not just command position).
  //   -s: suffix alias (expanded when a file with that suffix is run directly).
  //   The lexer must expand aliases at token-read time (alias_expand_ flag).
  (void)args; (void)ctx; return 0;
}
int builtin_unalias(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/unalias): Remove named entries from the alias table.
  //   -a: remove all aliases; -m pattern: remove matching aliases.
  (void)args; (void)ctx; return 0;
}
int builtin_builtin   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_command   (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_echo(std::vector<std::string>& args, ExecContext&) {
  for (std::size_t i = 1; i < args.size(); ++i) {
    if (i > 1) std::putchar(' ');
    std::fputs(args[i].c_str(), stdout);
  }
  std::putchar('\n'); return 0;
}
int builtin_print(std::vector<std::string>& args, ExecContext& ctx) {
  return builtin_echo(args, ctx);
}
int builtin_printf    (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_read(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/read): Read one line from stdin (or -u fd) into named vars.
  //   Split the line by IFS; assign fields to var1 var2 …; last var gets remainder.
  //   -r: raw mode (no backslash processing).
  //   -A/-a: read into array (all words).
  //   -s: silent (no echo, for passwords).
  //   -t N: timeout after N seconds.
  //   -n N: read at most N characters.
  //   -p prompt: display prompt without newline.
  //   -k: read a single keypress.
  (void)args; (void)ctx; return 0;
}
int builtin_source(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/source): Same as builtin_dot.  Source the file; pass any
  //   extra arguments as $1 $2 … to the sourced script.
  (void)args; (void)ctx; return 0;
}
int builtin_hash(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/hash): Manage the command hash table (PATH lookup cache).
  //   `hash` → print table; `hash cmd=path` → add entry; `hash -d` → dir hash.
  //   The hash table needs to be added to ExecContext as HashTable<std::string>.
  (void)args; (void)ctx; return 0;
}
int builtin_rehash(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/rehash): Rebuild the command hash table by walking $PATH.
  //   HASH_CMDS option: auto-hash on first use rather than on startup.
  (void)args; (void)ctx; return 0;
}
int builtin_true      (std::vector<std::string>&, ExecContext&) { return 0; }
int builtin_false     (std::vector<std::string>&, ExecContext&) { return 1; }
int builtin_test(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/test): Implement POSIX [ / test expression evaluator.
  //   Unary: -f -d -e -s -r -w -x -z -n -L -b -c -p -S -t (fd).
  //   Binary: = != < > -eq -ne -lt -le -gt -ge -nt -ot -ef.
  //   Logical: -a -o ! ( ) (different precedence from [[ ]]).
  //   For `[`, last arg must be `]`.
  (void)args; (void)ctx; return 0;
}
int builtin_ulimit(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/ulimit): Wrapper around getrlimit/setrlimit.
  //   -a: print all; -c -d -f -n -s -t -u -v -x: individual resource limits.
  //   Values: N (set hard+soft), `unlimited`, or Nh/Ns for hard/soft only.
  (void)args; (void)ctx; return 0;
}
int builtin_umask(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/umask): Get/set the file creation mask via umask(2).
  //   Accept octal or symbolic (u=rwx,g=rx,o=) notation.
  //   -S: print in symbolic form.
  (void)args; (void)ctx; return 0;
}
int builtin_zmodload(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/zmodload): Load/unload a module via ModuleManager::load().
  //   -a: autoload (load on first use); -u: unload; -l: list loaded modules.
  //   Pass the module name, search ModuleManager::search_paths().
  (void)args; (void)ctx; return 0;
}
int builtin_autoload(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/autoload): Mark a function name for lazy loading from $fpath.
  //   When the function is first called, search $fpath for a file named after
  //   it, source it, then invoke the now-defined function.
  //   -U: suppress alias expansion; -z/-k: zsh/ksh autoload style.
  (void)args; (void)ctx; return 0;
}
int builtin_functions(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/functions): Print function definitions using text.cpp decompiler.
  //   `functions name` → print one function; `functions` → print all.
  //   -u: print only undefined (autoload) functions; -m pattern: match.
  (void)args; (void)ctx; return 0;
}
int builtin_whence(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/whence): Report where a name is defined:
  //   alias, function, builtin, hashed command, or PATH search result.
  //   -c: print like `type`; -f: print function body; -v: verbose; -a: all.
  (void)args; (void)ctx; return 0;
}
int builtin_which(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/which): Like whence -c (command path only).
  //   Search PATH for each arg; print full path or "not found".
  (void)args; (void)ctx; return 0;
}
int builtin_type(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/type): POSIX-compat alias for `whence -v`.
  (void)args; (void)ctx; return 0;
}
int builtin_compinit(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/compinit): Bootstrap the completion system.
  //   Walk $fpath for files named _command, source them to populate the
  //   compdef table in CompletionEngine.
  //   -d dumpfile: cache compiled completions; -C: use cache without check.
  (void)args; (void)ctx; return 0;
}
int builtin_setopt(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/setopt): Set shell options by name.
  //   `setopt extendedglob` → opts.set(OPT_EXTENDEDGLOB)
  //   `setopt no_extendedglob` → opts.unset(OPT_EXTENDEDGLOB)
  //   `setopt` → print all currently set options.
  //   Use OptionState::find() for name lookup.
  (void)args; (void)ctx; return 0;
}
int builtin_unsetopt(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/unsetopt): Unset a shell option by name (inverse of setopt).
  //   `unsetopt globdots` → opts.unset(OPT_GLOBDOTS)
  (void)args; (void)ctx; return 0;
}
int builtin_bindkey(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/bindkey): Manage ZLE key bindings.
  //   `bindkey seq widget` → editor.bind_key(keymap, seq, widget)
  //   `bindkey -M keymap seq widget` → bind in named keymap
  //   `bindkey` → list all bindings; `bindkey seq` → show binding for seq.
  //   Keymap names: main, emacs, viins, vicmd, viopp, visual, menuselect.
  (void)args; (void)ctx; return 0;
}
int builtin_vared(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/vared): Edit a shell variable interactively with ZLE.
  //   Load the variable's current value into the line buffer, run the editor,
  //   then write the result back to the variable.
  (void)args; (void)ctx; return 0;
}
int builtin_zle(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/zle): ZLE widget management from within a widget function.
  //   `zle -N name [function]` → register a user-defined widget.
  //   `zle widget-name` → call a widget from within another widget.
  //   `zle -R` → trigger a display refresh.
  //   `zle -M msg` → display a message below the prompt.
  (void)args; (void)ctx; return 0;
}
int builtin_zpty(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/zpty): Run a command in a pseudo-terminal.
  //   `zpty name cmd` → fork cmd in a pty, store name for later I/O.
  //   `zpty -w name data` → write to the pty.
  //   `zpty -r name var` → read from the pty into var.
  //   `zpty -d name` → close the pty.
  (void)args; (void)ctx; return 0;
}
int builtin_sched(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/sched): Schedule a command to run after a delay.
  //   `sched +N:M cmd` → run cmd after N minutes M seconds.
  //   Implemented via SIGALRM or a checked event queue in the REPL loop.
  (void)args; (void)ctx; return 0;
}
int builtin_let(std::vector<std::string>& args, ExecContext& ctx) {
  // TODO(builtin/let): Evaluate one or more arithmetic expressions.
  //   Each arg is evaluated; return 0 if the last result is non-zero.
  //   Delegate to the math evaluator in builtins/math.cpp.
  //   `let "x = 1 + 2"` is equivalent to `(( x = 1 + 2 ))`.
  (void)args; (void)ctx; return 0;
}

}  // namespace jfsh
