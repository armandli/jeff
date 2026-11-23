// The `read` builtin.

// The splitting of the line into words is derived from zsh 5.9's bin_read
// (Src/builtin.c).  zsh's copyright and licence:
//
// The Z Shell is copyright (c) 1992-2017 Paul Falstad, Richard Coleman,
// Zoltán Hidvégi, Andrew Main, Peter Stephenson, Sven Wischnowsky, and
// others.  All rights reserved.  Individual authors, whether or not
// specifically named, retain copyright in all changes; in what follows, they
// are referred to as `the Zsh Development Group'.  This is for convenience
// only and this body has no legal status.  The Z shell is distributed under
// the following licence; any provisions made in individual files take
// precedence.
//
// Permission is hereby granted, without written agreement and without
// licence or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall the Zsh Development Group be liable to any party for
// direct, indirect, special, incidental, or consequential damages arising out
// of the use of this software and its documentation, even if the Zsh
// Development Group have been advised of the possibility of such damage.
//
// The Zsh Development Group specifically disclaim any warranties, including,
// but not limited to, the implied warranties of merchantability and fitness
// for a particular purpose.  The software provided hereunder is on an "as is"
// basis, and the Zsh Development Group have no obligation to provide
// maintenance, support, updates, enhancements, or modifications.

#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

namespace {

// Terminal modes for -s (no echo) and -k/-q (a character at a time), put
// back when the read ends.
struct TtyModes {
  int            fd = -1;
  struct termios saved{};

  void set(int f, bool no_echo, bool raw) {
    if (not (no_echo or raw) or not ::isatty(f)) return;
    struct termios t{};
    if (::tcgetattr(f, &t) != 0) return;
    fd = f;
    saved = t;
    if (no_echo) t.c_lflag &= ~static_cast<tcflag_t>(ECHO | ECHONL);
    // zsh's setcbreak: a character at a time, still echoed.
    if (raw) {
      t.c_lflag &= ~static_cast<tcflag_t>(ICANON);
      t.c_cc[VMIN]  = 1;
      t.c_cc[VTIME] = 0;
    }
    ::tcsetattr(fd, TCSADRAIN, &t);
  }
  ~TtyModes() {
    if (fd >= 0) ::tcsetattr(fd, TCSADRAIN, &saved);
  }
};

// Interrupted: Ctrl-C, or a trap that made the shell exit.
enum class Got : int { Char, End, Timeout, Interrupted };

using Clock = s::chrono::steady_clock;

// One byte from `fd`, waiting at most `timeout` seconds (negative: no
// limit).  With `watch` (a trap is set, or this is the last stage of a
// pipeline the shell runs itself under job control) the wait also polls
// the signal self-pipe, so a signal is handled at once
// (Executor::wait_for_input), with no window before the read: its trap
// runs (CHLD too), and a stage stopped by Ctrl-Z is continued.  Without a
// self-pipe (a subshell) a trapped signal's EINTR does the same.  A trap
// ends a `-t` read as a timeout, as in zsh.  `deadline` is when the
// timeout ends.  `ready` counts the bytes found waiting (FIONREAD), which
// are read without polling, as they can't block.
Got read_byte(ExecContext& ctx, int fd, double timeout, Clock::time_point deadline,
              bool watch, int& ready, char& c) {
  auto handle = [&] {
    bool trapped = ctx.signals.pending_trapped() != 0;
    if (ctx.executor) ctx.executor->wait_for_input();
    // (A trap's `return` ends the read too, as in zsh.)
    if (ctx.exiting or ctx.aborting or ctx.returning) return Got::Interrupted;
    return trapped and timeout >= 0 ? Got::Timeout : Got::Char;
  };
  for (;;) {
    int wake = watch ? ctx.signals.wake_fd() : -1;
    int waiting = 0;
    if (ready > 0) {
      --ready;
    } else if ((timeout >= 0 or wake >= 0) and ::ioctl(fd, FIONREAD, &waiting) == 0
               and waiting > 0) {
      ready = waiting - 1;
    } else if (timeout >= 0 or wake >= 0) {
      struct pollfd p[2]{{fd, POLLIN, 0}, {wake, POLLIN, 0}};
      int ms = -1;
      if (timeout >= 0) {
        auto left = s::chrono::ceil<s::chrono::milliseconds>(deadline - Clock::now());
        ms = static_cast<int>(s::max<s::int64_t>(left.count(), 0));
      }
      int n = ::poll(p, wake >= 0 ? 2 : 1, ms);
      if ((n < 0 and errno == EINTR)
          or (n > 0 and wake >= 0 and (p[1].revents & POLLIN) and not p[0].revents)) {
        if (Got g = handle(); g != Got::Char) return g;
        continue;
      }
      if (n == 0) return Got::Timeout;
      if (::ioctl(fd, FIONREAD, &waiting) == 0 and waiting > 0) ready = waiting - 1;
    }
    ssize_t n = ::read(fd, &c, 1);
    if (n < 0 and errno == EINTR) {
      if (Got g = handle(); g != Got::Char) return g;
      continue;
    }
    return n == 1 ? Got::Char : Got::End;
  }
}

// What `read` returns when Ctrl-C or an exiting trap ended it.
int interrupted_status(const ExecContext& ctx) {
  // A TRAPxxx that ended the line: 1, or for SIGINT the trap's status.
  if (ctx.aborting and ctx.trap_failed) return 1;
  if (ctx.aborting and ctx.interrupt and not ctx.intr_echoed) return ctx.last_exit;
  return ctx.aborting ? 128 + SIGINT : 1;
}

// A number right after the option letter (`-k3`, `-t0.5`), or in the next
// word when that is all digits.
opt<str> optional_number(const vec<str>& args, s::size_t& i, strv rest, s::size_t& k) {
  s::size_t n = 0;
  while (n < rest.size() and (s::isdigit(static_cast<unsigned char>(rest[n])) or rest[n] == '.')) ++n;
  if (n) {
    k += n;
    return str(rest.substr(0, n));
  }
  if (rest.empty() and i + 1 < args.size() and not args[i + 1].empty()
      and args[i + 1].find_first_not_of("0123456789.") == str::npos) {
    return args[++i];
  }
  return s::nullopt;
}

}  // namespace

