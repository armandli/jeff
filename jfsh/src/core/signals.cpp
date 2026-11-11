#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jfsh/core/jobs.hh>
#include <jfsh/core/signals.hh>

namespace jfsh {

struct SigEntry { int num; const char* name; };

static const SigEntry SIG_NAMES[] = {
  {SIGHUP,   "HUP"},   {SIGINT,  "INT"},    {SIGQUIT, "QUIT"},
  {SIGILL,   "ILL"},   {SIGTRAP, "TRAP"},   {SIGABRT, "ABRT"},
  {SIGFPE,   "FPE"},   {SIGKILL, "KILL"},   {SIGSEGV, "SEGV"},
  {SIGPIPE,  "PIPE"},  {SIGALRM, "ALRM"},   {SIGTERM, "TERM"},
  {SIGUSR1,  "USR1"},  {SIGUSR2, "USR2"},   {SIGCHLD, "CHLD"},
  {SIGCONT,  "CONT"},  {SIGSTOP, "STOP"},   {SIGTSTP, "TSTP"},
  {SIGTTIN,  "TTIN"},  {SIGTTOU, "TTOU"},   {SIGURG,  "URG"},
  {SIGXCPU,  "XCPU"},  {SIGXFSZ, "XFSZ"},   {SIGVTALRM, "VTALRM"},
  {SIGPROF,  "PROF"},  {SIGWINCH, "WINCH"}, {SIGIO,   "IO"},
#ifdef SIGPWR
  {SIGPWR,   "PWR"},
#endif
#ifdef SIGINFO
  {SIGINFO,  "INFO"},
#endif
  {SIGSYS,   "SYS"},
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

  for (const auto* e = SIG_NAMES; e->name; ++e) {
    if (n == e->name) return e->num;
    std::string upper;
    for (char c : n)
      upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper == e->name) return e->num;
  }
  return std::nullopt;
}

const char* SignalTable::sig_to_name(int sig_no) noexcept {
  for (const auto* e = SIG_NAMES; e->name; ++e)
    if (e->num == sig_no) return e->name;
  return "UNKNOWN";
}

SignalTable::SignalTable() = default;

void SignalTable::set_trap(int sig_idx, TrapAction action, std::string cmd) {
  if (sig_idx < 0 or sig_idx >= TRAP_TABLE_SIZE) return;
  auto& t = mTraps[static_cast<std::size_t>(sig_idx)];
  t.action  = action;
  t.command = std::move(cmd);
  if (sig_idx < MAX_SIG) {
    switch (action) {
      case TrapAction::Default:
        std::signal(sig_idx, SIG_DFL);
      break; case TrapAction::Ignore:
        std::signal(sig_idx, SIG_IGN);
      break; case TrapAction::Command:
        // Real handler is set by SignalManager::init; Command only records the
        // trap string here.
      break; default:
        ;
    }
  }
}

void SignalTable::clear_trap(int sig_idx) {
  set_trap(sig_idx, TrapAction::Default);
}

const Trap& SignalTable::get_trap(int sig_idx) const noexcept {
  static const Trap kDefault{};
  if (sig_idx < 0 or sig_idx >= TRAP_TABLE_SIZE) return kDefault;
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

// SIGCHLD handler.  Must be async-signal-safe: only writes one byte to the
// self-pipe, which dispatch_pending() will later drain.  Using the pipe
// means we never call waitpid or malloc from a signal handler context.
static void sigchld_handler(int) {
  int err = errno;
  char byte = 1;
  int fd = Signals.self_pipe_write_fd();
  if (fd >= 0) {
    ssize_t n;
    do { n = ::write(fd, &byte, 1); } while (n < 0 and errno == EINTR);
    (void)n;
  }
  errno = err;
}

void SignalManager::note_sigchld() noexcept { mPending.mark(SIGCHLD); }

void SignalManager::init(bool interactive) {
  mInteractive = interactive;

  // Open a self-pipe (both ends CLOEXEC + nonblocking so handlers never
  // stall and children never inherit the parent's drain fds).  Fallback
  // for platforms without pipe2 (BSDs, old glibc) uses pipe + fcntl.
  int pfd[2] = {-1, -1};
#if defined(O_CLOEXEC) && defined(__linux__)
  if (::pipe2(pfd, O_CLOEXEC | O_NONBLOCK) != 0) pfd[0] = pfd[1] = -1;
#else
  if (::pipe(pfd) == 0) {
    for (int fd : {pfd[0], pfd[1]}) {
      int fl = ::fcntl(fd, F_GETFL, 0);
      ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
      int fdfl = ::fcntl(fd, F_GETFD, 0);
      ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
    }
  }
#endif
  mSelfPipe[0] = pfd[0];
  mSelfPipe[1] = pfd[1];

  struct sigaction sa{};
  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  ::sigaction(SIGCHLD, &sa, nullptr);
}

void SignalManager::block()   { ++mBlockDepth; }

void SignalManager::unblock() {
  if (--mBlockDepth <= 0) {
    mBlockDepth = 0;
    dispatch_pending();
  }
}

void SignalManager::dispatch_pending() {
  // Drain whatever bytes are queued in the self-pipe.  We do not care about
  // the count — each byte means "SIGCHLD fired at least once", and waitpid
  // in a loop below collects every child that actually exited.
  if (mSelfPipe[0] >= 0) {
    char buf[64];
    while (::read(mSelfPipe[0], buf, sizeof buf) > 0) {}
  }

  // Reap every child whose state has changed.
  for (;;) {
    int status = 0;
    pid_t pid = ::waitpid(-1, &status, WNOHANG | WUNTRACED);
    if (pid <= 0) break;
    Jobs.update(pid, status);
  }
}

Result<void> SignalManager::kill_job(Pid pgid, int sig) noexcept {
  if (::kill(-pgid, sig) != 0)
    return std::unexpected(
      ShellError::from_errno(ErrorKind::ExecError, "kill"));
  return {};
}

}  // namespace jfsh
