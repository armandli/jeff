#include <jfsh/core/signals.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>

namespace jfsh {

namespace {

struct SigEntry { int num; const char* name; };

// Primary names first, in signal-number order where it matters: kill -l
// lists the first name of each number.  On Linux, zsh calls 6 IOT and 29
// POLL; ABRT and IO are accepted as aliases.
constexpr SigEntry SIG_NAMES[] = {
  {SIGHUP,   "HUP"},   {SIGINT,  "INT"},    {SIGQUIT, "QUIT"},
  {SIGILL,   "ILL"},   {SIGTRAP, "TRAP"},
#ifdef __linux__
  {SIGABRT,  "IOT"},
#endif
  {SIGABRT,  "ABRT"},
#ifdef SIGEMT
  {SIGEMT,   "EMT"},
#endif
  {SIGBUS,   "BUS"},   {SIGFPE,  "FPE"},    {SIGKILL, "KILL"},
  {SIGUSR1,  "USR1"},  {SIGSEGV, "SEGV"},   {SIGUSR2, "USR2"},
  {SIGPIPE,  "PIPE"},  {SIGALRM, "ALRM"},   {SIGTERM, "TERM"},
#ifdef SIGSTKFLT
  {SIGSTKFLT, "STKFLT"},
#endif
  {SIGCHLD,  "CHLD"},  {SIGCONT, "CONT"},   {SIGSTOP, "STOP"},
  {SIGTSTP,  "TSTP"},  {SIGTTIN, "TTIN"},   {SIGTTOU, "TTOU"},
  {SIGURG,   "URG"},   {SIGXCPU, "XCPU"},   {SIGXFSZ, "XFSZ"},
  {SIGVTALRM, "VTALRM"}, {SIGPROF, "PROF"}, {SIGWINCH, "WINCH"},
#ifdef __linux__
  {SIGIO,    "POLL"},
#endif
  {SIGIO,    "IO"},
#ifdef SIGPWR
  {SIGPWR,   "PWR"},
#endif
#ifdef SIGINFO
  {SIGINFO,  "INFO"},
#endif
  {SIGSYS,   "SYS"},
#ifdef __linux__
  {SIGCHLD,  "CLD"},
#endif
  // Pseudo-signals
  {SIGEXIT,  "EXIT"},  {SIGZERR, "ZERR"},   {SIGDEBUG, "DEBUG"},
  {SIGZERR,  "ERR"},
};

struct SigMessage { int num; const char* text; };

// zsh's sigmsg table: what `jobs` and job notifications say.
constexpr SigMessage SIG_MESSAGES[] = {
  {SIGHUP,  "hangup"},             {SIGINT,  "interrupt"},
  {SIGQUIT, "quit"},               {SIGILL,  "illegal hardware instruction"},
  {SIGTRAP, "trace trap"},         {SIGABRT, "abort"},
  {SIGBUS,  "bus error"},          {SIGFPE,  "floating point exception"},
  {SIGKILL, "killed"},             {SIGUSR1, "user-defined signal 1"},
  {SIGSEGV, "segmentation fault"}, {SIGUSR2, "user-defined signal 2"},
  {SIGPIPE, "broken pipe"},        {SIGALRM, "alarm"},
  {SIGTERM, "terminated"},
#ifdef SIGSTKFLT
  {SIGSTKFLT, "stack fault"},
#endif
  {SIGCHLD, "death of child"},     {SIGCONT, "continued"},
  {SIGSTOP, "suspended (signal)"}, {SIGTSTP, "suspended"},
  {SIGTTIN, "suspended (tty input)"}, {SIGTTOU, "suspended (tty output)"},
  {SIGURG,  "urgent condition"},   {SIGXCPU, "cpu limit exceeded"},
  {SIGXFSZ, "file size limit exceeded"}, {SIGVTALRM, "virtual time alarm"},
  {SIGPROF, "profile signal"},     {SIGWINCH, "window changed"},
  {SIGIO,   "i/o ready"},
#ifdef SIGPWR
  {SIGPWR,  "power fail"},
#endif
  {SIGSYS,  "invalid system call"},
};

// The signals an interactive shell handles differently from the default.
constexpr int JOB_CONTROL_SIGNALS[] = {
  SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGWINCH, SIGHUP,
};

bool equal_upper(strv a, strv b) noexcept {
  if (a.size() != b.size()) return false;
  for (s::size_t i = 0; i < a.size(); ++i)
    if (s::toupper(static_cast<unsigned char>(a[i])) != b[i]) return false;
  return true;
}

void on_signal(int sig) {
  int err = errno;
  Signals.note(sig);
  errno = err;
}

}  // namespace

// ---- SignalTable -------------------------------------------------------------

opt<int> SignalTable::name_to_sig(strv name, bool any_case) noexcept {
  if (name.empty()) return s::nullopt;
  if (s::all_of(name.begin(), name.end(),
                [](char c) { return s::isdigit(static_cast<unsigned char>(c)); })) {
    if (name.size() > 3) return s::nullopt;
    int n = 0;
    for (char c : name) n = n * 10 + (c - '0');
    if (n == 0) return SIGEXIT;
    if (n > MAX_SIG) return s::nullopt;
    return n;
  }
  strv n = name;
  if (n.size() > 3 and (any_case ? equal_upper(n.substr(0, 3), "SIG") : n.starts_with("SIG")))
    n = n.substr(3);
  return bare_name_to_sig(n, any_case);
}

opt<int> SignalTable::bare_name_to_sig(strv name, bool any_case) noexcept {
  for (const auto& e : SIG_NAMES)
    if (any_case ? equal_upper(name, e.name) : name == e.name) return e.num;
  return s::nullopt;
}

int SignalTable::zsh_number(int sig_no) noexcept {
  if (is_real_signal(sig_no)) return sig_no;
  if (sig_no == SIGEXIT) return 0;
  int last = 0;
  for (const auto& e : SIG_NAMES)
    if (is_real_signal(e.num)) last = s::max(last, e.num);
  return last + (sig_no == SIGZERR ? 1 : 2);
}

const char* SignalTable::sig_to_name(int sig_no) noexcept {
  for (const auto& e : SIG_NAMES)
    if (e.num == sig_no) return e.name;
  return "UNKNOWN";
}

const char* SignalTable::sig_message(int sig_no) noexcept {
  for (const auto& m : SIG_MESSAGES)
    if (m.num == sig_no) return m.text;
  return "unknown signal";
}

void SignalTable::set(int sig, TrapAction action, str cmd,
                      sptr<CompiledProgram> prog, bool alias) {
  if (sig < 0 or sig >= TRAP_TABLE_SIZE) return;
  auto& t = mTraps[static_cast<s::size_t>(sig)];
  if (not mFrames.empty()) {
    auto& saved = mFrames.back().saved;
    bool known = s::any_of(saved.begin(), saved.end(),
                           [&](const auto& e) { return e.first == sig; });
    if (not known) saved.emplace_back(sig, t);
  }
  bool command = action == TrapAction::Command;
  t.action  = action;
  t.command = command ? s::move(cmd) : str{};
  t.prog    = command or action == TrapAction::Function ? s::move(prog) : nullptr;
  t.alias   = alias;
}

const Trap& SignalTable::get(int sig) const noexcept {
  static const Trap kDefault{};
  if (sig < 0 or sig >= TRAP_TABLE_SIZE) return kDefault;
  return mTraps[static_cast<s::size_t>(sig)];
}

vec<s::pair<int, TrapAction>> SignalTable::pop_frame(bool restore, opt<Trap>& exit_trap) {
  vec<s::pair<int, TrapAction>> changed;
  exit_trap.reset();
  if (mFrames.empty()) return changed;
  Frame frame = s::move(mFrames.back());
  mFrames.pop_back();
  for (auto& [sig, old] : frame.saved) {
    auto& t = mTraps[static_cast<s::size_t>(sig)];
    if (sig == SIGEXIT) {
      if (t.action != TrapAction::Default) exit_trap = t;
      changed.emplace_back(sig, t.action);
      t = s::move(old);
      continue;
    }
    if (restore) {
      changed.emplace_back(sig, t.action);
      t = s::move(old);
    } else if (not mFrames.empty()) {
      // The change outlives this function; an outer LOCAL_TRAPS function
      // still needs the value from before it.
      auto& outer = mFrames.back().saved;
      bool known = s::any_of(outer.begin(), outer.end(),
                             [&](const auto& e) { return e.first == sig; });
      if (not known) outer.emplace_back(sig, s::move(old));
    }
  }
  return changed;
}

void SignalTable::reset_for_child(ChildKind kind, bool keep_ignored) {
  for (int sig = 0; sig < TRAP_TABLE_SIZE; ++sig) {
    auto& t = mTraps[static_cast<s::size_t>(sig)];
    bool real = is_real_signal(sig);
    bool keep = false;
    if (sig == SIGEXIT)                    keep = false;
    else if (kind == ChildKind::Stage)     keep = true;
    else if (kind == ChildKind::Subshell)  keep = not real or t.action == TrapAction::Function
                                               or (keep_ignored and t.action == TrapAction::Ignore);
    else                                   keep = real and t.action == TrapAction::Ignore;
    if (not keep) t = Trap{};
  }
  mFrames.clear();
}

// ---- PendingSignals ----------------------------------------------------------

static_assert(s::atomic<s::uint64_t>::is_always_lock_free,
              "the signal handler needs lock-free 64-bit atomics");
static_assert(s::atomic<s::uint16_t>::is_always_lock_free,
              "the signal handler needs lock-free 16-bit atomics");

void PendingSignals::mark(int sig) noexcept {
  if (sig < 0 or sig >= 128) return;
  // The count before the bit: take() reads the count of every bit it took.
  if (sig <= MAX_SIG) count[static_cast<s::size_t>(sig)].fetch_add(1, s::memory_order_relaxed);
  if (sig < 64) lo.fetch_or(1ULL << sig, s::memory_order_relaxed);
  else          hi.fetch_or(1ULL << (sig - 64), s::memory_order_relaxed);
}

SignalCounts PendingSignals::take() noexcept {
  // The counts are what arrived; the bits only say that something did, so
  // a stale bit takes nothing and an arrival is never counted twice.
  SignalCounts out{};
  lo.exchange(0, s::memory_order_relaxed);
  hi.exchange(0, s::memory_order_relaxed);
  for (int sig = 1; sig <= MAX_SIG; ++sig)
    out[static_cast<s::size_t>(sig)] =
        count[static_cast<s::size_t>(sig)].exchange(0, s::memory_order_relaxed);
  return out;
}

void PendingSignals::clear_all() noexcept {
  lo = 0;
  hi = 0;
  for (auto& c : count) c.store(0, s::memory_order_relaxed);
}

void PendingSignals::clear(int sig) noexcept {
  if (sig < 0 or sig >= 128) return;
  // The bit may stay: take() then finds nothing for it.
  if (sig <= MAX_SIG) {
    count[static_cast<s::size_t>(sig)].exchange(0, s::memory_order_relaxed);
    return;
  }
  if (sig < 64) lo.fetch_and(~(1ULL << sig), s::memory_order_relaxed);
  else          hi.fetch_and(~(1ULL << (sig - 64)), s::memory_order_relaxed);
}

bool PendingSignals::test(int sig) const noexcept {
  if (sig < 0 or sig >= 128) return false;
  if (sig >= 1 and sig <= MAX_SIG)
    return count[static_cast<s::size_t>(sig)].load(s::memory_order_relaxed) > 0;
  if (sig < 64) return (lo.load(s::memory_order_relaxed) >> sig) & 1;
  return (hi.load(s::memory_order_relaxed) >> (sig - 64)) & 1;
}

// ---- SignalManager -----------------------------------------------------------

void SignalManager::note(int sig) noexcept {
  mPending.mark(sig);
  int fd = mSelfPipe[1];
  if (fd >= 0) {
    char byte = 1;
    ssize_t n;
    do { n = ::write(fd, &byte, 1); } while (n < 0 and errno == EINTR);
    (void)n;
  }
}

void SignalManager::open_self_pipe() noexcept {
  if (mSelfPipe[0] >= 0) return;
  int pfd[2] = {-1, -1};
#if defined(__linux__)
  if (::pipe2(pfd, O_CLOEXEC | O_NONBLOCK) != 0) pfd[0] = pfd[1] = -1;
#else
  if (::pipe(pfd) == 0) {
    for (int fd : {pfd[0], pfd[1]}) {
      ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
      ::fcntl(fd, F_SETFD, ::fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
    }
  } else {
    pfd[0] = pfd[1] = -1;
  }
#endif
  // Kept on high fds, as zsh keeps its own: `exec 3<file` must not close
  // the read end, or the handler's next write kills the shell (SIGPIPE).
  for (int& fd : pfd) {
    if (fd < 0 or fd >= 10) continue;
    int high = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
    if (high < 0) continue;
    ::close(fd);
    fd = high;
  }
  mSelfPipe[0] = pfd[0];
  mSelfPipe[1] = pfd[1];
}

void SignalManager::begin_trap(int sig, bool self_sent) noexcept {
  if (not is_real_signal(sig)) return;
  mRunning[static_cast<s::size_t>(sig)]  = true;
  mSelfSent[static_cast<s::size_t>(sig)] = self_sent;
}

void SignalManager::end_trap(int sig) noexcept {
  if (not is_real_signal(sig)) return;
  mRunning[static_cast<s::size_t>(sig)] = false;
  install(sig);
}

void SignalManager::raise_default(int sig) noexcept {
  switch (sig) {
    // Ignored, or stopping, by default: nothing to do here.
    case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH: case SIGIO:
#ifdef SIGINFO
    case SIGINFO:
#endif
    case SIGTSTP: case SIGTTIN: case SIGTTOU: case SIGSTOP:
      return;
    default: ;
  }
  struct sigaction sa{};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  ::sigaction(sig, &sa, nullptr);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);
  ::sigprocmask(SIG_UNBLOCK, &set, nullptr);
  ::raise(sig);
}

void SignalManager::let_kept_through(bool on) const noexcept {
  if (mHaveKept) ::sigprocmask(on ? SIG_UNBLOCK : SIG_BLOCK, &mKeepBlocked, nullptr);
}

void SignalManager::unblock(int sig) noexcept {
  if (not is_real_signal(sig) or not sigismember(&mKeepBlocked, sig)) return;
  sigdelset(&mKeepBlocked, sig);
  mHaveKept = false;
  for (int other = 1; other <= MAX_SIG and not mHaveKept; ++other)
    mHaveKept = sigismember(&mKeepBlocked, other) == 1;
  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, sig);
  ::sigprocmask(SIG_UNBLOCK, &one, nullptr);
}

