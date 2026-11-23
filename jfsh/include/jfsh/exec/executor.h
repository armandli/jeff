#ifndef JFSH_EXEC_EXECUTOR_H
#define JFSH_EXEC_EXECUTOR_H
// jfsh/exec/executor.h — shell executor.
//
// Mirrors zsh's exec.c hierarchy:
//   execlist() → execpline() → execpline2() → execcmd() → execsimple()
// plus subshell, function call, and async execution paths.

#include <cstdint>
#include <ctime>
#include <unistd.h>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <jfsh/core/alias_table.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/signals.h>
#include <jfsh/core/types.h>
#include <jfsh/core/wordcode.h>
#include <jfsh/glob/glob.h>
#include <jfsh/modules/module.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {

enum ExecFlags : s::uint32_t {
  EF_NONE    = 0,
  EF_ASYNC   = 1 << 0,  // run as background job
  EF_NOPRINT = 1 << 1,  // don't print job info (used internally)
  EF_DASH    = 1 << 2,  // - prefix: login shell argv[0]
  EF_NOFORK  = 1 << 3,  // exec instead of fork (last cmd in subshell)
  EF_COPROC  = 1 << 4,  // set up coprocess pipes
  // The last stage of a pipeline, run from the shell: a command that forks
  // joins the pipeline's job, and exec_pline waits for it.
  EF_PIPELAST = 1 << 5,
  // A pipeline stage's own redirections: fd 1 already goes to the pipe,
  // so under MULTIOS a `>file` adds to it (EF_STAGEOUT); `|&` adds a
  // `2>&1` after them (EF_STAGEERR).
  EF_STAGEOUT = 1 << 6,
  EF_STAGEERR = 1 << 7,
  // With EF_ASYNC: `&|` / `&!`, not made a job.
  EF_DISOWN   = 1 << 8,
  // The sublist a background job's child runs: its own failure doesn't
  // set off ZERR or ERR_EXIT (only exec_sublist reads it).
  EF_NOERRCHECK = 1 << 9,
};

struct Executor;
struct HistoryManager;
struct Zle;
struct CompletionEngine;
struct CompState;

// A `sched` event: a command to run before the first prompt at or after
// `when`.
struct SchedEvent {
  s::time_t when      = 0;
  str       cmd{};
  bool      trash_zle = false;   // -o: clear the line editor first (no-op)
};

// What the interactive loop remembers between prompts (M8).
struct LoopState {
  vec<SchedEvent> sched{};            // sorted by `when`
  s::time_t       last_periodic  = 0; // when `periodic` last ran
  s::time_t       last_mailcheck = 0; // when the mail files were last checked
};

// Where modules hook into the running shell (M11).  Each hook is tagged
// with the module that added it so unloading the module drops it.
struct ShellHooks {
  // Around every shell-function call (zsh/zprof): the function's name and
  // true on entry, false on return.
  vec<s::pair<str, func<void(const str& name, bool entering)>>> function;
  // Before each interactive prompt, after precmd/periodic (zsh/watch).
  vec<s::pair<str, func<void()>>> before_prompt;

  void remove(strv owner) {
    auto drop = [&](auto& v) {
      s::erase_if(v, [&](const auto& e) { return e.first == owner; });
    };
    drop(function);
    drop(before_prompt);
  }
};

struct ExecContext {
  // Clears trap_failed and trap_status (below); wherever `aborting` is
  // cleared.
  void end_trap_abort() noexcept {
    trap_failed = false;
    trap_status = -1;
  }

  ParamTable&    params;
  OptionState&   opts;
  JobTable&      jobs;
  SignalManager& signals;

