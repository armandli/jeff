#ifndef JFSH_EXEC_EXECUTOR_HH
#define JFSH_EXEC_EXECUTOR_HH
// jfsh/exec/executor.hh — shell executor.
//
// Mirrors zsh's exec.c hierarchy:
//   execlist() → execpline() → execpline2() → execcmd() → execsimple()
// plus subshell, function call, and async execution paths.

#include <cstdint>

#include <string>
#include <vector>

#include <jfsh/core/jobs.hh>
#include <jfsh/core/options.hh>
#include <jfsh/core/param.hh>
#include <jfsh/core/signals.hh>
#include <jfsh/core/types.hh>
#include <jfsh/core/wordcode.hh>

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

struct Executor {
  explicit Executor(ExecContext& ctx);

  int exec(const CompiledProgram& prog);
  int exec_string(std::string_view src);
  int exec_simple(
    std::vector<std::string> args,
    std::vector<Assignment>  assigns,
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
    const std::vector<Assignment>&  assigns);
  void setup_redirects(ExecState& es);
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

#endif  // JFSH_EXEC_EXECUTOR_HH
