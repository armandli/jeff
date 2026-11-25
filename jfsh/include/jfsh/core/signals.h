#ifndef JFSH_CORE_SIGNALS_H
#define JFSH_CORE_SIGNALS_H
// jfsh/core/signals.h — signal handling, traps and the shell's terminal.
//
// Mirrors zsh's signals.c / jobs.c (the terminal half).  Covers:
//   - one async-signal-safe handler for every signal the shell catches: it
//     sets a bit in an atomic pending set and writes a byte to a self-pipe,
//     and the shell acts on it later, between commands;
//   - the trap table (`trap 'cmd' SIG`, `trap '' SIG`, `TRAPINT() { … }`)
//     and the OS dispositions that follow from it and from job control;
//   - function-scoped trap frames (LOCAL_TRAPS, function-local EXIT traps);
//   - job control setup: the shell's own process group, the controlling
//     terminal, and its saved modes;
//   - resetting all of that in a forked child.

#include <csignal>
#include <cstdint>
#include <termios.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct JobTable;
struct CompiledProgram;

// What kind of child a fork made, for SignalManager::child_reset.  As
// zsh's entersubsh:
enum class ChildKind : s::uint8_t {
  // `( … )`, `$( … )`, `<( … )`, `>( … )`, `=( … )`, background jobs and
  // the stages of a background pipeline: signal traps and ignored signals
  // go back to their defaults (TRAPxxx functions stay), ZERR and DEBUG
  // stay, EXIT goes.
  Subshell,
  // A forked stage of a foreground pipeline: keeps every trap but EXIT.
  Stage,
  // External commands and helper processes: every trap goes, ignored
  // signals stay ignored (an exec keeps them).
  Other,
};

// Real signals are 1 … MAX_SIG.  Pseudo-signals live after them in the
// trap table.
inline constexpr int MAX_SIG = 64;

inline constexpr int SIGEXIT  = MAX_SIG + 1;
inline constexpr int SIGZERR  = MAX_SIG + 2;
inline constexpr int SIGDEBUG = MAX_SIG + 3;
inline constexpr int TRAP_TABLE_SIZE = MAX_SIG + 4;

constexpr bool is_real_signal(int sig) noexcept { return sig >= 1 and sig <= MAX_SIG; }

enum class TrapAction : s::uint8_t {
  Default,   // no trap: the shell's default disposition
  Ignore,    // trap '' SIG
  Command,   // trap 'cmd' SIG: run a command string
  Function,  // TRAPSIG() { … }: call that function with the signal number
};

struct Trap {
  str        command;              // only valid when action == Command
  // Command: the command parsed when the trap was set, so it isn't parsed
  // again each time it runs; also what `trap` lists.  Function: the
  // TRAPxxx function's body, so a function's frame can put back the one
  // it hid.  Null for a trap set without one.
  sptr<CompiledProgram> prog;
  TrapAction action  = TrapAction::Default;
  // Set as ERR rather than ZERR: listed under that name, as zsh does.
  bool       alias   = false;
};

// The trap table.  Pure bookkeeping: SignalManager installs the OS
// dispositions that go with it.
struct SignalTable {
  void        set(int sig, TrapAction action, str cmd = {},
                  sptr<CompiledProgram> prog = {}, bool alias = false);
  const Trap& get(int sig) const noexcept;
  bool        trapped(int sig) const noexcept {
    auto a = get(sig).action;
    return a == TrapAction::Command or a == TrapAction::Function;
  }

  // A function call opens a frame; set() remembers in the innermost frame
  // the first value each trap had before the function changed it.
  void push_frame() { mFrames.emplace_back(); }
  // Closes the frame.  When `restore` (LOCAL_TRAPS), every trap the
  // function changed gets its old value back; an EXIT trap set inside the
  // function is always function-local: it is handed back in `exit_trap`
  // for the caller to run, and the outer EXIT trap is restored.  Returns
  // each signal whose trap was put back (EXIT always, when the function
  // changed it) with the action it had in the function, so dispositions
  // and TRAPxxx functions can be redone.
  vec<s::pair<int, TrapAction>> pop_frame(bool restore, opt<Trap>& exit_trap);
  s::size_t depth() const noexcept { return mFrames.size(); }
  // The EXIT trap belongs to the current function (or there is none):
  // `trap` lists an outer one only outside functions, as zsh.
  bool exit_trap_here() const noexcept {
    if (mFrames.empty()) return true;
    const auto& saved = mFrames.back().saved;
    return s::any_of(saved.begin(), saved.end(),
                     [](const auto& e) { return e.first == SIGEXIT; });
  }

  // In a forked child of `kind` (see ChildKind), as zsh's entersubsh.
  // `keep_ignored` (POSIX_TRAPS) keeps `trap '' SIG` in a subshell.
  void reset_for_child(ChildKind kind, bool keep_ignored);
  void clear_all() {
    mTraps.fill(Trap{});
    mFrames.clear();
  }

