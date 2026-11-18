#include <jfsh/modules/net_socket.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/net/socket (M11 Phase 11a): UNIX-domain socket builtin.  Only
// AF_UNIX is covered here; AF_INET lands with zsh/net/tcp in Phase 11c.

namespace jfsh {

namespace {

int err(strv msg) {
  s::fprintf(stderr, "jfsh: zsocket: %.*s\n",
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

static bool fill_addr(struct sockaddr_un& addr, const str& path) {
  if (path.size() >= sizeof(addr.sun_path)) return false;
  s::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  s::memcpy(addr.sun_path, path.data(), path.size());
  return true;
}

int builtin_zsocket(vec<str>& args, ExecContext& ctx) {
  bool listening = false;
  bool accepting = false;
  bool closing   = false;
  bool verbose   = false;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if      (a == "-l") listening = true;
    else if (a == "-a") accepting = true;
    else if (a == "-d") closing   = true;
    else if (a == "-v") verbose   = true;
    else if (a == "-b") { /* bind only (no listen): no-op */ }
    else if (a == "--") { ++i; break; }
    else if (a.size() > 1 and a[0] == '-') return err("bad option: " + a);
    else break;
  }
  if (closing) {
    if (i >= args.size()) return err("-d requires an fd");
    int fd = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    if (::close(fd) < 0) return err(s::strerror(errno));
    return 0;
  }
  if (accepting) {
    if (i >= args.size()) return err("-a requires a listening fd");
    int lfd = static_cast<int>(s::strtol(args[i].c_str(), nullptr, 10));
    int c = ::accept(lfd, nullptr, nullptr);
    if (c < 0) return err(s::strerror(errno));
    (void)ctx.params.set_scalar("REPLY", s::to_string(c));
    if (verbose) s::fprintf(stderr, "accepted fd %d\n", c);
    return 0;
  }
  if (i >= args.size()) return err("not enough arguments");
  const str& path = args[i];
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return err(s::strerror(errno));
  struct sockaddr_un addr;
  if (not fill_addr(addr, path)) { ::close(fd); return err("path too long"); }
  if (listening) {
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) < 0 or
        ::listen(fd, 5) < 0) {
      int saved = errno;
      ::close(fd);
      return err(s::strerror(saved));
    }
  } else {
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr) < 0) {
      int saved = errno;
      ::close(fd);
      return err(s::strerror(saved));
    }
  }
  (void)ctx.params.set_scalar("REPLY", s::to_string(fd));
  if (verbose) s::fprintf(stderr, "%s fd %d for %s\n",
                          listening ? "listening on" : "connected", fd, path.c_str());
  return 0;
}

}  // namespace

void register_net_socket_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/net/socket", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"zsocket", builtin_zsocket, BINF_NONE, 1});
    out.feature_names = {"b:zsocket"};
  });
  // zsh's historical name (`zmodload zsh/socket`) → same module.
  mm.add_alias("zsh/socket", "zsh/net/socket");
}

}  // namespace jfsh
