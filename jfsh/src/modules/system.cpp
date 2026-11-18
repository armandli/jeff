#include <jfsh/modules/system.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/select.h>
#include <unistd.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/system (M10 Phase 2b): syserror / sysopen / sysread / syswrite /
// sysseek / zsystem.  Thin wrappers around the POSIX syscall surface; the
// shell exits codes track zsh's (0 on success, 1 on error, 2 on "operation
// cannot complete without blocking" for non-blocking `-t 0`).

namespace jfsh {

namespace {

int builtin_error(strv name, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n",
             static_cast<int>(name.size()), name.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

// ---------------------------------------------------------------------------
// syserror [-e var] [-p prefix] errno
//   Print (or store in -e var) the strerror(3) text of the given errno.
//   `errno` may be an integer or an errno name ("EACCES", "ENOENT", …).
// ---------------------------------------------------------------------------

struct ErrnoEntry { const char* name; int value; };

// A static subset of the errno values POSIX requires (and the GNU/BSD
// extensions zsh exposes).  Anything the platform doesn't define is
// skipped.
#define JFSH_ERRNO_TABLE                 \
  X(EPERM)   X(ENOENT)  X(ESRCH)         \
  X(EINTR)   X(EIO)     X(ENXIO)         \
  X(E2BIG)   X(ENOEXEC) X(EBADF)         \
  X(ECHILD)  X(EAGAIN)  X(ENOMEM)        \
  X(EACCES)  X(EFAULT)  X(ENOTBLK)       \
  X(EBUSY)   X(EEXIST)  X(EXDEV)         \
  X(ENODEV)  X(ENOTDIR) X(EISDIR)        \
  X(EINVAL)  X(ENFILE)  X(EMFILE)        \
  X(ENOTTY)  X(ETXTBSY) X(EFBIG)         \
  X(ENOSPC)  X(ESPIPE)  X(EROFS)         \
  X(EMLINK)  X(EPIPE)   X(EDOM)          \
  X(ERANGE)  X(EDEADLK) X(ENAMETOOLONG)  \
  X(ENOLCK)  X(ENOSYS)  X(ENOTEMPTY)     \
  X(ELOOP)   X(EWOULDBLOCK) X(ENOMSG)    \
  X(EIDRM)   X(ENOSTR)  X(ENODATA)       \
  X(ETIME)   X(ENOSR)   X(EREMOTE)       \
  X(ENOLINK) X(EPROTO)  X(EMULTIHOP)     \
  X(EBADMSG) X(EOVERFLOW) X(EILSEQ)      \
  X(EUSERS)  X(ENOTSOCK) X(EDESTADDRREQ) \
  X(EMSGSIZE) X(EPROTOTYPE) X(ENOPROTOOPT) \
  X(EPROTONOSUPPORT) X(ESOCKTNOSUPPORT)  \
  X(EOPNOTSUPP) X(EPFNOSUPPORT)          \
  X(EAFNOSUPPORT) X(EADDRINUSE)          \
  X(EADDRNOTAVAIL) X(ENETDOWN)           \
  X(ENETUNREACH) X(ENETRESET)            \
  X(ECONNABORTED) X(ECONNRESET)          \
  X(ENOBUFS) X(EISCONN) X(ENOTCONN)      \
  X(ESHUTDOWN) X(ETOOMANYREFS)           \
  X(ETIMEDOUT) X(ECONNREFUSED)           \
  X(EHOSTDOWN) X(EHOSTUNREACH)           \
  X(EALREADY) X(EINPROGRESS) X(ESTALE)   \
  X(EDQUOT) X(ECANCELED)

static const vec<ErrnoEntry>& errno_table() {
  static const vec<ErrnoEntry> t = []{
    vec<ErrnoEntry> v;
#define X(n) v.push_back({#n, n});
    JFSH_ERRNO_TABLE
#undef X
    return v;
  }();
  return t;
}

static int parse_errno(strv text) {
  // Either a decimal number or an errno name.
  if (not text.empty() and s::isdigit(static_cast<unsigned char>(text[0])))
    return static_cast<int>(s::strtol(str(text).c_str(), nullptr, 10));
  for (const auto& e : errno_table()) if (text == e.name) return e.value;
  return -1;
}

int builtin_syserror(vec<str>& args, ExecContext& ctx) {
  str var, prefix;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-e") {
      if (++i >= args.size()) return builtin_error("syserror", "option -e requires an argument");
      var = args[i];
    } else if (a == "-p") {
      if (++i >= args.size()) return builtin_error("syserror", "option -p requires an argument");
      prefix = args[i];
    } else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return builtin_error("syserror", "bad option: " + a);
    else break;
  }
  if (i >= args.size()) return builtin_error("syserror", "not enough arguments");
  int err = parse_errno(args[i]);
  if (err < 0) return builtin_error("syserror", "unknown errno: " + args[i]);
  str msg = prefix + s::strerror(err);
  if (not var.empty()) {
    auto r = ctx.params.set_scalar(var, msg);
    return r ? 0 : 1;
  }
  s::fprintf(stderr, "%s\n", msg.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// sysopen -o flags -u fd file
//   Opens `file` with the given comma-separated flags
//   (read/write/append/create/excl/trunc/nonblock/noctty/sync), storing
//   the resulting fd in the parameter named `-u fd`.  When `-u` is absent
//   the fd goes into $REPLY.
// ---------------------------------------------------------------------------

int builtin_sysopen(vec<str>& args, ExecContext& ctx) {
  int mode = -1;
  int extra_flags = 0;
  str var = "REPLY";
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-o") {
      if (++i >= args.size()) return builtin_error("sysopen", "option -o requires an argument");
      // comma-separated flag list
      str list = args[i];
      str tok;
      list.push_back(',');
      for (char c : list) {
        if (c != ',') { tok.push_back(c); continue; }
        if      (tok == "read" or tok == "r")  mode = O_RDONLY;
        else if (tok == "write" or tok == "w") mode = O_WRONLY;
        else if (tok == "readwrite" or tok == "rw") mode = O_RDWR;
        else if (tok == "append")        extra_flags |= O_APPEND;
        else if (tok == "create")        extra_flags |= O_CREAT;
        else if (tok == "excl")          extra_flags |= O_EXCL;
        else if (tok == "truncate")      extra_flags |= O_TRUNC;
        else if (tok == "nonblock")      extra_flags |= O_NONBLOCK;
        else if (tok == "noctty")        extra_flags |= O_NOCTTY;
        else if (tok == "sync")          extra_flags |= O_SYNC;
        else if (tok == "cloexec")       extra_flags |= O_CLOEXEC;
        else if (not tok.empty())        return builtin_error("sysopen", "bad -o flag: " + tok);
        tok.clear();
      }
    } else if (a == "-u") {
      if (++i >= args.size()) return builtin_error("sysopen", "option -u requires an argument");
      var = args[i];
    } else if (a == "-a" or a == "-r" or a == "-w") {
      // Shortcut flags (zsh accepts them).
      if      (a == "-r") mode = O_RDONLY;
      else if (a == "-w") mode = O_WRONLY;
      else                { mode = O_WRONLY; extra_flags |= O_APPEND; }
    } else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return builtin_error("sysopen", "bad option: " + a);
    else break;
  }
  if (mode < 0) mode = O_RDONLY;
  if (i >= args.size()) return builtin_error("sysopen", "not enough arguments");
  const str& path = args[i];
  int fd = ::open(path.c_str(), mode | extra_flags, 0644);
  if (fd < 0) return builtin_error("sysopen", s::strerror(errno));
  auto r = ctx.params.set_scalar(var, s::to_string(fd));
  return r ? 0 : 1;
}

// ---------------------------------------------------------------------------
// sysread [-c count] [-i fd] [-o fd] [-s size] [-t timeout] [name]
//   Reads up to `-s size` bytes from `-i fd` (default 0).  If `-t secs`
//   is set, waits at most that long via select(); on timeout returns 1,
//   on "would block" with -t 0 returns 2.  If `-o out` is set, writes
//   bytes to `out` instead of storing in `name` (default $REPLY).
// ---------------------------------------------------------------------------

int builtin_sysread(vec<str>& args, ExecContext& ctx) {
  int in_fd = STDIN_FILENO;
  int out_fd = -1;
  int size = 8192;
  double timeout = -1;
  str name = "REPLY";
  str count_var;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-i") {
      if (++i >= args.size()) return builtin_error("sysread", "option -i requires an argument");
      in_fd = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    } else if (a == "-o") {
      if (++i >= args.size()) return builtin_error("sysread", "option -o requires an argument");
      out_fd = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    } else if (a == "-s") {
      if (++i >= args.size()) return builtin_error("sysread", "option -s requires an argument");
      size = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    } else if (a == "-t") {
      if (++i >= args.size()) return builtin_error("sysread", "option -t requires an argument");
      timeout = s::strtod(args[i].c_str(), nullptr);
    } else if (a == "-c") {
      if (++i >= args.size()) return builtin_error("sysread", "option -c requires an argument");
      count_var = args[i];
    } else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return builtin_error("sysread", "bad option: " + a);
    else break;
  }
  if (i < args.size()) name = args[i];

  if (timeout >= 0) {
    fd_set rfds; FD_ZERO(&rfds); FD_SET(in_fd, &rfds);
    struct timeval tv;
    tv.tv_sec  = static_cast<time_t>(timeout);
    tv.tv_usec = static_cast<suseconds_t>((timeout - tv.tv_sec) * 1e6);
    int sr = ::select(in_fd + 1, &rfds, nullptr, nullptr, &tv);
    if (sr < 0) return builtin_error("sysread", s::strerror(errno));
    if (sr == 0) return (timeout == 0.0) ? 2 : 1;
  }

  vec<char> buf(static_cast<s::size_t>(size));
  ssize_t n = ::read(in_fd, buf.data(), buf.size());
  if (n < 0) return builtin_error("sysread", s::strerror(errno));
  if (not count_var.empty())
    (void)ctx.params.set_scalar(count_var, s::to_string(n));
  str data(buf.data(), static_cast<s::size_t>(n));
  if (out_fd >= 0) {
    ssize_t written = 0;
    while (written < n) {
      ssize_t w = ::write(out_fd, data.c_str() + written, static_cast<s::size_t>(n - written));
      if (w < 0) return builtin_error("sysread", s::strerror(errno));
      written += w;
    }
  } else {
    (void)ctx.params.set_scalar(name, data);
  }
  return n == 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// syswrite [-c count] [-o fd] data