  // "INT", "SIGINT", "2" → SIGINT; "EXIT" or "0" → SIGEXIT.  Names are
  // upper case, as zsh's getsignum takes them, unless `any_case`.
  static opt<int>    name_to_sig(strv name, bool any_case = false) noexcept;
  // A name without the SIG in front: "INT" → SIGINT.
  static opt<int>    bare_name_to_sig(strv name, bool any_case = false) noexcept;
  // The number zsh gives a signal (TRAPxxx's $1): real signals their
  // own, EXIT 0, ZERR and DEBUG the two after the last real signal.
  static int         zsh_number(int sig_no) noexcept;
  // Real signals and pseudo-signals; "UNKNOWN" for anything else.
  static const char* sig_to_name(int sig_no) noexcept;
  // What a job status line says about a signal: "terminated",
  // "suspended (tty input)" …
  static const char* sig_message(int sig_no) noexcept;

private:
  struct Frame {
    vec<s::pair<int, Trap>> saved;   // first old value per signal
  };
  s::array<Trap, TRAP_TABLE_SIZE> mTraps{};
  vec<Frame>                      mFrames;
};

// Signals that have arrived but not been handled yet.  Set from signal
// handlers, so every operation is a lock-free atomic.
// How many times each real signal arrived since it was last taken: zsh
// queues signals rather than merging them, so two arrivals run a trap
// twice.
using SignalCounts = s::array<s::uint16_t, MAX_SIG + 1>;

struct PendingSignals {
  s::atomic<s::uint64_t> lo{0};  // signals 0–63
  s::atomic<s::uint64_t> hi{0};  // signals 64–127
  s::array<s::atomic<s::uint16_t>, MAX_SIG + 1> count{};

  void mark(int sig)  noexcept;
  bool test(int sig)  const noexcept;
  bool any()          const noexcept {
    return (lo.load(s::memory_order_relaxed) | hi.load(s::memory_order_relaxed)) != 0;
  }
  // Takes every pending signal with its count (at least 1), leaving the
  // set empty.
  SignalCounts take() noexcept;
  void clear_all()          noexcept;
  void clear(int sig)       noexcept;
};

struct SignalManager {
  // Opens the self-pipe and installs the SIGCHLD handler.  An interactive
  // shell also takes job control: its own process group, the terminal,
  // and the dispositions an interactive shell needs (SIGINT, SIGHUP and
  // SIGWINCH acted on; SIGQUIT, SIGTERM and SIGTSTP caught and dropped;
  // SIGTTIN and SIGTTOU ignored).  Without a terminal job control stays
  // off.  `tty` is the shell's own terminal fd (zsh's SHTTY, opened by the
  // caller and kept open by it); -1 makes init look for one itself.
  void init(bool interactive, int tty = -1);

  // True when the shell owns a terminal and runs jobs in process groups.
  bool job_control()   const noexcept { return mJobControl; }
  int  tty_fd()        const noexcept { return mTtyFd; }
  Pid  shell_pgid()    const noexcept { return mShellPgid; }
  // Hands the terminal to process group `pgid`, or back to the shell.
  void give_terminal(Pid pgid) noexcept;
  void take_terminal() noexcept;
  // On the way out (or before `exec`), as zsh's release_pgrp: the
  // terminal and the process group go back to the group the shell was
  // started in, so the program that started it is in the foreground again.
  void release_terminal() noexcept;
  // The terminal modes the shell keeps between jobs.
  void save_shell_modes() noexcept;
  void restore_shell_modes() noexcept;
  bool read_modes(struct termios& t) const noexcept {
    return mTtyFd >= 0 and ::tcgetattr(mTtyFd, &t) == 0;
  }
  void write_modes(const struct termios& t) const noexcept;

  // In a freshly forked child: restore the dispositions the shell changed
  // for itself, drop traps (see SignalTable::reset_for_child), give up job
  // control, and open a self-pipe of its own.
  // Returns the signals the child keeps blocked (see the definition).
  sigset_t child_reset(ChildKind kind, bool keep_ignored = false) noexcept;

  // Sets a trap and installs the disposition that goes with it.  Fails
  // for an unknown signal number.
  // A signal trapped or reset is no longer the one ignored at start, as
  // zsh's settrap/unsettrap.
  void set_trap(int sig, TrapAction action, str cmd = {},
                sptr<CompiledProgram> prog = {}, bool alias = false) {
    mTraps.set(sig, action, s::move(cmd), s::move(prog), alias);
    scan_start();
    if (is_real_signal(sig)) mIgnoredAtStart[static_cast<s::size_t>(sig)] = false;
    install(sig);
    if (action == TrapAction::Command or action == TrapAction::Function) unblock(sig);
  }
  // Resets every trap to its default.
  void reset_traps();
  // See SignalTable::pop_frame; also redoes the dispositions.
  void push_trap_frame() { mTraps.push_frame(); }
  // `changed`, when given, gets what pop_frame returned.
  opt<Trap> pop_trap_frame(bool restore,
                           vec<s::pair<int, TrapAction>>* changed = nullptr);

