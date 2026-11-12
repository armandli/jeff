#ifndef JFSH_EXEC_EXECUTOR_H
#define JFSH_EXEC_EXECUTOR_H
// jfsh/exec/executor.h — shell executor.
//
// Mirrors zsh's exec.c hierarchy:
//   execlist() → execpline() → execpline2() → execcmd() → execsimple()
// plus subshell, function call, and async execution paths.

#include <cstdint>

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
  bool break_level = 0;     // `break N` counter
  bool cont_level  = 0;     // `continue N` counter
  bool returning   = false;
  int  return_val  = 0;
  bool exiting     = false;
  int  exit_val    = 0;
};

// A `name=value` (or `name+=value`) word in front of a simple command.
struct Assignment {
  std::string name;
  std::string value;
  bool        append = false;
};

// A single redirection decoded from the bytecode.  `fd == -1` means the
// user did not write an explicit fd prefix; the executor picks the default
// for `type` (0 for reads, 1 for writes, 2 for stderr merges).
struct Redirection {
  RedirType   type   = REDIR_READ;
  int         fd     = -1;
  std::string target;
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

  int exec(const CompiledProgram& prog);
  int exec_string(std::string_view src);
  int exec_simple(
    std::vector<std::string> args,
    std::vector<Assignment>  assigns,
    std::vector<Redirection> redirs,
    ExecFlags flags = EF_NONE);

  // Path to run for a command name: the name itself when it contains `/`,
  // otherwise a $PATH search (cached while HASH_CMDS is set).  Fails with
  // FileNotFound (exit 127) or PermissionDenied (exit 126).
  Result<std::string> find_command(std::string_view name);

protected:
  int exec_list   (ExecState& es, ExecFlags);
  int exec_sublist(ExecState& es, ExecFlags);
  int exec_pline  (ExecState& es, ExecFlags);
  int exec_cmd    (ExecState& es, ExecFlags);
  int exec_simple_cmd(ExecState& es, ExecFlags);

  int exec_if      (ExecState& es);
  int exec_while   (ExecState& es);
  int exec_for     (ExecState& es);
  int exec_case    (ExecState& es);
  int exec_select  (ExecState& es);
  int exec_repeat  (ExecState& es);
  int exec_subsh   (ExecState& es, ExecFlags);
  int exec_funcdef (ExecState& es);
  int exec_cond    (ExecState& es);
  int exec_arith   (ExecState& es);
  int exec_always  (ExecState& es);

  int  do_fork(ExecState& es, ExecFlags);
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
  std::vector<std::string> expand_words(const std::vector<std::string>& words);
  std::string              expand_value(std::string_view value);
  int  apply_assigns(const std::vector<Assignment>& assigns);
  void trace(
    const std::vector<std::string>& args,
    const std::vector<Assignment>&  assigns);

  bool is_builtin(std::string_view name) const noexcept;
  int  call_builtin(
    std::string_view name,
    std::vector<std::string>& args,
    ExecContext& ctx);
  int  call_function(
    std::string_view name,
    std::vector<std::string>& args);

private:
  ExecContext& mCtx;
  HashTable<std::string> mCmdHash;     // command name → resolved path
  std::string            mHashedPath;  // $PATH the hash was built against
};

}  // namespace jfsh

#endif  // JFSH_EXEC_EXECUTOR_H
