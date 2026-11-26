#include <jfsh/modules/zftp.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// zsh/zftp (M11): an FTP client builtin, as zsh's Src/Modules/zftp.c.
// Each named session has a control connection; data connections are
// passive and opened per transfer.  Replies go to $ZFTP_REPLY / $ZFTP_CODE
// and, when the reply's first digit is in $ZFTP_VERBOSE (default 450), to
// stderr.

namespace jfsh {

namespace {

constexpr int ZFTP_PORT = 21;

struct FtpSession {
  int  ctl = -1;
  str  host;
  int  port = ZFTP_PORT;
  str  ip;
  str  user;
  str  pwd;
  str  system;
  char type = 'I';            // A (ascii) or I (image)
  str  inbuf;                 // control bytes read but not yet used
  // `zftp params`: what `open` and `login` use when not given.
  vec<str> params;
};

struct State {
  s::map<str, FtpSession> sessions{{"default", FtpSession{}}};
  str current = "default";
  FtpSession& cur() { return sessions[current]; }
};

State& state() {
  static State st;
  return st;
}

int err(strv msg) { return mod_error("zftp", msg); }

int timeout_ms(ExecContext& ctx) {
  long t = s::strtol(param_or(ctx.params, "ZFTP_TMOUT", "60").c_str(), nullptr, 10);
  return t > 0 ? static_cast<int>(t * 1000) : -1;
}

// $ZFTP_HOST and the rest, from the current session; unset when it is
// not connected.
void sync_params(ExecContext& ctx) {
  auto& st = state();
  auto& f = st.cur();
  (void)ctx.params.set_scalar("ZFTP_SESSION", st.current);
  auto put = [&](strv name, const str& v) {
    if (f.ctl >= 0 and not v.empty()) (void)ctx.params.set_scalar(name, v);
    else (void)ctx.params.unset(name);
  };
  put("ZFTP_HOST",   f.host);
  put("ZFTP_PORT",   s::to_string(f.port));
  put("ZFTP_IP",     f.ip);
  put("ZFTP_USER",   f.user);
  put("ZFTP_PWD",    f.pwd);
  put("ZFTP_SYSTEM", f.system);
  (void)ctx.params.set_scalar("ZFTP_TYPE", str(1, f.type));
  (void)ctx.params.set_scalar("ZFTP_MODE", "S");
}

void disconnect(ExecContext& ctx, FtpSession& f) {
  if (f.ctl >= 0) ::close(f.ctl);
  f.ctl = -1;
  f.inbuf.clear();
  f.user.clear();
  f.pwd.clear();
  f.system.clear();
  sync_params(ctx);
}

// One line from the control connection, without its CRLF.  Null on EOF,
// an error or a timeout.
opt<str> read_line(ExecContext& ctx, FtpSession& f) {
  for (;;) {
    if (auto nl = f.inbuf.find('\n'); nl != str::npos) {
      str line = f.inbuf.substr(0, nl);
      f.inbuf.erase(0, nl + 1);
      if (not line.empty() and line.back() == '\r') line.pop_back();
      return line;
    }
    struct pollfd p{f.ctl, POLLIN, 0};
    int rc = ::poll(&p, 1, timeout_ms(ctx));
    if (rc < 0 and errno == EINTR) continue;
    if (rc <= 0) return s::nullopt;
    char buf[1024];
    ssize_t n = ::read(f.ctl, buf, sizeof buf);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) return s::nullopt;
    f.inbuf.append(buf, static_cast<s::size_t>(n));
  }
}

// A whole reply (multi-line ones end at `NNN `).  Returns its code, or 0
// when the connection went away (which closes the session).
int read_reply(ExecContext& ctx, FtpSession& f) {
  str verbose = param_or(ctx.params, "ZFTP_VERBOSE", "450");
  str code;
  for (;;) {
    auto line = read_line(ctx, f);
    if (not line) {
      err("connection closed by remote host");
      disconnect(ctx, f);
      return 0;
    }
    if (not line->empty() and verbose.find((*line)[0]) != str::npos)
      s::fprintf(stderr, "%s\n", line->c_str());
    bool is_code = line->size() >= 3 and s::isdigit(static_cast<unsigned char>((*line)[0]))
               and s::isdigit(static_cast<unsigned char>((*line)[1]))
               and s::isdigit(static_cast<unsigned char>((*line)[2]));
    if (code.empty() and is_code) code = line->substr(0, 3);
    bool last = is_code and line->substr(0, 3) == code
            and (line->size() == 3 or (*line)[3] == ' ');
    if (last) {
      (void)ctx.params.set_scalar("ZFTP_REPLY", *line);
      (void)ctx.params.set_scalar("ZFTP_CODE", code);
      return s::atoi(code.c_str());
    }
  }
}

bool write_all(int fd, strv data) {
  while (not data.empty()) {
    ssize_t n = ::write(fd, data.data(), data.size());
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) return false;
    data.remove_prefix(static_cast<s::size_t>(n));
  }
  return true;
}