  // While the trap for real signal `sig` runs (zsh's ZSIG_IGNORED), its
  // disposition stays as it is: a `trap - SIG` in the trap takes effect
  // when end_trap installs it.
  // `self_sent`: the shell sent itself the signal (`kill -USR1 $$`); zsh
  // runs such a trap from its signal queue, with the signal still blocked
  // for the subshells it forks.
  void begin_trap(int sig, bool self_sent = false) noexcept;
  void end_trap(int sig) noexcept;
  bool running_trap(int sig) const noexcept {
    return is_real_signal(sig) and mRunning[static_cast<s::size_t>(sig)];
  }
  // While waiting for a child, the signals a subshell keeps blocked come
  // through (zsh's sigsuspend waits with none blocked): a default one ends
  // the subshell, though the children it forked keep the block.
  void let_kept_through(bool on) const noexcept;
  // Acts on `sig` as its default disposition would: one that ends the
  // process does (SIGUSR1, SIGTERM …); one ignored by default does nothing.
  void raise_default(int sig) noexcept;
  void clear_pending(int sig) noexcept { mPending.clear(sig); }
  // Signals the shell acts on itself when untrapped instead of dying:
  // SIGHUP (it exits through its own path) and, in an interactive shell,
  // SIGINT.
  bool shell_handles(int sig) const noexcept {
    return sig == SIGHUP or (sig == SIGINT and (mJobControl or mInteractive))
        or (sig == SIGALRM and mInteractive);   // $TMOUT
  }

  // The handler: marks `sig` pending and wakes the self-pipe.
  void note(int sig) noexcept;
  bool has_pending() const noexcept { return mPending.any(); }
  bool pending(int sig) const noexcept { return mPending.test(sig); }
  // Takes the pending set and drains the self-pipe.
  SignalCounts take_pending() noexcept;
  // The first pending signal that has a trap, or 0.
  int  pending_trapped() const noexcept;

  // Reaps every child whose state changed (WNOHANG, with stops and
  // continues) into `jobs`.  Safe to call anytime.
  void reap(JobTable& jobs);
  // Drains the self-pipe, then reaps.  Kept for callers outside the
  // executor.
  void dispatch_pending();
  void dispatch_pending(JobTable& jobs);

  // Read end of the self-pipe: readable while a signal waits to be
  // handled.  The line editor polls it.
  int  wake_fd()            const noexcept { return mSelfPipe[0]; }
  int  self_pipe_write_fd() const noexcept { return mSelfPipe[1]; }

  // Signals process group `pgid`.  Refuses pgid <= 1, which would signal
  // init or the caller's own group.
  static Result<void> kill_job(Pid pgid, int sig) noexcept;

  SignalTable&       traps()       noexcept { return mTraps; }
  const SignalTable& traps() const noexcept { return mTraps; }

protected:
  void install(int sig) noexcept;
  // Records once which signals were ignored when the shell started.
  void scan_start() noexcept;
  // A subshell that traps a signal it inherited blocked (mKeepBlocked)
  // lets it through again, as zsh's.
  void unblock(int sig) noexcept;
  void open_self_pipe() noexcept;

private:
  SignalTable    mTraps;
  PendingSignals mPending;
  struct termios mShellModes{};
  int            mSelfPipe[2]  = {-1, -1};
  int            mTtyFd        = -1;
  Pid            mShellPgid    = -1;
  Pid            mOrigPgid     = -1;   // the group the shell was started in
  // Blocked in this process and its children: a subshell forked while a
  // trap for the signal ran (child_reset).
  sigset_t       mKeepBlocked{};
  // Signals that were ignored when the shell started stay ignored when
  // their trap is reset, as POSIX requires.
  s::array<bool, MAX_SIG + 1> mIgnoredAtStart{};
  s::array<bool, MAX_SIG + 1> mRunning{};   // begin_trap … end_trap
  s::array<bool, MAX_SIG + 1> mSelfSent{};  // begin_trap's self_sent
  bool           mInteractive  = false;
  bool           mJobControl   = false;
  bool           mHaveModes    = false;
  bool           mStartChecked = false;
  bool           mHaveKept     = false;   // mKeepBlocked isn't empty
  bool           mBorrowedTty  = false;   // mTtyFd is the caller's, not closed here
};

extern SignalManager Signals;

}  // namespace jfsh

#endif  // JFSH_CORE_SIGNALS_H