// ---------------------------------------------------------------------------

int builtin_syswrite(vec<str>& args, ExecContext& ctx) {
  int out_fd = STDOUT_FILENO;
  int count = -1;
  str count_var;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-o") {
      if (++i >= args.size()) return builtin_error("syswrite", "option -o requires an argument");
      out_fd = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    } else if (a == "-c") {
      if (++i >= args.size()) return builtin_error("syswrite", "option -c requires an argument");
      count_var = args[i];
    } else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return builtin_error("syswrite", "bad option: " + a);
    else break;
  }
  if (i >= args.size()) return builtin_error("syswrite", "not enough arguments");
  const str& data = args[i];
  (void)count;
  ssize_t total = 0;
  const char* p = data.c_str();
  ssize_t left = static_cast<ssize_t>(data.size());
  while (left > 0) {
    ssize_t w = ::write(out_fd, p + total, static_cast<s::size_t>(left));
    if (w < 0) {
      if (errno == EINTR) continue;
      return builtin_error("syswrite", s::strerror(errno));
    }
    total += w;
    left  -= w;
  }
  if (not count_var.empty())
    (void)ctx.params.set_scalar(count_var, s::to_string(total));
  return 0;
}

// ---------------------------------------------------------------------------
// sysseek [-u fd] [-w start|end|current] offset
// ---------------------------------------------------------------------------