// Sends `cmd` and reads the reply; returns its code (0: connection gone).
int command(ExecContext& ctx, FtpSession& f, strv cmd) {
  if (not write_all(f.ctl, str(cmd) + "\r\n")) {
    err("failed sending control message");
    disconnect(ctx, f);
    return 0;
  }
  return read_reply(ctx, f);
}

int connect_to(const str& host, int port, str& ip) {
  struct addrinfo hints{}, *res = nullptr;
  hints.ai_socktype = SOCK_STREAM;
  str serv = s::to_string(port);
  if (int rc = ::getaddrinfo(host.c_str(), serv.c_str(), &hints, &res); rc != 0) {
    err("host not found: " + host);
    return -1;
  }
  int fd = -1;
  for (auto* ai = res; ai; ai = ai->ai_next) {
    fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
    // A server that hung up gives EPIPE, not a SIGPIPE that kills the shell.
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      char h[NI_MAXHOST];
      if (::getnameinfo(ai->ai_addr, ai->ai_addrlen, h, sizeof h, nullptr, 0, NI_NUMERICHOST) == 0)
        ip = h;
      break;
    }
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);
  if (fd < 0) err(str("connection failed: ") + s::strerror(errno));
  return fd;
}

// Opens a passive data connection.  -1 on failure.
int open_data(ExecContext& ctx, FtpSession& f) {
  int code = command(ctx, f, "PASV");
  if (code != 227) {
    if (code) err("server does not support passive mode");
    return -1;
  }
  str reply = param_or(ctx.params, "ZFTP_REPLY");
  auto open = reply.find('(');
  s::size_t pos = open == str::npos ? 4 : open + 1;
  while (pos < reply.size() and not s::isdigit(static_cast<unsigned char>(reply[pos]))) ++pos;
  int n[6];
  if (s::sscanf(reply.c_str() + pos, "%d,%d,%d,%d,%d,%d", &n[0], &n[1], &n[2], &n[3], &n[4], &n[5]) != 6) {
    err("bad reply to PASV: " + reply);
    return -1;
  }
  // The control connection's peer, not the address in the reply: servers
  // behind NAT announce addresses the client can't reach.
  str ip;
  return connect_to(f.ip.empty() ? f.host : f.ip, n[4] * 256 + n[5], ip);
}

bool set_type(ExecContext& ctx, FtpSession& f, char type) {
  if (f.ctl < 0) { f.type = type; return true; }
  int code = command(ctx, f, str("TYPE ") + type);
  if (code / 100 != 2) return false;
  f.type = type;
  return true;
}

void update_pwd(ExecContext& ctx, FtpSession& f) {
  if (command(ctx, f, "PWD") / 100 != 2) return;
  str r = param_or(ctx.params, "ZFTP_REPLY");
  auto a = r.find('"'), b = r.rfind('"');
  if (a != str::npos and b > a) f.pwd = r.substr(a + 1, b - a - 1);
}

// Copies the data connection to stdout (downloads), converting CRLF to
// LF in ASCII mode.
bool copy_down(int data, bool ascii) {
  char buf[8192];
  bool cr = false;
  for (;;) {
    ssize_t n = ::read(data, buf, sizeof buf);
    if (n < 0 and errno == EINTR) continue;
    if (n < 0) return false;
    if (n == 0) break;
    str out;
    for (ssize_t k = 0; k < n; ++k) {
      char c = buf[k];
      if (ascii) {
        if (cr and c != '\n') out += '\r';
        cr = c == '\r';
        if (cr) continue;
      }
      out += c;
    }
    if (not write_all(STDOUT_FILENO, out)) return false;
  }
  if (cr) (void)write_all(STDOUT_FILENO, "\r");
  return true;
}

