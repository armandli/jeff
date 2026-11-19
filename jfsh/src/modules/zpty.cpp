#include <jfsh/modules/zpty.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/modules/support.h>

// zsh/zpty (M11): commands on pseudo-terminals, as zsh's
// Src/Modules/zpty.c.  The command runs in a forked copy of the shell on
// the pty's slave side; the shell keeps the master.

namespace jfsh {

namespace {

struct Pty {
  str      name;
  vec<str> cmd;
  int      fd  = -1;        // master
  pid_t    pid = -1;
  bool     echo = false;
  bool     nonblock = false;
  bool     done = false;    // the command has been reaped
  int      status = 0;
  str      pending;         // read past the end of the last -r
};

vec<Pty>& ptys() {
  static vec<Pty> v;
  return v;
}

int err(strv msg) { return mod_error("zpty", msg); }

Pty* find(const str& name) {
  auto& v = ptys();
  auto it = s::find_if(v.begin(), v.end(), [&](const Pty& p) { return p.name == name; });
  return it == v.end() ? nullptr : &*it;
}

void reap(Pty& p, bool wait) {
  if (p.done or p.pid < 0) return;
  int st = 0;
  pid_t r;
  do r = ::waitpid(p.pid, &st, wait ? 0 : WNOHANG);
  while (r < 0 and errno == EINTR);
  if (r == p.pid) {
    p.done = true;
    p.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  }
}

void remove(Pty& p) {
  if (p.fd >= 0) ::close(p.fd);
  if (not p.done and p.pid > 0) {
    ::kill(p.pid, SIGHUP);
    reap(p, true);
  }
  auto& v = ptys();
  v.erase(v.begin() + (&p - v.data()));
}

str join(const vec<str>& words) {
  str out;
  for (const auto& w : words) out += (out.empty() ? "" : " ") + w;
  return out;
}

int start(ExecContext& ctx, const str& name, const vec<str>& cmd, bool echo, bool nonblock) {
  if (find(name)) return err("pty command name already used: " + name);
  s::fflush(nullptr);
  int master = -1;
  pid_t pid = ::forkpty(&master, nullptr, nullptr, nullptr);
  if (pid < 0) return err(str("can't open pseudo terminal: ") + s::strerror(errno));
  if (pid == 0) {
    if (not echo) {
      struct termios t{};
      if (::tcgetattr(STDIN_FILENO, &t) == 0) {
        t.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &t);
      }
    }
    for (int sig : {SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE, SIGHUP})
      ::signal(sig, SIG_DFL);
    int rc = ctx.executor ? ctx.executor->exec_string(join(cmd)) : 1;
    if (ctx.exiting) rc = ctx.exit_val;
    s::fflush(nullptr);
    ::_exit(rc);
  }
  ::fcntl(master, F_SETFD, FD_CLOEXEC);
  if (nonblock) ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
  Pty p;
  p.name = name;
  p.cmd  = cmd;
  p.fd   = master;
  p.pid  = pid;
  p.echo = echo;
  p.nonblock = nonblock;
  ptys().push_back(s::move(p));
  (void)ctx.params.set_scalar("REPLY", s::to_string(master));
  return 0;
}

bool write_all(int fd, strv data) {
  while (not data.empty()) {
    ssize_t n = ::write(fd, data.data(), data.size());
    if (n < 0 and (errno == EINTR or errno == EAGAIN)) {
      struct pollfd p{fd, POLLOUT, 0};
      ::poll(&p, 1, 100);
      continue;
    }
    if (n <= 0) return false;
    data.remove_prefix(static_cast<s::size_t>(n));
  }
  return true;
}

int do_write(Pty& p, const vec<str>& words, bool newline) {
  if (words.empty()) {
    char buf[4096];
    ssize_t n;
    while ((n = ::read(STDIN_FILENO, buf, sizeof buf)) > 0)
      if (not write_all(p.fd, strv(buf, static_cast<s::size_t>(n)))) return 1;
    return 0;
  }
  str text = join(words);
  if (newline) text += '\n';
  return write_all(p.fd, text) ? 0 : 1;
}

// One byte from the pty: 1 read, 0 nothing yet (with `wait_ms` 0), -1
// end of output (the slave side has closed).
int read_byte(Pty& p, char& c, int wait_ms) {
  if (not p.pending.empty()) {
    c = p.pending.front();
    p.pending.erase(0, 1);
    return 1;
  }
  for (;;) {
    struct pollfd pf{p.fd, POLLIN, 0};
    int r = ::poll(&pf, 1, wait_ms);
    if (r < 0 and errno == EINTR) continue;
    if (r == 0) return 0;
    ssize_t n = ::read(p.fd, &c, 1);
    if (n < 0 and (errno == EINTR or errno == EAGAIN)) {
      if (wait_ms == 0) return 0;
      continue;
    }
    return n == 1 ? 1 : -1;
  }
}

// -r: a line (through its newline), or with `pattern` everything up to
// the point where what was read matches it.  -t: don't wait for output
// that isn't there yet.
int do_read(ExecContext& ctx, Pty& p, const str& param, const str& pattern, bool test) {
  opt<CompiledPattern> pat;
  if (not pattern.empty())
    pat = CompiledPattern::compile(pattern, false, ctx.opts.extended_glob());
  str got;
  bool eof = false;
  for (;;) {
    char c;
    int wait = test and got.empty() ? 0 : -1;
    int r = read_byte(p, c, wait);
    if (r == 0) {
      if (got.empty()) return 1;
      break;
    }
    if (r < 0) { eof = true; break; }
    got += c;
    if (pat ? pat->matches(got) : c == '\n') break;
  }
  if (param.empty()) {
    s::fwrite(got.data(), 1, got.size(), stdout);
    s::fflush(stdout);
  } else {
    (void)ctx.params.set_scalar(param, got);
  }
  if (eof and got.empty()) return 2;
  return pat and not pat->matches(got) ? 1 : 0;
}

void list(bool script) {
  for (auto& p : ptys()) {
    reap(p, false);
    if (script)
      s::printf("zpty%s%s %s %s\n", p.echo ? " -e" : "", p.nonblock ? " -b" : "",
                p.name.c_str(), join(p.cmd).c_str());
    else
      s::printf("(%d) %s: %s\n", p.done ? -1 : static_cast<int>(p.pid), p.name.c_str(),
                join(p.cmd).c_str());
  }
}

int builtin_zpty(vec<str>& args, ExecContext& ctx) {
  bool echo = false, nonblock = false, del = false, wr = false, rd = false;
  bool test = false, script = false, nonl = false;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; break; }
    if (a.size() < 2 or a[0] != '-') break;
    for (char c : a.substr(1)) {
      switch (c) {
        case 'e': echo = true; break;
        case 'b': nonblock = true; break;
        case 'd': del = true; break;
        case 'w': wr = true; break;
        case 'r': rd = true; break;
        case 't': test = true; break;
        case 'L': script = true; break;
        case 'n': nonl = true; break;
        case 'm': break;   // -r -m: the pattern is matched as it stands
        default: return err(str("bad option: -") + c);
      }
    }
  }
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());
  if (del) {
    if (rest.empty()) {
      while (not ptys().empty()) remove(ptys().back());
      return 0;
    }
    int rc = 0;
    for (const auto& n : rest) {
      if (Pty* p = find(n)) remove(*p);
      else rc = err("no such pty command: " + n);
    }
    return rc;
  }
  if (wr or rd or (test and not rd)) {
    if (rest.empty()) return err("pty command name required");
    Pty* p = find(rest[0]);
    if (not p) return err("no such pty command: " + rest[0]);
    if (wr) return do_write(*p, vec<str>(rest.begin() + 1, rest.end()), not nonl);
    if (rd) return do_read(ctx, *p, rest.size() > 1 ? rest[1] : "",
                           rest.size() > 2 ? rest[2] : "", test);
    reap(*p, false);
    return p->done ? 1 : 0;
  }
  if (rest.empty()) { list(script); return 0; }
  if (rest.size() < 2) return err("command required");
  return start(ctx, rest[0], vec<str>(rest.begin() + 1, rest.end()), echo, nonblock);
}

}  // namespace

void register_zpty_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/zpty", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"zpty", builtin_zpty, BINF_NONE, 0});
    out.on_unload = [] {
      while (not ptys().empty()) remove(ptys().back());
    };
    out.feature_names = {"b:zpty"};
  });
}

}  // namespace jfsh