  int  last_exit   = 0;     // $?
  Pid  last_bg_pid = -1;    // $!
  int  break_level = 0;     // loops still to leave after `break N`/`continue N`
  bool continuing  = false; // … and the last of them goes on (`continue`)
  int  loop_depth  = 0;     // loops being run, functions included
  int  func_depth  = 0;     // function calls being run (FUNCNEST)
  bool returning   = false;
  int  return_val  = 0;
  bool exiting     = false;
  int  exit_val    = 0;
  // The exit leaves at once, without unwinding through `always` blocks:
  // `exit` outside a function, and ERR_EXIT (zsh's zexit).
  bool exit_now    = false;
  // Ctrl-C under job control, or a TRAPxxx function that failed: unwind
  // the command line.  An interactive shell goes back to the prompt, any
  // other shell exits.
  bool aborting    = false;
  // A failed expansion (`${x?}`) set `aborting`: in a widget function it
  // throws away the line being edited too.
  bool abort_line  = false;
  // Set by `${x?}`, zsh's ERRFLAG_HARD: unlike other errors it ends more
  // than an `eval` (builtin_eval clears it before running and reads it).
  bool hard_error  = false;
  // Set with `aborting` by an interrupt (Ctrl-C, a signal trap that
  // failed), as opposed to an error.  Cleared by its two readers before
  // they run their code: `source`, which ends the file on an error but goes
  // on unwinding on an interrupt, and `always` ($TRY_BLOCK_INTERRUPT).
  bool interrupt   = false;
  // $TRY_BLOCK_ERROR and $TRY_BLOCK_INTERRUPT: whether the `{ … }` of the
  // `always` block being run ended in an error (or an interrupt); -1
  // outside one.
  s::int64_t try_error     = -1;
  s::int64_t try_interrupt = -1;
  bool hangup      = false; // SIGHUP reached an interactive shell
  // Ctrl-C ended the command line: the terminal echoed ^C without a
  // newline, which the REPL then prints before the prompt.
  bool intr_echoed = false;
  // A TRAPxxx that ended with `return N`, N non-zero, ended the line (zsh's
  // trapreturn): `trap_failed` for any signal but SIGINT and SIGQUIT, which
  // zsh counts as an interrupt rather than an error.  Cleared with
  // `aborting` where the line ends.
  bool trap_failed = false;
  // N while that abort is in effect (else -1): what dotrapargs leaves in
  // lastval, which the commands around then keep.
  int  trap_status = -1;
  // With `returning`: a return that at the top of a script ends just the
  // current line (the trap's return an `eval` keeps, zsh's retflag).
  bool return_line = false;
  // The builtin running took a signal itself: `kill` sent one to the
  // shell, or `wait` was cut short by one.
  bool signal_in_builtin = false;
  // Under job control, the process group of a pipeline whose last stage
  // runs in the shell: it keeps the terminal while that stage runs, so a
  // foreground job started there gives the terminal back to it.
  Pid  pipeline_pgid = 0;
  int  pipeline_jid  = 0;   // … and the job of its forked stages
  // ERR_EXIT, ERR_RETURN and ZERR are off while this is positive: in the
  // conditions of if/while/until, after `!`, and in all but the last
  // pipeline of a && / || chain.
  int  noerrexit   = 0;
  // The same for ERR_RETURN, but counted per function: a function called
  // from a condition still returns on a failure inside it (zsh 5.9).
  int  noerrreturn = 0;
  bool err_reported = false; // ZERR already ran for the status in hand
  // Counts the function bodies, evals and sourced files run: a `! f` whose
  // f ran commands is not exempt from ERR_EXIT and ZERR, as in zsh 5.9.
  s::uint64_t code_runs = 0;
  bool ran_jobs    = false; // `jobs` ran (lets exit go on with stopped jobs)

  HashTable<ProgRef> functions{};  // shell functions: name → body
  // Where each function was defined: the script or sourced file, or the
  // shell's name ($0) for one typed in (`whence -v`).
  HashTable<str>     function_files{};
  // `autoload name`: loaded from $fpath when first called.
  uset<str>          autoloads{};
  // `disable name`, `disable -f name`: skipped when commands are looked up.
  uset<str>          disabled_builtins{};
  uset<str>          disabled_functions{};
  AliasTable         aliases{};
  vec<str>           dirstack{};   // pushd/popd, without $PWD ($dirstack)
  int                source_depth = 0;   // `.`/`source` files being read
  str                source_name{};      // the file being read, if any
  // Loadable and compiled-in modules (M10).  `modules.math_funcs()` feeds
  // the arithmetic evaluator; `modules.all_builtins()` is folded into the
  // builtin dispatch when a module registers new builtins.
  ModuleManager      modules{};

