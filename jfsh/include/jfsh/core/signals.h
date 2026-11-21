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

#include <array>
#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct JobTable;

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
  TrapAction action  = TrapAction::Default;
  str        command;              // only valid when action == Command
};

// The trap table.  Pure bookkeeping: SignalManager installs the OS
// dispositions that go with it.
struct SignalTable {
  void        set(int sig, TrapAction action, str cmd = {});
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
  // the signals whose trap changed, so their dispositions can be redone.
  vec<int>  pop_frame(bool restore, opt<Trap>& exit_trap);
  s::size_t depth() const noexcept { return mFrames.size(); }

  // In a forked child: drop every trap except ignored signals, which
  // children inherit.  `keep_pseudo` keeps ZERR and DEBUG (a `( … )`
  // subshell runs them, a command substitution does not).
  void reset_for_child(bool keep_pseudo);
  void clear_all() {
    mTraps.fill(Trap{});
    mFrames.clear();
  }

  // "INT", "SIGINT", "int", "2" → SIGINT; "EXIT" or "0" → SIGEXIT.
  static opt<int>    name_to_sig(strv name)  noexcept;
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
struct PendingSignals {
  s::atomic<s::uint64_t> lo{0};  // signals 0–63
  s::atomic<s::uint64_t> hi{0};  // signals 64–127

  void mark(int sig)  noexcept;
  bool test(int sig)  const noexcept;
  bool any()          const noexcept {
    return (lo.load(s::memory_order_relaxed) | hi.load(s::memory_order_relaxed)) != 0;
  }
  // Takes every pending bit, leaving the set empty.
  s::array<s::uint64_t, 2> take() noexcept {
    return {lo.exchange(0, s::memory_order_relaxed),
            hi.exchange(0, s::memory_order_relaxed)};
  }
  void clear_all()          noexcept { lo = 0; hi = 0; }
};

// What kind of child a fork made, for SignalManager::child_reset.
enum class ChildKind : s::uint8_t {
  Subshell,   // `( … )`: keeps ZERR and DEBUG traps
  Other,      // pipeline stages, background jobs, external commands,
              // command and process substitution, here-doc writers
};

struct SignalManager {
  // Opens the self-pipe and installs the SIGCHLD handler.  An interactive
  // shell also takes job control: its own process group, the terminal,
  // and the dispositions an interactive shell needs (SIGINT, SIGHUP and
  // SIGWINCH acted on; SIGQUIT, SIGTERM and SIGTSTP caught and dropped;
  // SIGTTIN and SIGTTOU ignored).  Without a terminal job control stays
  // off.
  void init(bool interactive);

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
  void child_reset(ChildKind kind) noexcept;

  // Sets a trap and installs the disposition that goes with it.  Fails
  // for an unknown signal number.
  void set_trap(int sig, TrapAction action, str cmd = {}) {
    mTraps.set(sig, action, s::move(cmd));
    install(sig);
  }
  // Resets every trap to its default.
  void reset_traps();
  // See SignalTable::pop_frame; also redoes the dispositions.
  void push_trap_frame() { mTraps.push_frame(); }
  opt<Trap> pop_trap_frame(bool restore);

  // The handler: marks `sig` pending and wakes the self-pipe.
  void note(int sig) noexcept;
  bool has_pending() const noexcept { return mPending.any(); }
  bool pending(int sig) const noexcept { return mPending.test(sig); }
  // Takes the pending set and drains the self-pipe.
  s::array<s::uint64_t, 2> take_pending() noexcept;
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
  void open_self_pipe() noexcept;

private:
  SignalTable    mTraps;
  PendingSignals mPending;
  int            mSelfPipe[2]  = {-1, -1};
  bool           mInteractive  = false;
  bool           mJobControl   = false;
  int            mTtyFd        = -1;
  Pid            mShellPgid    = -1;
  Pid            mOrigPgid     = -1;   // the group the shell was started in
  struct termios mShellModes{};
  bool           mHaveModes    = false;
  // Signals that were ignored when the shell started stay ignored when
  // their trap is reset, as POSIX requires.
  s::array<bool, MAX_SIG + 1> mIgnoredAtStart{};
  bool           mStartChecked = false;
};

extern SignalManager Signals;

}  // namespace jfsh

#endif  // JFSH_CORE_SIGNALS_H
