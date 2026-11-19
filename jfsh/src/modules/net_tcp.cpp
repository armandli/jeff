#include <jfsh/modules/net_tcp.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// zsh/net/tcp (M11): `ztcp`, TCP sessions the shell keeps track of, as
// zsh's Src/Modules/tcp.c.  IPv4 and IPv6 both go through getaddrinfo.

namespace jfsh {

namespace {

struct TcpSession {
  int  fd = -1;
  char kind = 'O';     // L listening, I accepted (incoming), O outgoing
  str  local_host;
  int  local_port = 0;
  str  remote_host;
  int  remote_port = 0;
};

vec<TcpSession>& sessions() {
  static vec<TcpSession> v;
  return v;
}

int err(strv msg) { return mod_error("ztcp", msg); }

// The numeric host and port of a socket address.
s::pair<str, int> describe(const struct sockaddr* sa, socklen_t len) {
  char host[NI_MAXHOST] = "", serv[NI_MAXSERV] = "";
  if (::getnameinfo(sa, len, host, sizeof host, serv, sizeof serv,
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    return {"", 0};
  // An IPv4 peer on the dual-stack listener shows as itself.
  str h = host;
  if (h.starts_with("::ffff:") and h.find('.') != str::npos) h.erase(0, 7);
  return {h, s::atoi(serv)};
}

void fill_ends(TcpSession& t) {
  struct sockaddr_storage ss{};
  socklen_t len = sizeof ss;
  if (::getsockname(t.fd, reinterpret_cast<struct sockaddr*>(&ss), &len) == 0)
    s::tie(t.local_host, t.local_port) = describe(reinterpret_cast<struct sockaddr*>(&ss), len);
  len = sizeof ss;
  if (t.kind != 'L' and ::getpeername(t.fd, reinterpret_cast<struct sockaddr*>(&ss), &len) == 0)
    s::tie(t.remote_host, t.remote_port) = describe(reinterpret_cast<struct sockaddr*>(&ss), len);
}

// `-d fd`: the session's socket moves to `target`.
bool move_fd(int& fd, int target) {
  if (target < 0 or target == fd) return true;
  if (::dup2(fd, target) < 0) return false;
  ::close(fd);
  fd = target;
  return true;
}

int store(ExecContext& ctx, TcpSession t, bool verbose) {
  fill_ends(t);
  (void)ctx.params.set_scalar("REPLY", s::to_string(t.fd));
  if (verbose)
    s::printf("%d %s:%d\n", t.fd, (t.kind == 'L' ? t.local_host : t.remote_host).c_str(),
              t.kind == 'L' ? t.local_port : t.remote_port);
  sessions().push_back(s::move(t));
  return 0;
}

int do_connect(ExecContext& ctx, const str& host, const str& port, int target, bool verbose) {
  struct addrinfo hints{}, *res = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  if (int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0)
    return err("host resolution failure: " + host + ": " + ::gai_strerror(rc));
  int fd = -1, saved = 0;
  for (auto* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { saved = errno; continue; }
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    saved = errno;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) return err(str("connection failed: ") + s::strerror(saved));
  if (not move_fd(fd, target)) { ::close(fd); return err(s::strerror(errno)); }
  TcpSession t;
  t.fd = fd;
  t.kind = 'O';
  return store(ctx, s::move(t), verbose);
}

int do_listen(ExecContext& ctx, const str& port, int target, bool verbose) {
  char* end = nullptr;
  long p = s::strtol(port.c_str(), &end, 10);
  if (port.empty() or *end or p < 0 or p > 65535) {
    struct servent* se = ::getservbyname(port.c_str(), "tcp");
    if (not se) return err("bad service name or port number: " + port);
    p = ntohs(static_cast<s::uint16_t>(se->s_port));
  }
  int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  bool v6 = fd >= 0;
  if (not v6) fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return err(s::strerror(errno));
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  int rc;
  if (v6) {
    int zero = 0;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
    struct sockaddr_in6 a{};
    a.sin6_family = AF_INET6;
    a.sin6_addr   = in6addr_any;
    a.sin6_port   = htons(static_cast<s::uint16_t>(p));
    rc = ::bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a);
  } else {
    struct sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons(static_cast<s::uint16_t>(p));
    rc = ::bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a);
  }
  if (rc < 0 or ::listen(fd, 1) < 0) {
    int saved = errno;
    ::close(fd);
    return err(str("could not bind or listen: ") + s::strerror(saved));
  }
  if (not move_fd(fd, target)) { ::close(fd); return err(s::strerror(errno)); }
  TcpSession t;
  t.fd = fd;
  t.kind = 'L';
  return store(ctx, s::move(t), verbose);
}

int do_accept(ExecContext& ctx, const str& lfd_text, int target, bool test, bool verbose) {
  int lfd = static_cast<int>(s::strtol(lfd_text.c_str(), nullptr, 10));
  auto& v = sessions();
  if (s::none_of(v.begin(), v.end(), [&](const TcpSession& t) { return t.fd == lfd and t.kind == 'L'; }))
    return err("could not find listening socket on fd " + lfd_text);
  if (test) {
    struct pollfd p{lfd, POLLIN, 0};
    if (::poll(&p, 1, 0) <= 0) return 1;
  }
  int fd;
  do fd = ::accept(lfd, nullptr, nullptr);
  while (fd < 0 and errno == EINTR);
  if (fd < 0) return err(str("could not accept connection: ") + s::strerror(errno));
  if (not move_fd(fd, target)) { ::close(fd); return err(s::strerror(errno)); }
  TcpSession t;
  t.fd = fd;
  t.kind = 'I';
  return store(ctx, s::move(t), verbose);
}

int do_close(const vec<str>& rest, bool force) {
  auto& v = sessions();
  if (rest.empty()) {
    for (auto& t : v) ::close(t.fd);
    v.clear();
    return 0;
  }
  int rc = 0;
  for (const auto& a : rest) {
    int fd = static_cast<int>(s::strtol(a.c_str(), nullptr, 10));
    auto it = s::find_if(v.begin(), v.end(), [&](const TcpSession& t) { return t.fd == fd; });
    if (it == v.end()) {
      if (force) { ::close(fd); continue; }
      rc = err("fd " + a + " not found in tcp table");
      continue;
    }
    ::close(it->fd);
    v.erase(it);
  }
  return rc;
}

void list(bool script) {
  for (const auto& t : sessions()) {
    if (script) {
      s::printf("%d %c %s %d %s %d\n", t.fd, t.kind, t.local_host.c_str(), t.local_port,
                t.remote_host.c_str(), t.remote_port);
    } else if (t.kind == 'L') {
      s::printf("%s:%d is on fd %d (listening)\n", t.local_host.c_str(), t.local_port, t.fd);
    } else {
      s::printf("%s:%d %s %s:%d is on fd %d\n", t.local_host.c_str(), t.local_port,
                t.kind == 'I' ? "<-" : "->", t.remote_host.c_str(), t.remote_port, t.fd);
    }
  }
}

int builtin_ztcp(vec<str>& args, ExecContext& ctx) {
  bool listen = false, accept = false, close = false, force = false;
  bool script = false, test = false, verbose = false;
  int target = -1;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; break; }
    if (a.size() < 2 or a[0] != '-') break;
    for (s::size_t k = 1; k < a.size(); ++k) {
      switch (a[k]) {
        case 'l': listen  = true; break;
        case 'a': accept  = true; break;
        case 'c': close   = true; break;
        case 'f': force   = true; break;
        case 'L': script  = true; break;
        case 't': test    = true; break;
        case 'v': verbose = true; break;
        case 'd': {
          str fd = a.substr(k + 1);
          if (fd.empty()) {
            if (++i >= args.size()) return err("-d requires an argument");
            fd = args[i];
          }
          target = static_cast<int>(s::strtol(fd.c_str(), nullptr, 10));
          k = a.size();
          break;
        }
        default: return err(str("bad option: -") + a[k]);
      }
    }
  }
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());
  if (close)  return do_close(rest, force);
  if (listen) {
    if (rest.empty()) return err("-l requires an argument");
    return do_listen(ctx, rest[0], target, verbose);
  }
  if (accept) {
    if (rest.empty()) return err("-a requires an argument");
    return do_accept(ctx, rest[0], target, test, verbose);
  }
  if (rest.empty()) { list(script); return 0; }
  return do_connect(ctx, rest[0], rest.size() > 1 ? rest[1] : "telnet", target, verbose);
}

}  // namespace

void register_net_tcp_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/net/tcp", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"ztcp", builtin_ztcp, BINF_NONE, 0});
    out.feature_names = {"b:ztcp"};
  });
}

}  // namespace jfsh