  // The executor running this context, set by its constructor, for the
  // builtins that run code (eval, source, command, …).
  Executor*       executor = nullptr;
  // The interactive shell's history, for fc/history/r; null without one.
  HistoryManager* history  = nullptr;
  // The line editor's keymaps, widgets and completion (bindkey, zle,
  // vared, compdef); the Executor makes one when none is given.
  Zle*            zle      = nullptr;

  vec<str> positional{};    // $1 … $N, also $@ $* $argv
  str      arg0 = "jfsh";   // $0
  Pid      shell_pid = ::getpid();  // $$, unchanged in subshells

  ShellHooks  hooks{};         // module hooks (zsh/zprof, zsh/watch)

  vec<int>    pipestatus{0};   // $pipestatus: each stage of the last pipeline
  // $funcstack and $functrace: the functions (and eval, sourced files)
  // being run, outermost first, each with where it was called from.
  struct Frame { str name, caller; };
  vec<Frame>  funcstack{};
  // getopts: $OPTIND, the place in the option word being read, $OPTARG.
  // A function starts its own count unless POSIX_BUILTINS is set.
  s::int64_t  optind  = 1;
  s::size_t   optcind = 0;
  str         optarg{};
  s::uint32_t lineno = 0;      // $LINENO: line of the command being run
  // The interactive loop added the line being run to the history before
  // running it, so `fc` and `r` leave that entry out.
  bool        hist_current = false;
  LoopState   loop{};
};

// Pushes a $funcstack frame for `name` while it lives.
struct FuncFrame {
  FuncFrame(ExecContext& c, str name);
  ~FuncFrame() { ctx.funcstack.pop_back(); }
  FuncFrame(const FuncFrame&) = delete;
  FuncFrame& operator=(const FuncFrame&) = delete;
  ExecContext& ctx;
};

// Gives `ctx.params` the parameters whose values the shell computes:
// $SECONDS, $RANDOM, $LINENO, $HISTCMD, $status, $pipestatus, $PPID,
// $EPOCHSECONDS and $EPOCHREALTIME.  The Executor constructor calls it.
void install_special_params(ExecContext& ctx);

// A `name=value` (or `name+=value`, `name=(…)`) word in front of a simple
// command.  For an array assignment `value` holds the source between the
// parentheses until expansion fills `elements`.
struct Assignment {
  str      name;
  str      value;
  bool     append = false;
  bool     array  = false;
  vec<str> elements;
  // `name[sub]=value`: the subscript, expanded, once `name` has been split
  // off it.
  opt<str> subscript;
  // An argument of typeset: how many of its other words come before it.
  s::size_t position = 0;
  // The subscript as written has a top-level comma (`a[2,3]=x`), as
  // opposed to one that came from an expansion (`i=2,3; a[$i]=x`).
  bool     subscript_comma = false;
  // The subscript as written is `@` or `*`, the whole parameter (`h[$k]=x`
  // with k=@ is the key `@`).
  bool     subscript_whole = false;
};

// A single redirection decoded from the bytecode.  `fd == -1` means the
// user did not write an explicit fd prefix; the executor picks the default
// for `type` (0 for the operators that read, 1 for the rest).
struct Redirection {
  RedirType type    = REDIR_READ;
  int       fd      = -1;
  str       target;
  bool      literal = false;   // here-doc with a quoted delimiter
  str       varid;             // `{name}>file`: the fd is new, put in $name
};

// RAII guard that restores a set of fds when it goes out of scope.
// Used around in-process builtins and shell functions so their redirections
// do not leak into the next command.  `fail` is set when any step of the
// setup failed; the caller treats this as exit status 1.
struct SavedFds {
  SavedFds() = default;
  SavedFds(const SavedFds&)            = delete;
  SavedFds& operator=(const SavedFds&) = delete;
  SavedFds(SavedFds&& other) noexcept
    : entries(s::move(other.entries)), tees(s::move(other.tees)), fail(other.fail) {
    other.entries.clear();
    other.tees.clear();
  }
  SavedFds& operator=(SavedFds&&) noexcept;
  ~SavedFds() { restore(); }

