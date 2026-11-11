#include <csignal>
#include <cstring>

#include <jfsh/core/signals.hh>

namespace jfsh {

struct SigEntry { int num; const char* name; };

static const SigEntry kSigNames[] = {
  {SIGHUP,   "HUP"},   {SIGINT,  "INT"},    {SIGQUIT, "QUIT"},
  {SIGILL,   "ILL"},   {SIGTRAP, "TRAP"},   {SIGABRT, "ABRT"},
  {SIGFPE,   "FPE"},   {SIGKILL, "KILL"},   {SIGSEGV, "SEGV"},
  {SIGPIPE,  "PIPE"},  {SIGALRM, "ALRM"},   {SIGTERM, "TERM"},
  {SIGUSR1,  "USR1"},  {SIGUSR2, "USR2"},   {SIGCHLD, "CHLD"},
  {SIGCONT,  "CONT"},  {SIGSTOP, "STOP"},   {SIGTSTP, "TSTP"},
  {SIGTTIN,  "TTIN"},  {SIGTTOU, "TTOU"},   {SIGURG,  "URG"},
  {SIGXCPU,  "XCPU"},  {SIGXFSZ, "XFSZ"},   {SIGVTALRM, "VTALRM"},
  {SIGPROF,  "PROF"},  {SIGWINCH, "WINCH"}, {SIGIO,   "IO"},
  {SIGPWR,   "PWR"},   {SIGSYS,  "SYS"},
  // Pseudo
  {SIGEXIT,  "EXIT"},  {SIGZERR, "ZERR"},   {SIGDEBUG, "DEBUG"},
  {0, nullptr},
};

std::optional<int> SignalTable::name_to_sig(std::string_view name) noexcept {
  std::string_view n = name;
  if (n.size() > 3
      and (n[0]=='S' or n[0]=='s')
      and (n[1]=='I' or n[1]=='i')
      and (n[2]=='G' or n[2]=='g'))
    n = n.substr(3);

  for (const auto* e = kSigNames; e->name; ++e) {
    if (n == e->name) return e->num;
    std::string upper;
    for (char c : n)
      upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == e->name) return e->num;
  }
  return std::nullopt;
}

const char* SignalTable::sig_to_name(int sig_no) noexcept {
  for (const auto* e = kSigNames; e->name; ++e)
    if (e->num == sig_no) return e->name;
  return "UNKNOWN";
}

SignalTable::SignalTable() = default;

void SignalTable::set_trap(int sig_idx, TrapAction action, std::string cmd) {
  if (sig_idx < 0 or sig_idx >= kTrapTableSize) return;
  auto& t = mTraps[static_cast<std::size_t>(sig_idx)];
  t.action  = action;
  t.command = std::move(cmd);
  if (sig_idx < kMaxSig) {
    switch (action) {
      case TrapAction::Default:
        std::signal(sig_idx, SIG_DFL); break;
      case TrapAction::Ignore:
        std::signal(sig_idx, SIG_IGN); break;
      case TrapAction::Command:
        break;  // real handler set by SignalManager::init
      default:
        break;
    }
  }
}

void SignalTable::clear_trap(int sig_idx) {
  set_trap(sig_idx, TrapAction::Default);
}

const Trap& SignalTable::get_trap(int sig_idx) const noexcept {
  static const Trap kDefault{};
  if (sig_idx < 0 or sig_idx >= kTrapTableSize) return kDefault;
  return mTraps[static_cast<std::size_t>(sig_idx)];
}

void SignalTable::push_local_traps() {
  mSavedTraps.push_back(mTraps);
  for (auto& t : mTraps) t.local = true;
}

void SignalTable::pop_local_traps() {
  if (not mSavedTraps.empty()) {
    mTraps = mSavedTraps.back();
    mSavedTraps.pop_back();
  }
}

void SignalTable::run_exit_trap()     { /* TODO: eval mTraps[SIGEXIT].command */ }
void SignalTable::run_zerr_trap(int)  { /* TODO: eval mTraps[SIGZERR].command */ }
void SignalTable::run_debug_trap()    { /* TODO: eval mTraps[SIGDEBUG].command */ }

void PendingSignals::mark(int sig) noexcept {
  if (sig < 64) lo |=  (1ULL << sig);
  else          hi |=  (1ULL << (sig - 64));
}

void PendingSignals::clear(int sig) noexcept {
  if (sig < 64) lo &= ~(1ULL << sig);
  else          hi &= ~(1ULL << (sig - 64));
}

bool PendingSignals::test(int sig) const noexcept {
  if (sig < 64) return (lo >> sig) & 1;
  return (hi >> (sig - 64)) & 1;
}

void SignalManager::init(bool /*interactive*/) {
  std::signal(SIGCHLD, [](int) { g_signals.note_sigchld(); });
}

void SignalManager::block()   { ++mBlockDepth; }

void SignalManager::unblock() {
  if (--mBlockDepth <= 0) {
    mBlockDepth = 0;
    dispatch_pending();
  }
}

void SignalManager::dispatch_pending() {
  if (mPending.test(SIGCHLD)) {
    mPending.clear(SIGCHLD);
    // TODO(signals/sigchld): Call waitpid(-1, &status, WNOHANG|WUNTRACED)
    //   in a loop until it returns 0 or -1; for each pid found call
    //   g_jobs.update(pid, status) to update job/process state, then
    //   g_jobs.notify_changes() if the NOTIFY option is set.
  }
  // TODO(signals/dispatch): For each set bit in mPending_ (other than SIGCHLD):
  //   1. Look up mTraps[sig].action
  //   2. If TrapAction::Command: push the command string onto an eval queue
  //      (do NOT call exec directly here — we may be in a signal handler context).
  //   3. After the pending set is cleared, drain the eval queue from the
  //      main REPL loop using exec_string().
  //   4. For SIGINT: set ctx_.break_level and clear pending input if interactive.
}

Result<void> SignalManager::kill_job(Pid pgid, int sig) noexcept {
  if (::kill(-pgid, sig) != 0)
    return std::unexpected(
      ShellError::from_errno(ErrorKind::ExecError, "kill"));
  return {};
}

}  // namespace jfsh