int builtin_sysseek(vec<str>& args, ExecContext&) {
  int fd = STDIN_FILENO;
  int whence = SEEK_SET;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-u") {
      if (++i >= args.size()) return builtin_error("sysseek", "option -u requires an argument");
      fd = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    } else if (a == "-w") {
      if (++i >= args.size()) return builtin_error("sysseek", "option -w requires an argument");
      if      (args[i] == "start")   whence = SEEK_SET;
      else if (args[i] == "end")     whence = SEEK_END;
      else if (args[i] == "current") whence = SEEK_CUR;
      else return builtin_error("sysseek", "bad -w value: " + args[i]);
    } else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return builtin_error("sysseek", "bad option: " + a);
    else break;
  }
  if (i >= args.size()) return builtin_error("sysseek", "not enough arguments");
  off_t off = static_cast<off_t>(s::strtoll(args[i].c_str(), nullptr, 10));
  if (::lseek(fd, off, whence) < 0) return builtin_error("sysseek", s::strerror(errno));
  return 0;
}

// ---------------------------------------------------------------------------
// zsystem flock [-t secs] [-f var] [-r] file
//   Open `file` (if not already) and flock it (LOCK_EX by default, LOCK_SH
//   with -r).  -t 0 → LOCK_NB; -t N → wait N seconds then give up.  -f
//   var stores the fd, so you can `zsystem flock -u $fd` later.
// zsystem unflock fd
// zsystem supports feature
// ---------------------------------------------------------------------------