  // Duplicate `dst` into a new high fd (CLOEXEC) if not already saved, so
  // the destructor can restore it with dup2.  If `dst` was closed, records
  // that fact, and restore closes it again.
  void save(int dst);
  // Puts the fds back, then waits for the MULTIOS tees, which see the end
  // of their input once the fds writing to them are closed.
  void restore() noexcept;
  // Keeps the redirections: forgets the saved fds instead of restoring them
  // (`exec >file`); the tees go on running.
  void commit() noexcept;

  struct Entry { int dst; int backup; };
  vec<Entry> entries;
  vec<Pid>   tees;     // MULTIOS helpers forked for these redirections
  bool fail = false;
};

struct Executor {
  explicit Executor(ExecContext& ctx);
  ~Executor();

  int exec(const CompiledProgram& prog);
  // Parses and runs `src`.  By default the whole text is parsed first, as
  // for `-c` and `eval`; `by_line` parses and runs one line at a time, as
  // for scripts and sourced files, so an alias defined on one line is
  // used on the next.
  int exec_string(strv src, bool by_line = false);
  // Parses `src` as exec_string would, without running it.
  Result<ProgRef> parse_string(strv src);
  void            report_parse_error(const ShellError& e);
  // Runs a program parsed by parse_string as exec_string runs its text.
  int exec_parsed(const CompiledProgram& prog);
  // In a freshly forked child that runs shell code: signals and traps
  // reset for `kind` (SignalManager::child_reset), the parent's jobs
  // forgotten.  exit_child ends such a child with status `rc` (or its
  // `exit` / `return` status) after running an EXIT trap it set.
  void enter_child(ChildKind kind);
  [[noreturn]] void exit_child(int rc);
  // `.`/`source`: runs the file at `path` in this shell, with `args` (if
  // any) as the positional parameters meanwhile.  `name` is what errors
  // and $0 call it.
  int source_file(const str& path, const vec<str>& args, const str& name);
  // Defines autoloaded function `name` from the first file of that name
  // in $fpath.  Prints why and returns false when it can't.
  bool load_autoload(const str& name);
  // Runs `args` as an external command, bypassing functions and builtins
  // (`command`), in the shell's current fds.
  int run_command(const vec<str>& args) {
    return run_external(args, {}, {}, EF_NONE);
  }
  // The command hash table (`hash`, `rehash`), checked against $PATH.
  HashTable<str>& command_hash();
  // Fills the command hash with every executable in $PATH (`hash -r` is
  // clear, `rehash` refills).
  void fill_command_hash();
  // The command hash with every command in $PATH, filled when it isn't
  // already ($commands, as zsh's HASH_LIST_ALL).
  HashTable<str>& full_command_hash() {
    auto& hash = command_hash();
    if (not mHashFull or hash.empty()) fill_command_hash();
    return mCmdHash;
  }
  // `typeset_args` are the `name=value` arguments of typeset and its
  // variants: expanded like assignments, then passed on as `name=value`.
  int exec_simple(
    vec<str>         args,
    vec<Assignment>  assigns,
    vec<Redirection> redirs,
    ExecFlags flags = EF_NONE,
    vec<Assignment>  typeset_args = {});

  // Path to run for a command name: the name itself when it contains `/`,
  // otherwise a $PATH search (cached while HASH_CMDS is set).  Fails with
  // FileNotFound (exit 127) or PermissionDenied (exit 126).
  Result<str> find_command(strv name);