void SignalManager::scan_start() noexcept {
  if (mStartChecked) return;
  for (int s = 1; s <= MAX_SIG; ++s) {
    struct sigaction old{};
    if (::sigaction(s, nullptr, &old) == 0)
      mIgnoredAtStart[static_cast<s::size_t>(s)] = old.sa_handler == SIG_IGN;
  }
  mStartChecked = true;
}

void SignalManager::install(int sig) noexcept {
  if (not is_real_signal(sig) or sig == SIGKILL or sig == SIGSTOP) return;
  if (mRunning[static_cast<s::size_t>(sig)]) return;   // see begin_trap
  scan_start();
  struct sigaction sa{};
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = SIG_DFL;
  bool catch_it = false;
  bool restart  = false;
  const Trap& t = mTraps.get(sig);
  if (sig == SIGCHLD) {
    // Always caught: it is how finished jobs are noticed.
    catch_it = restart = true;
  } else if (t.action == TrapAction::Command or t.action == TrapAction::Function) {
    // No SA_RESTART: a trapped signal interrupts `wait` and `read`.
    catch_it = true;
    restart  = sig == SIGWINCH;
  } else if (t.action == TrapAction::Ignore) {
    sa.sa_handler = SIG_IGN;
  } else if (mJobControl) {
    // The shell ignores SIGQUIT, SIGTERM and SIGTSTP by catching and
    // dropping them: a signal that is caught, unlike an ignored one, stays
    // pending while blocked, so a child that gets one before it has reset
    // its dispositions still receives it.  SIGTTIN and SIGTTOU must really
    // be ignored for tcsetpgrp to work from the background.
    switch (sig) {
      case SIGINT: case SIGHUP:
        catch_it = true;
      break; case SIGWINCH: case SIGQUIT: case SIGTERM: case SIGTSTP:
        catch_it = restart = true;
      break; case SIGTTIN: case SIGTTOU:
        sa.sa_handler = SIG_IGN;
      break; default:
        ;
    }
  } else if (mIgnoredAtStart[static_cast<s::size_t>(sig)]) {
    sa.sa_handler = SIG_IGN;
  } else if (sig == SIGHUP) {
    // As zsh, a script hung up runs its EXIT trap and exits (status 1).
    catch_it = true;
  }
  if (catch_it) {
    // A child's self-pipe is opened when it first catches a signal.
    if (sig != SIGCHLD) open_self_pipe();
    sa.sa_handler = on_signal;
    sa.sa_flags   = restart ? SA_RESTART : 0;
  }
  ::sigaction(sig, &sa, nullptr);
}