// Copies stdin up the data connection, LF to CRLF in ASCII mode.
bool copy_up(int data, bool ascii) {
  char buf[8192];
  for (;;) {
    ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
    if (n < 0 and errno == EINTR) continue;
    if (n < 0) return false;
    if (n == 0) break;
    str out;
    for (ssize_t k = 0; k < n; ++k) {
      if (ascii and buf[k] == '\n') out += '\r';
      out += buf[k];
    }
    if (not write_all(data, out)) return false;
  }
  return true;
}

// One transfer: `cmd` over a fresh data connection, after `REST offset`
// when one is given.  `up` sends stdin, otherwise the data goes to stdout.
int transfer(ExecContext& ctx, FtpSession& f, const str& cmd, bool up, bool ascii,
             const str& offset = {}) {
  s::fflush(stdout);
  int data = open_data(ctx, f);
  if (data < 0) return f.ctl < 0 ? 2 : 1;
  if (not offset.empty() and command(ctx, f, "REST " + offset) / 100 != 3) {
    ::close(data);
    return f.ctl < 0 ? 2 : 1;
  }
  int code = command(ctx, f, cmd);
  if (code / 100 != 1 and code / 100 != 2) {
    ::close(data);
    return f.ctl < 0 ? 2 : 1;
  }
  bool ok = up ? copy_up(data, ascii) : copy_down(data, ascii);
  ::close(data);
  if (code / 100 == 1) code = read_reply(ctx, f);
  if (not ok) return 1;
  return code / 100 == 2 ? 0 : (f.ctl < 0 ? 2 : 1);
}

int login(ExecContext& ctx, FtpSession& f, const vec<str>& a) {
  str user = a.size() > 0 ? a[0] : (f.params.size() > 1 ? f.params[1] : "");
  str pass = a.size() > 1 ? a[1] : (f.params.size() > 2 ? f.params[2] : "");
  str acct = a.size() > 2 ? a[2] : (f.params.size() > 3 ? f.params[3] : "");
  if (user.empty()) return err("no user name given");
  int code = command(ctx, f, "USER " + user);
  if (code == 331) code = command(ctx, f, "PASS " + pass);
  if (code == 332) code = command(ctx, f, "ACCT " + acct);
  if (code / 100 != 2) {
    if (code) err("login failed");
    return f.ctl < 0 ? 2 : 1;
  }
  f.user = user;
  if (command(ctx, f, "SYST") / 100 == 2) {
    str r = param_or(ctx.params, "ZFTP_REPLY");
    f.system = r.size() > 4 ? r.substr(4) : str{};
  }
  (void)set_type(ctx, f, f.type);
  update_pwd(ctx, f);
  sync_params(ctx);
  return 0;
}

int open_session(ExecContext& ctx, FtpSession& f, vec<str> a) {
  if (a.empty()) a = f.params;
  if (a.empty()) return err("no host specified");
  if (f.ctl >= 0) {
    (void)command(ctx, f, "QUIT");
    disconnect(ctx, f);
  }
  str host = a[0];
  int port = ZFTP_PORT;
  // host:port, but not inside an IPv6 address.
  if (auto c = host.rfind(':'); c != str::npos and host.find(':') == c) {
    port = s::atoi(host.c_str() + c + 1);
    host.resize(c);
  }
  f.host = host;
  f.port = port;
  f.ctl = connect_to(host, port, f.ip);
  if (f.ctl < 0) { sync_params(ctx); return 1; }
  ::fcntl(f.ctl, F_SETFD, FD_CLOEXEC);
  int code = read_reply(ctx, f);
  if (code / 100 != 2) {
    if (code) err("server refused connection");
    disconnect(ctx, f);
    return 1;
  }
  sync_params(ctx);
  if (a.size() > 1) return login(ctx, f, vec<str>(a.begin() + 1, a.end()));
  return 0;
}

// "size mtime" of a remote file (SIZE / MDTM), -1 / empty when unknown.
str remote_info(ExecContext& ctx, FtpSession& f, const str& file, int& rc) {
  str size = "-1", mtime;
  if (command(ctx, f, "SIZE " + file) / 100 == 2) {
    str r = param_or(ctx.params, "ZFTP_REPLY");
    size = r.size() > 4 ? r.substr(4) : "-1";
  } else {
    rc = 1;
  }
  if (command(ctx, f, "MDTM " + file) / 100 == 2) {
    str r = param_or(ctx.params, "ZFTP_REPLY");
    mtime = r.size() > 4 ? r.substr(4) : str{};
  }
  return mtime.empty() ? size : size + " " + mtime;
}