  // Acts on signals that arrived since the last call: reaps children into
  // the job table, runs traps, turns an untrapped Ctrl-C into `aborting`
  // and SIGHUP into an exit, and follows SIGWINCH into $COLUMNS/$LINES.
  // Called between commands, and by the REPL at the prompt.
  void handle_signals();
  // Runs the trap for `sig` (a real or pseudo-signal), keeping $?.
  void run_trap(int sig);
  // After waiting for a command: drops a signal whose trap is running, as
  // zsh does there (see the definition); with `backlog`, also the arrivals
  // queued before the trap started.
  void drop_trap_signals(bool backlog = true);
  // What an untrapped signal the shell handles itself does: SIGINT ends
  // the command line, SIGHUP the shell.
  void untrapped_signal(int sig);
  // Runs the DEBUG trap with ERR_EXIT off, as zsh does; true when the trap
  // set ERR_EXIT, which skips the command that follows.
  bool run_debug_trap();
  // While the shell runs the last stage of a pipeline under job control,
  // the other stages can't be suspended without it: when one was stopped
  // (Ctrl-Z), says so and continues them, as zsh does.
  void continue_pipeline_stages();
  // For a builtin blocked reading (`read`): called when the signal
  // self-pipe (SignalManager::wake_fd) is readable; handles the signals
  // (traps, reaping, continuing stopped pipeline stages) as between
  // commands.
  void wait_for_input();
  // The shell's EXIT trap won't run (`${x?}` in a function, as zsh's zexit).
  void drop_exit_trap() noexcept { mExitTrapRan = true; }
  // A trap's `return` at the prompt (outside any function): it ends there,
  // and $? is its status, as zsh's.  False when there was none.
  bool end_prompt_trap() noexcept;
  // Runs the EXIT trap once, as the shell exits with `status`; returns the
  // status to exit with (the trap may call `exit`).  `ran_off`: a -c
  // string that ended by itself, where zsh lets an `exit` in a signal trap
  // end the EXIT trap at once.
  int  run_exit_trap(int status, bool ran_off = false);
  // Sets $COLUMNS and $LINES from the terminal.
  void update_window_size();
  // What prompt expansion needs from this shell: PROMPT_SUBST's
  // expansion, the job count (%j) and the script name (%N).
  PromptHooks prompt_hooks();
  // `prompt` (PS3, PS4, `print -P`) expanded for output.
  str expand_prompt(strv prompt);
  // Job control is on: the shell owns a terminal and MONITOR is set.
  bool job_control() const noexcept {
    return mCtx.signals.job_control() and mCtx.opts.monitor();
  }

protected:
  int exec_list   (ExecState& es, ExecFlags);
  int exec_sublist(ExecState& es, ExecFlags);
  int exec_pline  (ExecState& es, ExecFlags);
  int exec_cmd    (ExecState& es, ExecFlags);
  int exec_simple_cmd(ExecState& es, ExecFlags);

  // Compound commands: es.pc is just past the header `head`, and is left
  // at the end of the construct.
  int exec_compound(ExecState& es, ExecFlags);
  int exec_if      (ExecState& es, WordCode head);
  int exec_while   (ExecState& es, WordCode head);
  // A loop that an error ended has status 1, as in zsh; one a TRAPxxx's
  // failure ended has the trap's (zsh's trapreturn breaks out of the
  // loops and leaves lastval).
  int loop_status(int rc) const noexcept {
    if (mCtx.aborting and mCtx.trap_status >= 0) return mCtx.last_exit;
    return mCtx.aborting and not mCtx.interrupt ? 1 : rc;
  }
  int exec_for     (ExecState& es, WordCode head);
  int exec_for_cond(ExecState& es, WordCode head);
  int exec_case    (ExecState& es, WordCode head);
  const CompiledPattern& cached_pattern(const str& pat);
  int exec_select  (ExecState& es, WordCode head);
  int exec_repeat  (ExecState& es, WordCode head);
  int exec_subsh   (ExecState& es, WordCode head, ExecFlags);
  int exec_funcdef (ExecState& es, WordCode head);
  int exec_cond    (ExecState& es, WordCode head);
  int exec_cond_node(ExecState& es, WordCode head);
  int exec_cond_leaf(ExecState& es, WordCode head);
  // `[[ s =~ re ]]`: PCRE under REMATCH_PCRE, else a POSIX ERE; with
  // BASH_REMATCH the match goes to $BASH_REMATCH alone.
  int cond_rematch(const str& lhs, const str& re);
  int exec_arith   (ExecState& es, WordCode head);
  int exec_always  (ExecState& es, WordCode head);

  // The values a `for`/`select` loops over; false (after printing the
  // error) when their expansion fails.
  bool loop_values(ExecState& es, WordCode head, vec<str>& names,
                   vec<str>& values);
  // True while a break, continue, return or exit is unwinding the lists.
  bool interrupted() const noexcept {
    return mCtx.exiting or mCtx.returning or mCtx.aborting or mCtx.break_level > 0;
  }
  // After a loop's body: whether the loop stops.  Consumes one level of a
  // pending `break N`/`continue N`.
  bool loop_done() noexcept;
  // A trap's `return` came while a for, case, repeat, select or `for (( ))`
  // word was expanded in a function: the command doesn't run and the
  // function returns 0, as zsh's.
  bool trap_returned_in_words() noexcept;