void SignalManager::init(bool interactive, int tty) {
  mInteractive = interactive;
  open_self_pipe();
  install(SIGCHLD);
  install(SIGHUP);
  if (not interactive) {
    // zsh records a SIGQUIT ignored at start as `trap '' QUIT`.
    if (mIgnoredAtStart[static_cast<s::size_t>(SIGQUIT)]) mTraps.set(SIGQUIT, TrapAction::Ignore);
    return;
  }

  // Unlike a script, an interactive shell doesn't pass on the signals it
  // was started with ignored (SIGPIPE from a Python parent, SIGHUP from
  // nohup): as in zsh 5.9, its commands get the defaults and it handles
  // SIGHUP itself.
  for (int sig = 1; sig <= MAX_SIG; ++sig) {
    auto i = static_cast<s::size_t>(sig);
    if (not mIgnoredAtStart[i]) continue;
    mIgnoredAtStart[i] = false;
    if (sig != SIGCHLD) ::signal(sig, SIG_DFL);   // SIGCHLD keeps its handler
  }
  install(SIGHUP);

  // The terminal: the shell's own (shared, as zsh's SHTTY serves both
  // init_io and acquire_pgrp), else /dev/tty or stdin, kept on a high
  // close-on-exec fd.
  int high = tty;
  mBorrowedTty = tty >= 0;
  if (high < 0) {
    int fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0 and ::isatty(STDIN_FILENO)) fd = ::dup(STDIN_FILENO);
    if (fd < 0) return;
    high = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
    ::close(fd);
    if (high < 0) return;
  }
  if (not ::isatty(high)) {
    if (not mBorrowedTty) ::close(high);
    return;
  }

  // Started in the background (`jfsh &` from another shell): wait until
  // brought to the foreground.
  for (;;) {
    Pid fg = ::tcgetpgrp(high);
    if (fg < 0 or fg == ::getpgrp()) break;
    ::kill(-::getpgrp(), SIGTTIN);
  }

  mTtyFd      = high;
  mJobControl = true;
  for (int sig : JOB_CONTROL_SIGNALS) install(sig);

  // Our own process group, unless we already lead one (a session leader
  // can't call setpgid).
  mOrigPgid = ::getpgrp();
  if (::getpgrp() != ::getpid()) ::setpgid(0, 0);
  mShellPgid = ::getpgrp();
  if (::tcsetpgrp(mTtyFd, mShellPgid) != 0) {
    mJobControl = false;
    for (int sig : JOB_CONTROL_SIGNALS) install(sig);
    if (not mBorrowedTty) ::close(mTtyFd);
    mTtyFd = -1;
    return;
  }
  save_shell_modes();
}