int simple(ExecContext& ctx, FtpSession& f, const str& verb, const vec<str>& files) {
  int rc = 0;
  for (const auto& file : files) {
    int code = command(ctx, f, verb + " " + file);
    if (code / 100 != 2) rc = f.ctl < 0 ? 2 : 1;
    if (f.ctl < 0) break;
  }
  return rc;
}

int builtin_zftp(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return err("subcommand required");
  str sub = args[1];
  vec<str> a(args.begin() + 2, args.end());
  auto& st = state();
  auto& f  = st.cur();

  // Subcommands that don't need a connection.
  if (sub == "params") {
    if (a.empty()) {
      for (s::size_t k = 0; k < f.params.size(); ++k)
        s::printf("%s\n", k == 2 ? "********" : f.params[k].c_str());
      return f.params.empty() ? 1 : 0;
    }
    if (a.size() == 1 and a[0] == "-") f.params.clear();
    else f.params = a;
    return 0;
  }
  if (sub == "session") {
    if (a.empty()) {
      for (const auto& [name, _] : st.sessions) s::printf("%s\n", name.c_str());
      return 0;
    }
    st.current = a[0];
    st.sessions[a[0]];
    sync_params(ctx);
    return 0;
  }
  if (sub == "rmsession") {
    str name = a.empty() ? st.current : a[0];
    auto it = st.sessions.find(name);
    if (it == st.sessions.end()) return err("no such session: " + name);
    if (it->second.ctl >= 0) {
      (void)command(ctx, it->second, "QUIT");
      if (it->second.ctl >= 0) ::close(it->second.ctl);
    }
    st.sessions.erase(it);
    if (st.current == name) {
      st.current = st.sessions.empty() ? "default" : st.sessions.begin()->first;
      st.sessions[st.current];
    }
    sync_params(ctx);
    return 0;
  }
  if (sub == "type" or sub == "ascii" or sub == "binary") {
    char t = sub == "ascii" ? 'A' : sub == "binary" ? 'I' : 0;
    if (sub == "type") {
      if (a.empty()) { s::printf("%c\n", f.type); return 0; }
      char c = static_cast<char>(s::toupper(static_cast<unsigned char>(a[0][0])));
      if (a[0].size() != 1 or (c != 'A' and c != 'I' and c != 'B'))
        return err("bad type " + a[0]);
      t = c == 'B' ? 'I' : c;
    }
    if (not set_type(ctx, f, t)) return 1;
    sync_params(ctx);
    return 0;
  }
  if (sub == "mode") {
    if (a.empty()) { s::printf("S\n"); return 0; }
    if (a[0] == "S" or a[0] == "s") return 0;
    return err("mode " + a[0] + " not supported");
  }
  if (sub == "local") {
    int rc = 0;
    for (const auto& file : a.empty() ? vec<str>{"."} : a) {
      struct stat sb{};
      if (::stat(file.c_str(), &sb) != 0) { rc = 1; s::printf("-1\n"); continue; }
      struct tm tm{};
      ::gmtime_r(&sb.st_mtime, &tm);
      char when[32];
      s::strftime(when, sizeof when, "%Y%m%d%H%M%S", &tm);
      s::printf("%lld %s\n", static_cast<long long>(sb.st_size), when);
    }
    return rc;
  }
  if (sub == "open")  return open_session(ctx, f, a);
  if (sub == "test") {
    if (f.ctl < 0) return 1;
    struct pollfd p{f.ctl, POLLIN, 0};
    if (::poll(&p, 1, 0) > 0) {
      char buf[512];
      ssize_t n = ::read(f.ctl, buf, sizeof buf);
      if (n <= 0) { disconnect(ctx, f); return 2; }
      f.inbuf.append(buf, static_cast<s::size_t>(n));
      if (f.inbuf.starts_with("421")) { disconnect(ctx, f); return 2; }
    }
    return 0;
  }

  if (f.ctl < 0) return err("not connected.");
  if (sub == "login" or sub == "user") return login(ctx, f, a);
  if (sub == "close" or sub == "quit") {
    (void)command(ctx, f, "QUIT");
    disconnect(ctx, f);
    return 0;
  }
  if (sub == "cd" or sub == "cdup") {
    int code = sub == "cdup" or (not a.empty() and a[0] == "..")
             ? command(ctx, f, "CDUP")
             : command(ctx, f, "CWD " + (a.empty() ? str("/") : a[0]));
    if (code / 100 != 2) return f.ctl < 0 ? 2 : 1;
    update_pwd(ctx, f);
    sync_params(ctx);
    return 0;
  }
  if (sub == "dir" or sub == "ls") {
    str cmd = sub == "dir" ? "LIST" : "NLST";
    for (const auto& w : a) cmd += " " + w;
    char saved = f.type;
    if (saved != 'A' and not set_type(ctx, f, 'A')) return 1;
    int rc = transfer(ctx, f, cmd, false, true);
    if (saved != 'A' and f.ctl >= 0) (void)set_type(ctx, f, saved);
    return rc;
  }
  bool ascii = f.type == 'A';
  if (sub == "get" or sub == "getat") {
    if (a.empty()) return err("not enough arguments");
    if (sub == "getat") {
      if (a.size() != 2) return err("getat: file and offset required");
      return transfer(ctx, f, "RETR " + a[0], false, ascii, a[1]);
    }
    int rc = 0;
    for (const auto& file : a) {
      rc = transfer(ctx, f, "RETR " + file, false, ascii);
      if (rc) break;
    }
    return rc;
  }
  if (sub == "put" or sub == "putat" or sub == "append" or sub == "appendat") {
    if (a.empty()) return err("not enough arguments");
    str verb = sub.starts_with("put") ? "STOR " : "APPE ";
    if (sub.ends_with("at")) {
      if (a.size() != 2) return err(sub + ": file and offset required");
      return transfer(ctx, f, verb + a[0], true, ascii, a[1]);
    }
    int rc = 0;
    for (const auto& file : a) {
      rc = transfer(ctx, f, verb + file, true, ascii);
      if (rc) break;
    }
    return rc;
  }
  if (sub == "delete") return simple(ctx, f, "DELE", a);
  if (sub == "mkdir")  return simple(ctx, f, "MKD", a);
  if (sub == "rmdir")  return simple(ctx, f, "RMD", a);
  if (sub == "rename") {
    if (a.size() != 2) return err("rename: old and new names required");
    if (command(ctx, f, "RNFR " + a[0]) / 100 != 3) return f.ctl < 0 ? 2 : 1;
    return command(ctx, f, "RNTO " + a[1]) / 100 == 2 ? 0 : (f.ctl < 0 ? 2 : 1);
  }
  if (sub == "remote") {
    int rc = 0;
    for (const auto& file : a) s::printf("%s\n", remote_info(ctx, f, file, rc).c_str());
    return rc;
  }
  if (sub == "quote" or sub == "site") {
    if (a.empty()) return err("not enough arguments");
    str cmd = sub == "site" ? "SITE" : "";
    for (const auto& w : a) cmd += (cmd.empty() ? "" : " ") + w;
    int code = command(ctx, f, cmd);
    return code and code < 400 ? 0 : (f.ctl < 0 ? 2 : 1);
  }
  return err("no such subcommand: " + sub);
}

}  // namespace