  int  do_fork(ExecState& es, ExecFlags) { return 0; }
  // Forks.  With job control the child joins process group `pgid` (0: a
  // new one of its own) and, when `foreground`, takes the terminal; then
  // it resets signals and traps for `kind` and forgets the parent's jobs.
  // Returns the pid, 0 in the child, -1 (after a message) on failure.
  pid_t fork_child(Pid pgid, bool foreground, ChildKind kind);
  // Command text of the pipeline being run, for job listings.
  str current_job_text() const;
  // The failing status `rc` of a command: ZERR, then ERR_EXIT/ERR_RETURN.
  void on_command_error(int rc);
  // `traced` is the command's xtrace text, printed (by the child) once
  // the assignments are made; nullptr when not tracing or already traced.
  int  run_external(
    const vec<str>&         args,
    const vec<Assignment>&  assigns,
    const vec<Redirection>& redirs,
    ExecFlags flags,
    const str* traced = nullptr);
  // The child's side of run_external: applies the redirections, makes the
  // assignments, finds the command (unless `found` already has) and execs.
  [[noreturn]] void exec_external_child(
    const vec<str>&         args,
    vec<Assignment>         assigns,
    const vec<Redirection>& redirs,
    opt<Result<str>>        found,
    const str*              traced,
    ExecFlags               flags);
  // Forks the process of a command that runs in one of its own (an
  // external command, a subshell).  As the last stage of a pipeline
  // (EF_PIPELAST) it joins the pipeline's process group.
  pid_t fork_command(ExecFlags flags, ChildKind kind);
  // Waits for the command fork_command started and returns its status; as
  // the last stage of a pipeline, leaves it to exec_pline (status 0).
  int   wait_command(pid_t pid);
  // Apply `redirs` in order in the current process, remembering originals
  // in `saved` so they can be restored.  Under MULTIOS several outputs to
  // one fd go through a tee.  `flags` may hold EF_STAGEOUT/EF_STAGEERR.
  // Prints the error and returns false when one fails.
  bool apply_redirects(const vec<Redirection>& redirs,
                       SavedFds& saved, ExecFlags flags = EF_NONE);
  // Process-substitution helpers.  <(cmd) → read-end fd; >(cmd) → write-end fd.
  // =(cmd) → path to a tmp file.  All fork a subshell internally.
  int procsubst_in (strv src);
  int procsubst_out(strv src);
  str procsubst_eq (strv src);
  // What the process substitutions among a command's words left behind:
  // the fds behind their /dev/fd/N paths, kept open while the command
  // runs, and the files of `=(cmd)`, removed afterwards.
  struct ProcSubstResources {
    vec<int> open_fds;
    vec<str> tmp_files;
  };
  void procsubst_release(ProcSubstResources& res) noexcept;
  // Expands the words, then the redirection targets, of a simple command
  // in place (src/exec/subst.cpp does the work); the assignments wait for
  // apply_assigns.  Prints the error and returns false when an expansion
  // fails.  `subst_status` receives the status of the last command
  // substitution, if any ran.  `fatal` is cleared when only a
  // here-document or here-string failed, which fails the command alone, as
  // in zsh.  `counts`, when given, receives how many words each of `args`
  // became.
  bool expand_command(vec<str>& args,
                      vec<Redirection>& redirs,
                      opt<int>&         subst_status,
                      bool&             fatal,
                      vec<s::size_t>*   counts = nullptr);
  // Where apply_assigns puts the assignments: the shell's parameters, or
  // (in front of a command) the current scope, which the caller pushed and
  // pops, optionally exported.
  enum class AssignTo : int { Shell, Prefix, Exported };
  // Expands each assignment and makes it before expanding the next, so a
  // later one sees the earlier ones (`a=1 b=$a`), as zsh's addvars does.
  // Prints the error and returns 1 on a failed expansion or a readonly
  // name.  `subst_status` receives the last command substitution's status.
  int  apply_assigns(vec<Assignment>& assigns, AssignTo to, opt<int>& subst_status);

public:
  // Makes one assignment already expanded (`name=v`, `name[sub]=v`,
  // `name=(…)`), as for ${name[sub]:=v}.
  Result<void> assign_expanded(const Assignment& a);
  // `name=value` as for, select and read make it: as an assignment does
  // (an integer parameter evaluates the value, `2` sets $2).  A failure is
  // reported and, as in zsh, ends the command line.
  bool assign_var(str name, str value);

protected:
  // The expanded PS4 that starts each xtrace line ("" when PS4 is unset).
  str  trace_prompt();
  // One xtrace line: PS4, then `text`, written to `fd`.
  void trace_text(strv text, int fd = 2);

public:
  // Runs a process substitution met in a word (`kind` is `<`, `>` or
  // `=`) and returns its path; its fd or file lasts until the command
  // that expanded it is done.
  str  procsubst(char kind, strv body);
  bool is_builtin(strv name) const noexcept;
  int  call_builtin(
    strv name,
    vec<str>& args,
    ExecContext& ctx);
  // Runs a shell function: `args[0]` is its name, the rest become $1 …
  // `returned`, when given, says whether it ended with `return`.
  int  call_function(
    const ProgRef& body,
    vec<str>& args,
    bool* returned = nullptr);
  // The builtins' names, sorted (whence -m, `builtin` listings).
  vec<str> builtin_names() const;

protected:
  // `exec cmd`: replaces the shell with `args`.  Returns only on failure,
  // with the status the shell then exits with.
  // `traced` is as for run_external, printed to `trace_to`.
  int  exec_replace(const vec<str>& args, vec<Assignment> assigns,
                    const str& argv0, bool clear_env, const str* traced,
                    int trace_to);
  // What exec_string does once the code has run: a top-level `return` and
  // an aborted line end the string.
  int end_string(int rc);
  // A trap's command string, run as exec_string would run it.
  int run_trap_command(const Trap& trap);

public:
  ExecContext& context() noexcept { return mCtx; }

private:
  // The pid a pipeline's last stage got when it forked (EF_PIPELAST), and
  // the `=(…)` files to remove once it is done.
  struct PipeLast {
    pid_t    pid = -1;
    vec<str> tmp_files{};
  };
  // Counts an exec_string or exec_parsed while it runs.
  struct StringDepth {
    explicit StringDepth(Executor& ex) : d(ex.mStringDepth) { ++d; }
    ~StringDepth() { --d; }
    StringDepth(const StringDepth&) = delete;
    StringDepth& operator=(const StringDepth&) = delete;
    int& d;
  };