void SignalManager::give_terminal(Pid pgid) noexcept {
  if (mTtyFd >= 0 and pgid > 0) ::tcsetpgrp(mTtyFd, pgid);
}

void SignalManager::take_terminal() noexcept {
  if (mTtyFd >= 0 and mShellPgid > 0) ::tcsetpgrp(mTtyFd, mShellPgid);
}

void SignalManager::release_terminal() noexcept {
  if (mOrigPgid <= 0 or mOrigPgid == mShellPgid) return;
  if (mTtyFd >= 0) ::tcsetpgrp(mTtyFd, mOrigPgid);
  ::setpgid(0, mOrigPgid);
  mShellPgid = mOrigPgid;
}

void SignalManager::save_shell_modes() noexcept {
  if (mTtyFd < 0) return;
  mHaveModes = ::tcgetattr(mTtyFd, &mShellModes) == 0;
  // Canonical input and echo always come back for the next command line,
  // as in zsh (`stty -echo` lasts only to the end of its line).
  if (mHaveModes) mShellModes.c_lflag |= ICANON | ECHO;
}

void SignalManager::restore_shell_modes() noexcept {
  if (mTtyFd < 0 or not mHaveModes) return;
  // Only when something changed: the REPL calls this before every prompt.
  struct termios now{};
  if (::tcgetattr(mTtyFd, &now) == 0 and now.c_iflag == mShellModes.c_iflag
      and now.c_oflag == mShellModes.c_oflag and now.c_cflag == mShellModes.c_cflag
      and now.c_lflag == mShellModes.c_lflag
      and s::memcmp(now.c_cc, mShellModes.c_cc, sizeof now.c_cc) == 0)
    return;
  ::tcsetattr(mTtyFd, TCSADRAIN, &mShellModes);
}