// read [-rszqeEA] [-k [n]] [-d delim] [-t [secs]] [-u fd] [name[?prompt] …]
// Reads a line from fd 0 (or -u), a byte at a time so nothing past it is
// consumed, and splits it on $IFS: each name but the last takes a field,
// the last the rest.  With no name the line goes to $REPLY; with -A the
// fields fill an array ($reply).  Without -r a backslash quotes the next
// character, and before a newline joins the next line.  -k reads n
// characters, -q one (y/Y gives status 0), -d stops at another delimiter,
// -t gives up after a timeout, -s doesn't echo, -e/-E echo the input.
// Status 1 at end of input or on a timeout.
int builtin_read(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  bool raw = false, array = false, silent = false, query = false;
  bool echo_only = false, echo_too = false;
  opt<s::size_t> nchars;
  char   delim   = '\n';
  double timeout = -1;
  int    fd      = STDIN_FILENO;
  bool   fd_given = false;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--" or a == "-") { ++i; break; }
    if (a.size() < 2 or a[0] != '-') break;
    for (s::size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      strv rest = strv(a).substr(k + 1);
      switch (c) {
        case 'r': raw = true; break;
        case 'A': array = true; break;
        case 's': silent = true; break;
        case 'q': query = true; break;
        case 'e': echo_only = true; break;
        case 'E': echo_too = true; break;
        case 'k': {
          auto n = optional_number(args, i, rest, k);
          nchars = n ? static_cast<s::size_t>(s::strtoul(n->c_str(), nullptr, 10)) : 1;
          break;
        }
        case 't': {
          auto n = optional_number(args, i, rest, k);
          timeout = n ? s::strtod(n->c_str(), nullptr) : 0;
          break;
        }
        case 'd': case 'u': {
          str value;
          if (not rest.empty()) value = str(rest);
          else if (i + 1 < args.size()) value = args[++i];
          else {
            s::fprintf(stderr, "jfsh: read: argument expected: -%c\n", c);
            return 1;
          }
          if (c == 'd') {
            delim = value.empty() ? '\0' : value[0];
          } else {
            char* end = nullptr;
            long n = s::strtol(value.c_str(), &end, 10);
            if (value.empty() or *end != '\0' or n < 0)
              return builtin_error(cmd, "number expected after -u: " + value);
            fd = static_cast<int>(n);
            fd_given = true;
          }
          k = a.size();
          break;
        }
        case 'z': case 'c': case 'l': case 'n': case 'p':
          s::fprintf(stderr, "jfsh: read: -%c: not implemented\n", c);
          return 1;
        default:
          s::fprintf(stderr, "jfsh: read: bad option: -%c\n", c);
          return 1;
      }
    }
  }
  vec<str> names(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());
  if (array and names.size() > 1) return builtin_error(cmd, "only one array argument allowed");
  // -q and -k read the terminal, not stdin, unless -u says (zsh's SHTTY).
  struct CloseFd {
    ~CloseFd() { if (fd >= 0) ::close(fd); }
    int fd = -1;
  } opened;
  if ((query or nchars) and not fd_given) {
    if (ctx.signals.tty_fd() >= 0) {
      fd = ctx.signals.tty_fd();
    } else if ((opened.fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC)) >= 0) {
      fd = opened.fd;
    } else {
      // zsh prints this one bare.
      s::fputs("not interactive and can't open terminal\n", stderr);
      return 1;
    }
  }
  // `name?prompt`: the prompt is shown when the shell is interactive.
  if (not names.empty())
    if (auto q = names[0].find('?'); q != str::npos) {
      if (ctx.opts.interactive()) {
        s::fputs(names[0].c_str() + q + 1, stderr);
        s::fflush(stderr);
      }
      names[0].resize(q);
      if (names[0].empty()) names[0] = "REPLY";
    }

  TtyModes modes;
  modes.set(fd, silent, nchars.has_value() or query);
  s::fflush(nullptr);
  auto deadline = Clock::now() + s::chrono::duration_cast<Clock::duration>(
                                     s::chrono::duration<double>(timeout < 0 ? 0 : timeout));
  // A regular file never blocks, so it needs no watching.
  bool trap_set = false;
  for (int sig = 1; sig <= MAX_SIG and not trap_set; ++sig)
    trap_set = ctx.signals.traps().trapped(sig);
  struct stat st{};
  bool watch = (trap_set or ctx.pipeline_jid) and ctx.executor
           and not (::fstat(fd, &st) == 0 and S_ISREG(st.st_mode));
  int ready = 0;   // bytes known to be waiting on fd (read_byte)

  // -q and -k: a fixed number of characters, kept as they are.
  if (query or nchars) {
    s::size_t want = query ? 1 : *nchars;
    str got;
    bool eof = false;
    while (got.size() < want) {
      char c;
      Got g = read_byte(ctx, fd, timeout, deadline, watch, ready, c);
      if (g == Got::Interrupted) return interrupted_status(ctx);
      if (g == Got::Timeout) return 1;
      if (g == Got::End) {
        eof = true;
        break;
      }
      got += c;
    }
    // As in zsh, the end of input still sets the name (`n` for -q, what
    // was read for -k), with status 2 for -q and 1 for -k.
    str name = names.empty() ? "REPLY" : names[0];
    if (query) {
      bool yes = got == "y" or got == "Y";
      if (not ctx.executor->assign_var(name, yes ? "y" : "n")) return 1;
      return eof ? 2 : yes ? 0 : 1;
    }
    if (echo_only or echo_too) s::printf("%s\n", got.c_str());
    if (echo_only) return eof ? 1 : 0;
    if (not ctx.executor->assign_var(name, got)) return 1;
    return eof ? 1 : 0;
  }

  // Characters quoted by a backslash are marked so they don't split.
  str line;
  vec<bool> quoted;
  bool eof = false;
  for (;;) {
    char c;
    Got g = read_byte(ctx, fd, timeout, deadline, watch, ready, c);
    if (g == Got::Interrupted) return interrupted_status(ctx);
    if (g == Got::Timeout) return 1;
    if (g == Got::End) { eof = true; break; }
    if (c == delim) break;
    if (c == '\\' and not raw) {
      g = read_byte(ctx, fd, timeout, deadline, watch, ready, c);
      if (g == Got::Interrupted) return interrupted_status(ctx);
      if (g == Got::Timeout) return 1;
      if (g == Got::End) { eof = true; break; }
      if (c == '\n' and delim == '\n') continue;
      line += c;
      quoted.push_back(true);
      continue;
    }
    line += c;
    quoted.push_back(false);
  }
  // -e and -E echo each word as it is taken (bin_read), -e without
  // assigning it.
  auto echo = [&](const str& w) {
    if (echo_only or echo_too) s::printf("%s\n", w.c_str());
  };

  str ifs = " \t\n";
  if (auto p = ctx.params.lookup("IFS"); p and p->get().is_set())
    ifs = p->get().scalar();
  // zsh's isep and iwsep: in $IFS, and IFS whitespace.
  auto isep  = [&](char c) { return ifs.find(c) != str::npos; };
  auto iwsep = [&](char c) { return isep(c) and (c == ' ' or c == '\t' or c == '\n'); };
  // The words, as zsh's bin_read takes them from the line.  `first` is
  // set by a separator that isn't whitespace, after which even an empty
  // word counts; `gotnl` once the line is used up.
  s::size_t pos = 0;
  bool first = true, gotnl = false;
  auto word = [&](str& buf) {
    for (;;) {
      if (pos >= line.size()) {
        gotnl = true;
        return;
      }
      char c = line[pos];
      bool sep = not quoted[pos] and isep(c);
      ++pos;
      if (sep) {
        if (not buf.empty() or (not iwsep(c) and first)) {
          first = first or not iwsep(c);
          return;
        }
        first = first or not iwsep(c);
        continue;
      }
      first = false;
      buf += c;
    }
  };

  // The names are assigned as `name=value` would be (an integer one
  // evaluates the text); a failure ends the command line, as in zsh.
  if (array) {
    vec<str> fields;
    while (not gotnl) {
      str buf;
      word(buf);
      if (not buf.empty() or first or gotnl) fields.push_back(s::move(buf));
    }
    for (const auto& f : fields) echo(f);
    if (echo_only) return eof ? 1 : 0;
    str name = names.empty() ? "reply" : names[0];
    if (auto r = ctx.params.set_array(name, fields); not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      ctx.aborting = true;
      return 1;
    }
    return eof ? 1 : 0;
  }
  if (names.empty()) names.push_back("REPLY");
  for (s::size_t n = 0; n + 1 < names.size(); ++n) {
    str buf;
    if (not gotnl) word(buf);
    echo(buf);
    if (not echo_only and not ctx.executor->assign_var(names[n], s::move(buf))) return 1;
  }
  // The rest of the line goes to the last name: leading IFS whitespace
  // (and one other separator, unless one ended the word before) skipped,
  // trailing IFS whitespace dropped, escaped or not.
  str rest;
  for (; pos < line.size(); ++pos) {
    char c = line[pos];
    if (rest.empty() and not quoted[pos] and isep(c)) {
      if (iwsep(c)) continue;
      if (not first) {
        first = true;
        continue;
      }
    }
    rest += c;
  }
  while (not rest.empty() and iwsep(rest.back())) rest.pop_back();
  echo(rest);
  if (not echo_only and not ctx.executor->assign_var(names.back(), s::move(rest))) return 1;
  return eof ? 1 : 0;
}

}  // namespace jfsh
