// The `read` builtin.

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

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
    if (raw) {
      t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
      t.c_cc[VMIN]  = 1;
      t.c_cc[VTIME] = 0;
    }
    ::tcsetattr(fd, TCSADRAIN, &t);
  }
  ~TtyModes() {
    if (fd >= 0) ::tcsetattr(fd, TCSADRAIN, &saved);
  }
};

enum class Got { Char, End, Timeout };

// One byte from `fd`, waiting at most `timeout` seconds (negative: no
// limit).
Got read_byte(int fd, double timeout, char& c) {
  for (;;) {
    if (timeout >= 0) {
      struct pollfd p{fd, POLLIN, 0};
      int ms = static_cast<int>(s::ceil(timeout * 1000));
      int n = ::poll(&p, 1, ms);
      if (n < 0 and errno == EINTR) continue;
      if (n == 0) return Got::Timeout;
    }
    ssize_t n = ::read(fd, &c, 1);
    if (n < 0 and errno == EINTR) continue;
    return n == 1 ? Got::Char : Got::End;
  }
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

  // -q and -k: a fixed number of characters, kept as they are.
  if (query or nchars) {
    s::size_t want = query ? 1 : *nchars;
    str got;
    while (got.size() < want) {
      char c;
      Got g = read_byte(fd, timeout, c);
      if (g != Got::Char) {
        if (got.empty()) return 1;
        break;
      }
      got += c;
    }
    str name = names.empty() ? "REPLY" : names[0];
    if (query) {
      bool yes = got == "y" or got == "Y";
      if (auto r = ctx.params.set_scalar(name, yes ? "y" : "n"); not r)
        return builtin_error(cmd, r.error().message);
      return yes ? 0 : 1;
    }
    if (echo_only or echo_too) s::printf("%s\n", got.c_str());
    if (echo_only) return 0;
    if (auto r = ctx.params.set_scalar(name, got); not r) return builtin_error(cmd, r.error().message);
    return 0;
  }

  // Characters quoted by a backslash are marked so they don't split.
  str line;
  vec<bool> quoted;
  bool eof = false;
  for (;;) {
    char c;
    Got g = read_byte(fd, timeout, c);
    if (g == Got::Timeout) return 1;
    if (g == Got::End) { eof = true; break; }
    if (c == delim) break;
    if (c == '\\' and not raw) {
      g = read_byte(fd, timeout, c);
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
  if (eof and line.empty()) return 1;
  if (echo_only or echo_too) s::printf("%s\n", line.c_str());
  if (echo_only) return eof ? 1 : 0;

  str ifs = " \t\n";
  if (auto p = ctx.params.lookup("IFS"); p and p->get().is_set())
    ifs = p->get().scalar();
  auto is_sep = [&](s::size_t k) {
    return not quoted[k] and ifs.find(line[k]) != str::npos;
  };
  auto is_white = [&](s::size_t k) {
    return is_sep(k) and (line[k] == ' ' or line[k] == '\t' or line[k] == '\n');
  };
  // Fields, plus where each starts so the last name can take the rest.
  vec<str> fields;
  vec<s::size_t> starts;
  s::size_t k = 0;
  while (k < line.size() and is_white(k)) ++k;
  while (k < line.size()) {
    starts.push_back(k);
    str field;
    while (k < line.size() and not is_sep(k)) field += line[k++];
    fields.push_back(s::move(field));
    while (k < line.size() and is_white(k)) ++k;
    if (k < line.size() and is_sep(k)) {
      ++k;
      while (k < line.size() and is_white(k)) ++k;
    }
  }

  auto set = [&](const str& name, str value) {
    if (auto r = ctx.params.set_scalar(name, s::move(value)); not r) {
      s::fprintf(stderr, "jfsh: read: %s\n", r.error().message.c_str());
      return false;
    }
    return true;
  };
  if (array) {
    str name = names.empty() ? "reply" : names[0];
    if (auto r = ctx.params.set_array(name, fields); not r) {
      s::fprintf(stderr, "jfsh: read: %s\n", r.error().message.c_str());
      return 1;
    }
    return eof ? 1 : 0;
  }
  if (names.empty()) names.push_back("REPLY");
  for (s::size_t n = 0; n < names.size(); ++n) {
    str value;
    if (n + 1 == names.size() and n < fields.size()) {
      // The rest of the line, without trailing IFS whitespace.
      s::size_t end = line.size();
      while (end > starts[n] and is_white(end - 1)) --end;
      value = line.substr(starts[n], end - starts[n]);
    } else if (n < fields.size()) {
      value = fields[n];
    }
    if (not set(names[n], s::move(value))) return 1;
  }
  return eof ? 1 : 0;
}

}  // namespace jfsh