void SignalManager::write_modes(const struct termios& t) const noexcept {
  if (mTtyFd >= 0) ::tcsetattr(mTtyFd, TCSADRAIN, &t);
}

sigset_t SignalManager::child_reset(ChildKind kind, bool keep_ignored) noexcept {
  // A child forked while a signal's command trap runs: as in zsh, a
  // subshell starts with that signal blocked (and no trap for it); a
  // pipeline stage, an external command and a TRAPxxx function's children
  // get the trap's or the default disposition.
  // The block lasts: that subshell's own children keep it.  A pipeline
  // stage keeps the running marks for a `( … )` it is (exec_subsh).
  auto running = mRunning;
  if (kind != ChildKind::Stage) mRunning = {};
  if (kind == ChildKind::Subshell)
    for (int sig = 1; sig <= MAX_SIG; ++sig)
      if (running[static_cast<s::size_t>(sig)] and mSelfSent[static_cast<s::size_t>(sig)]
          and mTraps.get(sig).action == TrapAction::Command) {
        sigaddset(&mKeepBlocked, sig);
        mHaveKept = true;
      }
  sigset_t keep_blocked = mKeepBlocked;
  // Dispositions to redo: whatever a trap or job control changed, and in
  // a subshell the signals ignored when the shell started, which zsh's
  // subshells don't keep either (POSIX_TRAPS keeps only `trap ''`).
  s::array<bool, MAX_SIG + 1> redo{};
  for (int sig = 1; sig <= MAX_SIG; ++sig)
    if (mTraps.get(sig).action != TrapAction::Default) redo[static_cast<s::size_t>(sig)] = true;
  if (mJobControl)
    for (int sig : JOB_CONTROL_SIGNALS) redo[static_cast<s::size_t>(sig)] = true;
  if (kind == ChildKind::Subshell) {
    for (int sig = 1; sig <= MAX_SIG; ++sig) {
      auto i = static_cast<s::size_t>(sig);
      if (mIgnoredAtStart[i]) redo[i] = true;
      mIgnoredAtStart[i] = false;
    }
  }

  mJobControl  = false;
  mOrigPgid    = -1;
  mInteractive = false;
  if (mTtyFd >= 0) ::close(mTtyFd);
  mTtyFd = -1;
  mHaveModes = false;
  mTraps.reset_for_child(kind, keep_ignored);
  mPending.clear_all();
  // The parent's self-pipe must not see this process's signals; install
  // opens one of the child's own for a trap it keeps.
  for (int& fd : mSelfPipe) {
    if (fd >= 0) ::close(fd);
    fd = -1;
  }
  for (int sig = 1; sig <= MAX_SIG; ++sig) {
    auto i = static_cast<s::size_t>(sig);
    if (redo[i] or (running[i] and kind != ChildKind::Stage)) install(sig);
  }
  return keep_blocked;
}

