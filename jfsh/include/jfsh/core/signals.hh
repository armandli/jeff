#pragma once
// jfsh/core/signals.hh — signal handling and trap management.
//
// Mirrors zsh's signal.c / trap.c.  Covers:
//   - Installing signal handlers (sigaction wrappers)
//   - Per-signal trap strings (trap 'cmd' SIGNAME)
//   - Deferred signal queue (signals received during critical sections)
//   - SIGCHLD-driven job reaping

#include <jfsh/core/types.hh>
#include <csignal>
#include <cstdint>
#include <optional>
#include <string>
#include <array>

namespace jfsh {

// ---------------------------------------------------------------------------
// Signal indices — a small abstraction so the rest of the shell uses names
// rather than raw POSIX numbers, and so SIGEXIT / SIGZERR / SIGDEBUG fit in.
// ---------------------------------------------------------------------------

// Max real-time signal index we'll handle (SIGRTMAX is usually 64).
inline constexpr int kMaxSig = 64;

// Pseudo-signals (zsh-style, stored after real ones in the trap table)
inline constexpr int SIGEXIT  = kMaxSig + 1;  // EXIT trap  (runs on exit)
inline constexpr int SIGZERR  = kMaxSig + 2;  // ZERR trap  (runs on non-zero exit)
inline constexpr int SIGDEBUG = kMaxSig + 3;  // DEBUG trap (runs before each command)
inline constexpr int kTrapTableSize = kMaxSig + 4;

// ---------------------------------------------------------------------------
// TrapAction — what should happen when a signal fires
// ---------------------------------------------------------------------------
enum class TrapAction : std::uint8_t {
    Default,   // SIG_DFL  — kernel default action
    Ignore,    // SIG_IGN
    Command,   // run a shell command string
};

struct Trap {
    TrapAction  action  = TrapAction::Default;
    std::string command;              // only valid when action == Command
    bool        local   = false;      // was this trap set inside a function?
    bool        changed = false;      // local override of an outer trap
};

// ---------------------------------------------------------------------------
// SignalTable — holds the trap for every signal + pseudo-signal
// ---------------------------------------------------------------------------
class SignalTable {
public:
    SignalTable();

    // Install a trap.  sig_idx is a real POSIX signal or one of the
    // SIGEXIT/SIGZERR/SIGDEBUG pseudo-signals.
    void set_trap(int sig_idx, TrapAction action, std::string cmd = {});
    void clear_trap(int sig_idx);

    const Trap& get_trap(int sig_idx) const noexcept;

    // Save/restore traps across function calls (for LOCAL_TRAPS option).
    void push_local_traps();
    void pop_local_traps();

    // Run the EXIT trap string (called by shell exit).
    void run_exit_trap();
    // Run the ZERR trap string (called after non-zero exit).
    void run_zerr_trap(int code);
    // Run the DEBUG trap string (called before each command).
    void run_debug_trap();

    // Translate signal name ↔ number ("SIGINT" ↔ SIGINT, "INT" ↔ SIGINT).
    static std::optional<int> name_to_sig(std::string_view name) noexcept;
    static const char*        sig_to_name(int sig_no)            noexcept;

private:
    std::array<Trap, kTrapTableSize> traps_{};
    // Stacked local-trap saves (for functions with LOCAL_TRAPS).
    std::vector<std::array<Trap, kTrapTableSize>> saved_traps_;
};

// ---------------------------------------------------------------------------
// Deferred signal queue — signals that arrived during a critical section
// ---------------------------------------------------------------------------

// Bit-set: one bit per signal (we have <= 68 signals, fits in two 64-bit words).
struct PendingSignals {
    std::uint64_t lo = 0;  // signals 0-63
    std::uint64_t hi = 0;  // signals 64-67 (pseudo-signals)

    void mark(int sig)     noexcept;
    void clear(int sig)    noexcept;
    bool test(int sig)     const noexcept;
    bool any()             const noexcept { return lo | hi; }
    void clear_all()       noexcept { lo = hi = 0; }
};

// ---------------------------------------------------------------------------
// SignalManager — the shell's central signal subsystem
// ---------------------------------------------------------------------------
class SignalManager {
public:
    // Initialize: install SIGCHLD / SIGTERM / SIGINT / SIGQUIT handlers.
    void init(bool interactive);

    // Enter a signal-deferring critical section.  Signals are queued.
    void block();
    // Leave critical section; dispatch any pending signals.
    void unblock();
    bool is_blocked() const noexcept { return block_depth_ > 0; }

    // Called from SIGCHLD handler: schedule a job reap.
    void note_sigchld() noexcept { pending_.mark(SIGCHLD); }

    // Dispatch all pending signals; run trap commands.
    void dispatch_pending();

    // Send a signal to a job's process group.
    static Result<void> kill_job(Pid pgid, int sig) noexcept;

    SignalTable& traps() noexcept { return traps_; }

private:
    SignalTable  traps_;
    PendingSignals pending_;
    int          block_depth_ = 0;
};

// ---------------------------------------------------------------------------
// Global signal manager
// ---------------------------------------------------------------------------
extern SignalManager g_signals;

} // namespace jfsh
