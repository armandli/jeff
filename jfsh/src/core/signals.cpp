#include <jfsh/core/signals.hh>
#include <csignal>
#include <cstring>

namespace jfsh {

// ---------------------------------------------------------------------------
// Signal name table
// ---------------------------------------------------------------------------
struct SigEntry { int num; const char* name; };
static const SigEntry kSigNames[] = {
    {SIGHUP,   "HUP"},   {SIGINT,  "INT"},   {SIGQUIT, "QUIT"},
    {SIGILL,   "ILL"},   {SIGTRAP, "TRAP"},  {SIGABRT, "ABRT"},
    {SIGFPE,   "FPE"},   {SIGKILL, "KILL"},  {SIGSEGV, "SEGV"},
    {SIGPIPE,  "PIPE"},  {SIGALRM, "ALRM"},  {SIGTERM, "TERM"},
    {SIGUSR1,  "USR1"},  {SIGUSR2, "USR2"},  {SIGCHLD, "CHLD"},
    {SIGCONT,  "CONT"},  {SIGSTOP, "STOP"},  {SIGTSTP, "TSTP"},
    {SIGTTIN,  "TTIN"},  {SIGTTOU, "TTOU"},  {SIGURG,  "URG"},
    {SIGXCPU,  "XCPU"},  {SIGXFSZ, "XFSZ"},  {SIGVTALRM,"VTALRM"},
    {SIGPROF,  "PROF"},  {SIGWINCH,"WINCH"},  {SIGIO,   "IO"},
    {SIGPWR,   "PWR"},   {SIGSYS,  "SYS"},
    // Pseudo
    {SIGEXIT,  "EXIT"},  {SIGZERR, "ZERR"},  {SIGDEBUG,"DEBUG"},
    {0, nullptr}
};

std::optional<int> SignalTable::name_to_sig(std::string_view name) noexcept {
    // Strip "SIG" prefix if present
    std::string_view n = name;
    if (n.size() > 3 &&
        (n[0]=='S'||n[0]=='s') && (n[1]=='I'||n[1]=='i') && (n[2]=='G'||n[2]=='g'))
        n = n.substr(3);
    for (const auto* e = kSigNames; e->name; ++e) {
        if (n == e->name) return e->num;
        // case-insensitive
        std::string upper;
        for (char c : n) upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (upper == e->name) return e->num;
    }
    return std::nullopt;
}

const char* SignalTable::sig_to_name(int sig_no) noexcept {
    for (const auto* e = kSigNames; e->name; ++e)
        if (e->num == sig_no) return e->name;
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// SignalTable
// ---------------------------------------------------------------------------

SignalTable::SignalTable() = default;

void SignalTable::set_trap(int sig_idx, TrapAction action, std::string cmd) {
    if (sig_idx < 0 || sig_idx >= kTrapTableSize) return;
    auto& t = traps_[static_cast<std::size_t>(sig_idx)];
    t.action  = action;
    t.command = std::move(cmd);
    // Install real signal handler
    if (sig_idx < kMaxSig) {
        switch (action) {
            case TrapAction::Default:
                std::signal(sig_idx, SIG_DFL);
                break;
            case TrapAction::Ignore:
                std::signal(sig_idx, SIG_IGN);
                break;
            case TrapAction::Command:
                // Real handler set by SignalManager::init
                break;
        }
    }
}

void SignalTable::clear_trap(int sig_idx) {
    set_trap(sig_idx, TrapAction::Default);
}

const Trap& SignalTable::get_trap(int sig_idx) const noexcept {
    static const Trap kDefault{};
    if (sig_idx < 0 || sig_idx >= kTrapTableSize) return kDefault;
    return traps_[static_cast<std::size_t>(sig_idx)];
}

void SignalTable::push_local_traps() {
    saved_traps_.push_back(traps_);
    for (auto& t : traps_) t.local = true;
}

void SignalTable::pop_local_traps() {
    if (!saved_traps_.empty()) {
        traps_ = saved_traps_.back();
        saved_traps_.pop_back();
    }
}

void SignalTable::run_exit_trap()          { /* TODO: eval traps_[SIGEXIT].command */ }
void SignalTable::run_zerr_trap(int)       { /* TODO: eval traps_[SIGZERR].command */ }
void SignalTable::run_debug_trap()         { /* TODO: eval traps_[SIGDEBUG].command */ }

// ---------------------------------------------------------------------------
// PendingSignals
// ---------------------------------------------------------------------------

void PendingSignals::mark(int sig) noexcept {
    if (sig < 64) lo |= (1ULL << sig);
    else          hi |= (1ULL << (sig - 64));
}

void PendingSignals::clear(int sig) noexcept {
    if (sig < 64) lo &= ~(1ULL << sig);
    else          hi &= ~(1ULL << (sig - 64));
}

bool PendingSignals::test(int sig) const noexcept {
    if (sig < 64) return (lo >> sig) & 1;
    return (hi >> (sig - 64)) & 1;
}

// ---------------------------------------------------------------------------
// SignalManager
// ---------------------------------------------------------------------------

void SignalManager::init(bool /*interactive*/) {
    // Basic SIGCHLD handler to note child state changes
    std::signal(SIGCHLD, [](int) {
        g_signals.note_sigchld();
    });
}

void SignalManager::block()   { ++block_depth_; }
void SignalManager::unblock() {
    if (--block_depth_ <= 0) {
        block_depth_ = 0;
        dispatch_pending();
    }
}

void SignalManager::dispatch_pending() {
    if (pending_.test(SIGCHLD)) {
        pending_.clear(SIGCHLD);
        // Reap children — called from main loop
    }
    // Other signals would be dispatched here
}

Result<void> SignalManager::kill_job(Pid pgid, int sig) noexcept {
    if (::kill(-pgid, sig) != 0)
        return std::unexpected(ShellError::from_errno(ErrorKind::ExecError, "kill"));
    return {};
}

} // namespace jfsh