void SignalManager::reset_traps() {
  s::array<bool, MAX_SIG + 1> redo{};
  for (int sig = 1; sig <= MAX_SIG; ++sig)
    redo[static_cast<s::size_t>(sig)] = mTraps.get(sig).action != TrapAction::Default;
  mTraps.clear_all();
  for (int sig = 1; sig <= MAX_SIG; ++sig)
    if (redo[static_cast<s::size_t>(sig)]) install(sig);
  mPending.clear_all();
}

opt<Trap> SignalManager::pop_trap_frame(bool restore,
                                        vec<s::pair<int, TrapAction>>* changed) {
  opt<Trap> exit_trap;
  auto sigs = mTraps.pop_frame(restore, exit_trap);
  for (const auto& [sig, _] : sigs) install(sig);
  if (changed) *changed = s::move(sigs);
  return exit_trap;
}

SignalCounts SignalManager::take_pending() noexcept {
  if (mSelfPipe[0] >= 0) {
    char buf[64];
    while (::read(mSelfPipe[0], buf, sizeof buf) > 0) {}
  }
  return mPending.take();
}

int SignalManager::pending_trapped() const noexcept {
  for (int sig = 1; sig <= MAX_SIG; ++sig)
    if (mPending.test(sig) and mTraps.trapped(sig)) return sig;
  return 0;
}

void SignalManager::reap(JobTable& jobs) {
  for (;;) {
    int status = 0;
    pid_t pid = ::waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
    if (pid < 0 and errno == EINTR) continue;
    if (pid <= 0) break;
    jobs.update(pid, status);
  }
}

void SignalManager::dispatch_pending() { dispatch_pending(Jobs); }

void SignalManager::dispatch_pending(JobTable& jobs) {
  if (mSelfPipe[0] >= 0) {
    char buf[64];
    while (::read(mSelfPipe[0], buf, sizeof buf) > 0) {}
  }
  reap(jobs);
}

Result<void> SignalManager::kill_job(Pid pgid, int sig) noexcept {
  if (pgid <= 1) {
    errno = ESRCH;
    return s::unexpected(ShellError::from_errno(ErrorKind::ExecError, "kill"));
  }
  if (::kill(-pgid, sig) != 0)
    return s::unexpected(ShellError::from_errno(ErrorKind::ExecError, "kill"));
  return {};
}

}  // namespace jfsh
