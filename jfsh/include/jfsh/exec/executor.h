#ifndef JFSH_EXEC_EXECUTOR_H
#define JFSH_EXEC_EXECUTOR_H
// jfsh/exec/executor.h — shell executor.
//
// Mirrors zsh's exec.c hierarchy:
//   execlist() → execpline() → execpline2() → execcmd() → execsimple()
// plus subshell, function call, and async execution paths.

#include <cstdint>

#include <unistd.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/signals.h>
#include <jfsh/core/types.h>
#include <jfsh/core/wordcode.h>

namespace jfsh {

enum ExecFlags : std::uint32_t {
  EF_NONE    = 0,
  EF_ASYNC   = 1 << 0,  // run as background job
  EF_NOPRINT = 1 << 1,  // don't print job info (used internally)
  EF_DASH    = 1 << 2,  // - prefix: login shell argv[0]
  EF_NOFORK  = 1 << 3,  // exec instead of fork (last cmd in subshell)
  EF_COPROC  = 1 << 4,  // set up coprocess pipes
};

struct ExecContext {
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
  // Ctrl-C under job control, or a TRAPxxx function that failed: unwind
  // the command line.  An interactive shell goes back to the prompt, any
  // other shell exits.
  bool aborting    = false;
  bool hangup      = false; // SIGHUP reached an interactive shell
  // ERR_EXIT, ERR_RETURN and ZERR are off while this is positive: in the
  // conditions of if/while/until, after `!`, and in all but the last
  // pipeline of a && / || chain.
  int  noerrexit   = 0;
  // The same for ERR_RETURN, but counted per function: a function called
  // from a condition still returns on a failure inside it (zsh 5.9).
  int  noerrreturn = 0;
  bool err_reported = false; // ZERR already ran for the status in hand
  bool ran_jobs    = false; // `jobs` ran (lets exit go on with stopped jobs)

  HashTable<ProgRef> functions{};  // shell functions: name → body

  std::vector<std::string> positional{};    // $1 … $N, also $@ $* $argv
  std::string              arg0 = "jfsh";   // $0
  Pid                      shell_pid = ::getpid();  // $$, unchanged in subshells
};

// A `name=value` (or `name+=value`, `name=(…)`) word in front of a simple
// command.  For an array assignment `value` holds the source between the
// parentheses until expansion fills `elements`.
struct Assignment {
  std::string              name;
  std::string              value;
  bool                     append = false;
  bool                     array  = false;
  std::vector<std::string> elements;
};

// A single redirection decoded from the bytecode.  `fd == -1` means the
// user did not write an explicit fd prefix; the executor picks the default
// for `type` (0 for reads, 1 for writes, 2 for stderr merges).
struct Redirection {
  RedirType   type    = REDIR_READ;
  int         fd      = -1;
  std::string target;
  bool        literal = false;   // here-doc with a quoted delimiter
};

// RAII guard that restores a set of fds when it goes out of scope.
// Used around in-process builtins and shell functions so their redirections
// do not leak into the next command.  `fail` is set when any step of the
// setup failed; the caller treats this as exit status 1.
struct SavedFds {
  SavedFds() = default;
  SavedFds(const SavedFds&)            = delete;
  SavedFds& operator=(const SavedFds&) = delete;
  SavedFds(SavedFds&&) noexcept;
  SavedFds& operator=(SavedFds&&) noexcept;
  ~SavedFds();

  // Duplicate `dst` into a new high fd (CLOEXEC) if not already saved, so
  // the destructor can restore it with dup2.  If `dst` was closed, records
  // that fact and reopens with /dev/null → dst on restore so later
  // close(2)s see a valid fd in the slot.
  void save(int dst);
  void restore() noexcept;

  struct Entry { int dst; int backup; };
  std::vector<Entry> entries;
  bool fail = false;
};

struct Executor {
  explicit Executor(ExecContext& ctx);
  ~Executor();

  int exec(const CompiledProgram& prog);
  int exec_string(std::string_view src);
  // `typeset_args` are the `name=value` arguments of typeset and its
  // variants: expanded like assignments, then passed on as `name=value`.
  int exec_simple(
    std::vector<std::string> args,
    std::vector<Assignment>  assigns,
    std::vector<Redirection> redirs,
    ExecFlags flags = EF_NONE,
    std::vector<Assignment>  typeset_args = {});

  // Path to run for a command name: the name itself when it contains `/`,
  // otherwise a $PATH search (cached while HASH_CMDS is set).  Fails with
  // FileNotFound (exit 127) or PermissionDenied (exit 126).
  Result<std::string> find_command(std::string_view name);

  // Acts on signals that arrived since the last call: reaps children into
  // the job table, runs traps, turns an untrapped Ctrl-C into `aborting`
  // and SIGHUP into an exit, and follows SIGWINCH into $COLUMNS/$LINES.
  // Called between commands, and by the REPL at the prompt.
  void handle_signals();
  // Runs the trap for `sig` (a real or pseudo-signal), keeping $?.
  void run_trap(int sig);
  // Runs the EXIT trap once, as the shell exits with `status`; returns the
  // status to exit with (the trap may call `exit`).
  int  run_exit_trap(int status);
  // Sets $COLUMNS and $LINES from the terminal.
  void update_window_size();
  // Job control is on: the shell owns a terminal and MONITOR is set.
  bool job_control() const noexcept;

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
  int exec_for     (ExecState& es, WordCode head);
  int exec_for_cond(ExecState& es, WordCode head);
  int exec_case    (ExecState& es, WordCode head);
  int exec_select  (ExecState& es, WordCode head);
  int exec_repeat  (ExecState& es, WordCode head);
  int exec_subsh   (ExecState& es, WordCode head, ExecFlags);
  int exec_funcdef (ExecState& es, WordCode head);
  int exec_cond    (ExecState& es, WordCode head);
  int exec_arith   (ExecState& es, WordCode head);
  int exec_always  (ExecState& es, WordCode head);