void register_zftp_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/zftp", [](ModuleFeatures& out, ExecContext& ctx) {
    // A fresh load starts with no sessions, as in a new shell.
    for (auto& [_, f] : state().sessions)
      if (f.ctl >= 0) ::close(f.ctl);
    state() = State{};
    out.builtins.push_back(BuiltinDef{"zftp", builtin_zftp, BINF_NONE, 0});
    for (const char* p : {"ZFTP_VERBOSE", "ZFTP_TMOUT"})
      if (not ctx.params.lookup(p)) (void)ctx.params.set_scalar(p, strv(p) == "ZFTP_VERBOSE" ? "450" : "60");
    sync_params(ctx);
    ExecContext* ctxp = &ctx;
    out.on_unload = [ctxp] {
      for (auto& [_, f] : state().sessions)
        if (f.ctl >= 0) ::close(f.ctl);
      state() = State{};
      for (const char* p : {"ZFTP_HOST", "ZFTP_PORT", "ZFTP_IP", "ZFTP_USER", "ZFTP_PWD",
                            "ZFTP_SYSTEM", "ZFTP_TYPE", "ZFTP_MODE", "ZFTP_SESSION",
                            "ZFTP_REPLY", "ZFTP_CODE"})
        drop_module_param(ctxp->params, p);
    };
  });
}

}  // namespace jfsh