  ExecContext& mCtx;
  // The pipeline exec_pline is running, for current_job_text().
  const CompiledProgram* mPipeProg = nullptr;
  s::size_t              mPipePc   = 0;
  PipeLast*              mPipeLast = nullptr;
  // Where the process substitutions of the command being expanded go, and
  // those made outside any command.
  ProcSubstResources*    mPsubst = nullptr;
  ProcSubstResources     mLooseSubst;
  // Under job control, the process group fork_child put its last child
  // in, for its job.
  Pid                    mChildPgid = 0;
  s::uint32_t            mProcSubsts = 0;   // `<( )` and `>( )` made so far

  s::array<bool, TRAP_TABLE_SIZE> mInTrap{};
  // Arrivals of a signal while its own trap ran (mQueued), and those that
  // came with the one that started it (mBacklog), for after it.
  s::array<s::uint16_t, MAX_SIG + 1> mQueued{};
  s::array<s::uint16_t, MAX_SIG + 1> mBacklog{};
  int            mTrapDepth  = 0;     // traps being run
  int            mStringDepth = 0;   // nested exec_string calls
  // A TRAPxxx run by handle_signals aborted with a status: {signal, status}.
  opt<s::pair<int, int>> mTrapAbort;
  // The status of an `exit` in a signal trap run from the EXIT trap, or -1.
  int            mExitFromTrap = -1;
  int            mExitTrapFuncDepth = 0;   // func_depth when the EXIT trap began
  bool           mExitTrapRan = false;
  bool           mInExitTrap  = false;
  bool           mExitTrapRanOff = false;   // run_exit_trap's ran_off
  // An `always` block ended with the error still set, and nothing came
  // after it: a child's EXIT trap still runs (exit_child).
  bool           mTryEndedChild = false;
  bool           mSigTrapInExit = false;   // run_trap: a signal trap in the EXIT trap
  bool           mExitInSigTrap = false;   // an `exit` in a function there
  bool           mExitEndsExitTrap = false;   // ... which now ends the EXIT trap
  bool           mTrapReturned = false;   // a trap handle_signals ran did `return`
  bool           mReturnAfterCommand = false;   // see exec_simple
  bool           mSelfSignal  = false;   // handle_signals after `kill` to the shell
  bool           mParseFailed = false;   // exec_string hit a parse error (source)
  // In a forked foreground pipeline stage, until its command starts: a
  // `( … )` there is reset as a subshell (exec_subsh).
  bool           mStageChild = false;
  bool           mHashFull  = false;   // fill_command_hash() since cleared
  s::uint8_t     mTrapChain = 0;   // run_trap handling what came during a trap
  str*           mCondTrace   = nullptr;   // xtrace text of the [[ ]] being run
  // `case` and `[[ = ]]` patterns compiled, by their text after expansion
  // and the options compile() takes (cached_pattern).
  HashTable<CompiledPattern> mPatterns;
  HashTable<str> mCmdHash;     // command name → resolved path
  str            mHashedPath;  // $PATH the hash was built against
  s::uint64_t    mHashedGen = 0;   // ParamTable::path_generation() then
  uptr<Zle>              mOwnZle;
  uptr<CompletionEngine> mOwnCompletion;
};

