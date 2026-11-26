#include <jfsh/modules/zselect.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/select.h>
#include <sys/time.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/zselect (M11 Phase 11a): wraps pselect(2) as `zselect`.

namespace jfsh {

namespace {

int err(strv cmd, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n",
             static_cast<int>(cmd.size()), cmd.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

static bool parse_fd(const str& s, int& out) {
  char* end = nullptr;
  long v = s::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() or *end != '\0' or v < 0) return false;
  out = static_cast<int>(v);
  return true;
}

int builtin_zselect(vec<str>& args, ExecContext& ctx) {
  fd_set rfds, wfds, efds;
  FD_ZERO(&rfds); FD_ZERO(&wfds); FD_ZERO(&efds);
  int maxfd = -1;

  auto add_fd = [&](int fd, fd_set& set) {
    FD_SET(fd, &set);
    if (fd > maxfd) maxfd = fd;
  };

  // Default channel: `-r`.  Each `-r`/`-w`/`-e` switches which set the
  // following fd arguments go into.  zsh also accepts `-N:` separators.
  char current = 'r';
  double timeout = -1;
  bool has_timeout = false;
  str array_name;

  for (s::size_t i = 1; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-r" or a == "-w" or a == "-e") { current = a[1]; continue; }
    if (a == "-t") {
      if (++i >= args.size()) return err("zselect", "option -t requires an argument");
      // zsh's -t is in hundredths of a second; keep that convention.
      timeout = s::strtod(args[i].c_str(), nullptr) / 100.0;
      has_timeout = true;
      continue;
    }
    if (a == "-A") {
      if (++i >= args.size()) return err("zselect", "option -A requires an argument");
      array_name = args[i];
      continue;
    }
    if (a == "--") { ++i; current = 'r'; continue; }
    if (not a.empty() and a[0] == '-' and a.size() > 1 and not s::isdigit(static_cast<unsigned char>(a[1])))
      return err("zselect", "bad option: " + a);
    int fd;
    if (not parse_fd(a, fd)) return err("zselect", "bad fd: " + a);
    switch (current) {
      case 'r': add_fd(fd, rfds); break;
      case 'w': add_fd(fd, wfds); break;
      case 'e': add_fd(fd, efds); break;
    }
  }

  if (maxfd < 0 and not has_timeout)
    return err("zselect", "no fds and no timeout given");

  struct timespec ts;
  struct timespec* tp = nullptr;
  if (has_timeout) {
    ts.tv_sec  = static_cast<time_t>(timeout);
    ts.tv_nsec = static_cast<long>((timeout - ts.tv_sec) * 1e9);
    if (ts.tv_nsec < 0) { ts.tv_nsec = 0; }
    tp = &ts;
  }
  int sr = ::pselect(maxfd + 1, &rfds, &wfds, &efds, tp, nullptr);
  if (sr < 0) {
    if (errno == EINTR) return 2;
    return err("zselect", s::strerror(errno));
  }
  if (sr == 0) return 1;

  vec<str> ready;
  for (int fd = 0; fd <= maxfd; ++fd) {
    if (FD_ISSET(fd, &rfds)) ready.push_back("r:" + s::to_string(fd));
    if (FD_ISSET(fd, &wfds)) ready.push_back("w:" + s::to_string(fd));
    if (FD_ISSET(fd, &efds)) ready.push_back("e:" + s::to_string(fd));
  }
  if (not array_name.empty()) {
    (void)ctx.params.set_array(array_name, ready, false);
  } else {
    for (const auto& r : ready) s::printf("%s\n", r.c_str());
  }
  (void)ctx;
  return 0;
}

}  // namespace

void register_zselect_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/zselect", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"zselect", builtin_zselect, BINF_NONE, 0});
  });
}

}  // namespace jfsh
