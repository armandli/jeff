#ifndef JFSH_CORE_SIGNALS_H
#define JFSH_CORE_SIGNALS_H
// jfsh/core/signals.h — signal handling and trap management.
//
// Mirrors zsh's signal.c / trap.c.  Covers:
//   - Installing signal handlers (sigaction wrappers)
//   - Per-signal trap strings (trap 'cmd' SIGNAME)
//   - Deferred signal queue (signals received during critical sections)
//   - SIGCHLD-driven job reaping

#include <csignal>
#include <cstdint>

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <jfsh/core/types.h>

namespace jfsh {

inline constexpr int MAX_SIG = 64;

// Pseudo-signals (zsh-style, stored after real ones in the trap table)
inline constexpr int SIGEXIT  = MAX_SIG + 1;
inline constexpr int SIGZERR  = MAX_SIG + 2;
inline constexpr int SIGDEBUG = MAX_SIG + 3;
inline constexpr int TRAP_TABLE_SIZE = MAX_SIG + 4;

enum class TrapAction : std::uint8_t {
  Default,  // SIG_DFL
  Ignore,   // SIG_IGN
  Command,  // run a shell command string
};

struct Trap {
  TrapAction  action  = TrapAction::Default;
  std::string command;              // only valid when action == Command
  bool        local   = false;      // was this trap set inside a function?
  bool        changed = false;      // local override of an outer trap
};

struct SignalTable {
  SignalTable();

  void set_trap(int sig_idx, TrapAction action, std::string cmd = {});
  void clear_trap(int sig_idx);

  const Trap& get_trap(int sig_idx) const noexcept;

  void push_local_traps();
  void pop_local_traps();

  void run_exit_trap();
  void run_zerr_trap(int code);
  void run_debug_trap();

  static std::optional<int> name_to_sig(std::string_view name) noexcept;
  static const char*        sig_to_name(int sig_no)            noexcept;

private:
  std::array<Trap, TRAP_TABLE_SIZE>              mTraps{};
  std::vector<std::array<Trap, TRAP_TABLE_SIZE>> mSavedTraps;
};

// Bit-set: one bit per signal (we have <= 68 signals, fits in two 64-bit words).
struct PendingSignals {
  std::uint64_t lo = 0;  // signals 0–63
  std::uint64_t hi = 0;  // signals 64–67 (pseudo-signals)

  void mark(int sig)  noexcept;
  void clear(int sig) noexcept;
  bool test(int sig)  const noexcept;
  bool any()          const noexcept { return lo | hi; }
  void clear_all()          noexcept { lo = hi = 0; }
};

struct SignalManager {
  void init(bool interactive);

  void block();
  void unblock();
  bool is_blocked() const noexcept { return mBlockDepth > 0; }

  // Called from the SIGCHLD handler (async-signal-safe): writes one byte
  // into a self-pipe so dispatch_pending() can reap at a later point.
  void note_sigchld() noexcept;

  // Drain the self-pipe and run waitpid in a non-blocking loop, feeding
  // each exit to JobTable::update.  Safe to call anytime; no-op when the
  // self-pipe has no pending byte.
  void dispatch_pending();

  // Write-side fd of the self-pipe.  Public so the signal handler can
  // find it via the Signals global without additional state.
  int self_pipe_write_fd() const noexcept { return mSelfPipe[1]; }

  static Result<void> kill_job(Pid pgid, int sig) noexcept;

  SignalTable& traps() noexcept { return mTraps; }

private:
  SignalTable    mTraps;
  PendingSignals mPending;
  int            mBlockDepth    = 0;
  int            mSelfPipe[2]   = {-1, -1};
  bool           mInteractive   = false;
};

extern SignalManager Signals;

}  // namespace jfsh

#endif  // JFSH_CORE_SIGNALS_H