  // The values a `for`/`select` loops over; false (after printing the
  // error) when their expansion fails.
  bool loop_values(ExecState& es, WordCode head, std::vector<std::string>& names,
                   std::vector<std::string>& values);
  // True while a break, continue, return or exit is unwinding the lists.
  bool interrupted() const noexcept;
  // After a loop's body: whether the loop stops.  Consumes one level of a
  // pending `break N`/`continue N`.
  bool loop_done() noexcept;

  int  do_fork(ExecState& es, ExecFlags);
  // Forks.  With job control the child joins process group `pgid` (0: a
  // new one of its own) and, when `foreground`, takes the terminal; then
  // it resets signals and traps for `kind` and forgets the parent's jobs.
  // Returns the pid, 0 in the child, -1 (after a message) on failure.
  pid_t fork_child(Pid pgid, bool foreground, ChildKind kind);
  // Command text of the pipeline being run, for job listings.
  std::string current_job_text() const;
  // The failing status `rc` of a command: ZERR, then ERR_EXIT/ERR_RETURN.
  void on_command_error(int rc);
  int  run_external(
    const std::vector<std::string>& args,
    const std::vector<Assignment>&  assigns,
    const std::vector<Redirection>& redirs,
    ExecFlags flags);
  // Apply `redirs` in the current process, remembering originals in `saved`
  // so they can be restored.  Returns true on success.
  bool apply_redirects(const std::vector<Redirection>& redirs,
                       SavedFds& saved);
  // Apply `redirs` in a freshly forked child (no fd save needed).
  // Returns true on success; false means the child should exit 1.
  bool apply_redirects_child(const std::vector<Redirection>& redirs);
  // Process-substitution helpers.  <(cmd) → read-end fd; >(cmd) → write-end fd.
  // =(cmd) → path to a tmp file.  All fork a subshell internally.
  int         procsubst_in (std::string_view src);
  int         procsubst_out(std::string_view src);
  std::string procsubst_eq (std::string_view src);
  // Resolve any `<(cmd)`, `>(cmd)`, `=(cmd)` tokens in `args` to actual paths
  // by forking the inner subshell and recording the parent fd that the child
  // will inherit.  Pipe-backed fds are kept open until the caller invokes
  // `procsubst_release` so the outer command can see /dev/fd/N; `=(cmd)`
  // creates a tmp file that must also be unlinked afterwards.
  struct ProcSubstResources {
    std::vector<int>         open_fds;
    std::vector<std::string> tmp_files;
  };
  ProcSubstResources expand_procsubst(std::vector<std::string>& args);
  void               procsubst_release(ProcSubstResources& res) noexcept;
  // Expands the words, assignments and redirection targets of a simple
  // command in place (src/exec/subst.cpp does the work).  Prints the error
  // and returns false when an expansion fails.  `subst_status` receives
  // the status of the last command substitution, if any ran.
  bool expand_command(std::vector<std::string>& args,
                      std::vector<Assignment>&  assigns,
                      std::vector<Redirection>& redirs,
                      std::optional<int>&       subst_status);
  int  apply_assigns(const std::vector<Assignment>& assigns);
  void trace(
    const std::vector<std::string>& args,
    const std::vector<Assignment>&  assigns);

  bool is_builtin(std::string_view name) const noexcept;
  int  call_builtin(
    std::string_view name,
    std::vector<std::string>& args,
    ExecContext& ctx);
  // Runs a shell function: `args[0]` is its name, the rest become $1 …
  int  call_function(
    const ProgRef& body,
    std::vector<std::string>& args);
  // Puts the assignments in front of a builtin or function into a new
  // scope, which the caller pops.  Returns non-zero on a readonly name.
  int  push_prefix_assigns(const std::vector<Assignment>& assigns, bool exported);

public:
  ExecContext& context() noexcept { return mCtx; }

private:
  ExecContext& mCtx;
  // The pipeline exec_pline is running, for current_job_text().
  const CompiledProgram* mPipeProg = nullptr;
  std::size_t            mPipePc   = 0;
  std::array<bool, TRAP_TABLE_SIZE> mInTrap{};
  int                    mTrapDepth  = 0;     // traps being run
  int                    mStringDepth = 0;   // nested exec_string calls
  bool                   mExitTrapRan = false;
  HashTable<std::string> mCmdHash;     // command name → resolved path
  std::string            mHashedPath;  // $PATH the hash was built against
};

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
int wait_background_process(ExecContext& ctx, int jid, std::size_t proc_index);

// Sends SIGHUP (and SIGCONT to stopped ones) to the jobs an exiting
// interactive shell leaves behind, unless NO_HUP or disowned.  Returns
// how many it signalled.
int hangup_jobs(ExecContext& ctx);

}  // namespace jfsh

#endif  // JFSH_EXEC_EXECUTOR_H
