#pragma once
// jfsh/exec/executor.hh — shell executor.
//
// Mirrors zsh's exec.c hierarchy:
//   execlist() → execpline() → execpline2() → execcmd() → execsimple()
// plus subshell, function call, and async execution paths.
//
// The executor walks a CompiledProgram's bytecode stream via ExecState and
// dispatches to the appropriate execution function for each WCOpcode.

#include <jfsh/core/types.hh>
#include <jfsh/core/wordcode.hh>
#include <jfsh/core/param.hh>
#include <jfsh/core/options.hh>
#include <jfsh/core/jobs.hh>
#include <jfsh/core/signals.hh>
#include <cstdint>
#include <string>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// ExecFlags — modifier bits passed down through the execution hierarchy
// ---------------------------------------------------------------------------
enum ExecFlags : std::uint32_t {
    EF_NONE      = 0,
    EF_ASYNC     = 1 << 0,  // run as background job
    EF_NOPRINT   = 1 << 1,  // don't print job info (used internally)
    EF_DASH      = 1 << 2,  // - prefix: login shell argv[0]
    EF_NOFORK    = 1 << 3,  // exec instead of fork (last cmd in subshell)
    EF_COPROC    = 1 << 4,  // set up coprocess pipes
};

// ---------------------------------------------------------------------------
// ExecContext — shared mutable state threaded through one top-level execution
// ---------------------------------------------------------------------------
struct ExecContext {
    ParamTable&   params;
    OptionState&  opts;
    JobTable&     jobs;
    SignalManager& signals;

    int   last_exit   = 0;    // $?
    Pid   last_bg_pid = -1;   // $!
    bool  break_level = 0;    // `break N` counter
    bool  cont_level  = 0;    // `continue N` counter
    bool  returning   = false; // `return` issued
    int   return_val  = 0;
    bool  exiting     = false; // `exit` issued
    int   exit_val    = 0;
};

// ---------------------------------------------------------------------------
// Executor
// ---------------------------------------------------------------------------
class Executor {
public:
    explicit Executor(ExecContext& ctx);

    // Execute a compiled program; returns last exit code.
    int exec(const CompiledProgram& prog);

    // Execute a string (parse + exec inline).
    int exec_string(std::string_view src);

    // Execute a simple command (after expansion).
    int exec_simple(std::vector<std::string> args,
                    std::vector<std::string> assigns,
                    ExecFlags flags = EF_NONE);

private:
    ExecContext& ctx_;

    // --- Main dispatch ---
    int exec_list   (ExecState& es, ExecFlags);
    int exec_sublist(ExecState& es, ExecFlags);
    int exec_pline  (ExecState& es, ExecFlags);
    int exec_cmd    (ExecState& es, ExecFlags);

    // --- Compound commands ---
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

    // --- Low-level ---
    int  do_fork(ExecState& es, ExecFlags);
    int  do_exec(std::vector<std::string>& args);
    void setup_redirects(ExecState& es);
    void expand_args(ExecState& es, std::vector<std::string>& out);
    void apply_assigns(const std::vector<std::string>& assigns, bool export_flag);

    // --- Builtin dispatch ---
    bool is_builtin(std::string_view name) const noexcept;
    int  call_builtin(std::string_view name,
                      std::vector<std::string>& args,
                      ExecContext& ctx);

    // --- Function call ---
    int  call_function(std::string_view name,
                       std::vector<std::string>& args);
};

} // namespace jfsh