// Lets the line editor run shell functions (user widgets, compdef
// functions) and commands through `ex`, and tells completion about its
// builtins, functions and aliases (src/builtins/zle.cpp).
void install_zle_hooks(Executor& ex);

// While a completion function runs: binds $words, $CURRENT, $PREFIX, …
// and $compstate to `st` in the current scope, and has every function
// called meanwhile restore them on return as $compstate[restore] says
// (src/builtins/complete.cpp).  unbind_… drops that again.
void bind_completion_params(ExecContext& ctx, CompState& st);
void unbind_completion_params(ExecContext& ctx);

// ---- Job control (src/exec/jobctl.cpp) ---------------------------------------

// Waits for foreground job `jid` under job control: gives it the terminal,
// waits until every process is done or stopped, and takes the terminal
// back.  A stopped job stays in the table (reported as `jfsh: suspended
// cmd`) and gives 128 + the signal; a finished one is removed and gives its
// exit status.  Ctrl-C killing the job sets ctx.aborting.
int wait_foreground_job(ExecContext& ctx, int jid);

// Continues job `jid` (SIGCONT if stopped): in the foreground, `fg`, which
// then waits for it, or in the background, `bg`.  Prints the job line
// first, as zsh does.  Returns the status for $?.
int resume_job(ExecContext& ctx, int jid, bool foreground);

// Sends `sig` to every process of the job: to its process group when it
// has one of its own, otherwise to each live process.
Result<void> signal_job(const Job& job, int sig);

// Waits for one process of background job `jid` (`wait`).  Returns its
// status, or 128 + the signal when a trapped signal (or Ctrl-C under job
// control) interrupts the wait.
int wait_background_process(ExecContext& ctx, int jid, s::size_t proc_index);

// Sends SIGHUP to the running jobs an exiting interactive shell leaves
// behind, unless NO_HUP or disowned; after a SIGHUP of its own, stopped
// ones too (with SIGCONT).  Returns how many it signalled.
int hangup_jobs(ExecContext& ctx);

// ---- $PATH (src/exec/executor.cpp) -------------------------------------------

// The directories of $PATH, in order: none when PATH is unset, and an empty
// one (the current directory) for each empty component, so `PATH=` alone
// searches the current directory, as in zsh.
vec<str> path_dirs(const ParamTable& params);

}  // namespace jfsh

#endif  // JFSH_EXEC_EXECUTOR_H