int builtin_zsystem(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return builtin_error("zsystem", "not enough arguments");
  const str& sub = args[1];

  if (sub == "supports") {
    if (args.size() < 3) return builtin_error("zsystem", "not enough arguments");
    const str& feat = args[2];
    if (feat == "flock" or feat == "yield" or feat == "supports") return 0;
    return 1;
  }

  if (sub == "unflock") {
    if (args.size() < 3) return builtin_error("zsystem", "not enough arguments");
    int fd = static_cast<int>(s::strtol(args[2].c_str(), nullptr, 10));
    if (::flock(fd, LOCK_UN) < 0) return builtin_error("zsystem", s::strerror(errno));
    ::close(fd);
    return 0;
  }

  if (sub == "flock") {
    double timeout = -1;
    bool shared = false;
    str var;
    s::size_t i = 2;
    for (; i < args.size(); ++i) {
      const str& a = args[i];
      if (a == "-t") {
        if (++i >= args.size()) return builtin_error("zsystem", "option -t requires an argument");
        timeout = s::strtod(args[i].c_str(), nullptr);
      } else if (a == "-f") {
        if (++i >= args.size()) return builtin_error("zsystem", "option -f requires an argument");
        var = args[i];
      } else if (a == "-r") { shared = true; }
      else if (a == "-i") { /* ignore existing locks: no-op */ }
      else if (a == "--") { ++i; break; }
      else if (a.size() > 1 and a[0] == '-') return builtin_error("zsystem flock", "bad option: " + a);
      else break;
    }
    if (i >= args.size()) return builtin_error("zsystem flock", "not enough arguments");
    int fd = ::open(args[i].c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
    if (fd < 0) return builtin_error("zsystem flock", s::strerror(errno));
    int op = shared ? LOCK_SH : LOCK_EX;

    int rc;
    if (timeout == 0.0) {
      rc = ::flock(fd, op | LOCK_NB);
    } else if (timeout > 0) {
      // Poll with LOCK_NB until the timeout elapses.
      struct timespec start{}; ::clock_gettime(CLOCK_MONOTONIC, &start);
      for (;;) {
        rc = ::flock(fd, op | LOCK_NB);
        if (rc == 0) break;
        if (errno != EWOULDBLOCK) break;
        struct timespec now{}; ::clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) +
                         (now.tv_nsec - start.tv_nsec) * 1e-9;
        if (elapsed >= timeout) { errno = EWOULDBLOCK; break; }
        struct timespec sleep_for{0, 50'000'000};  // 50ms
        ::nanosleep(&sleep_for, nullptr);
      }
    } else {
      rc = ::flock(fd, op);
    }
    if (rc < 0) {
      int saved = errno;
      ::close(fd);
      return builtin_error("zsystem flock", s::strerror(saved));
    }
    if (not var.empty())
      (void)ctx.params.set_scalar(var, s::to_string(fd));
    return 0;
  }

  return builtin_error("zsystem", "unknown subcommand: " + sub);
}

}  // namespace

void register_system_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/system", [](ModuleFeatures& out, ExecContext& ctx) {
    out.builtins.push_back(BuiltinDef{"syserror", builtin_syserror, BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"sysopen",  builtin_sysopen,  BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"sysread",  builtin_sysread,  BINF_NONE, 0});
    out.builtins.push_back(BuiltinDef{"syswrite", builtin_syswrite, BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"sysseek",  builtin_sysseek,  BINF_NONE, 1});
    out.builtins.push_back(BuiltinDef{"zsystem",  builtin_zsystem,  BINF_NONE, 1});

    // $errnos: associative array, declared with a copy snapshot (updates
    // are tied to the C table, which is immutable at runtime).
    umap<str, str> h;
    for (const auto& e : errno_table()) h[e.name] = s::to_string(e.value);
    Param& p = ctx.params.assign("errnos", s::move(h), PM_HASH | PM_SPECIAL | PM_READONLY);
    (void)p;
    out.params.emplace_back("errnos");
  });
}

}  // namespace jfsh
