// Tests for M11 Phase 11b: the rest of the specialized modules
// (zsh/net/tcp, zsh/zftp, zsh/termcap, zsh/terminfo, zsh/curses,
// zsh/nearcolor, zsh/mapfile, zsh/db/gdbm, zsh/private, zsh/zpty,
// zsh/zprof, zsh/attr, zsh/cap, zsh/compctl, zsh/clone, zsh/watch,
// zsh/newuser; zsh/deltochar is in editor_test.cpp) and the core support
// they needed: subscripted assignment and per-element parameter handlers.
// Uses the in-process Shell fixture; network tests use loopback on
// ephemeral ports.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utmpx.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include <gtest/gtest.h>

#include <shell_fixture.h>

#include <jfsh/editor/completion.h>
#include <jfsh/editor/zle.h>

namespace jfsh::test {

namespace {

str read_text(const sf::path& p) {
  s::ifstream in(p);
  s::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Runs `src` in `dir`, then goes back.
Run run_in(Shell& sh, const sf::path& dir, const str& src) {
  return sh.run("builtin cd " + dir.string() + " && { " + src + "\n}; builtin cd " +
                sf::current_path().string());
}

}  // namespace

// ---------------------------------------------------------------------------
// Core support: subscripted assignment, element handlers, PM_PRIVATE.
// ---------------------------------------------------------------------------

TEST(ModulesM11, SubscriptedAssignmentToArraysAndHashes) {
  Shell sh;
  auto r = sh.run(
      "typeset -A h; h[a]=1; h[b c]=2; k=key; h[$k]=v\n"
      "print -r -- ${h[a]} ${h[b c]} ${h[key]}\n"
      "a=(x y z); a[2]=Q; a[5]=E; print -r -- \"$a\" $#a\n"
      "a[1]+=P; a[-1]=L; print -r -- $a[1] $a[5]\n"
      "s=hello; s[1]=J; print -r -- $s\n"
      "unset 'h[a]'; print -r -- ${(ok)h}");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(r.out, "1 2 v\nx Q z  E 5\nxP L\nJello\nb c key\n");
}

TEST(ModulesM11, SubscriptedAssignmentArraySlice) {
  Shell sh;
  EXPECT_EQ(sh.run("a=(1 2 3); a[2]=(x y); print -r -- $a").out, "1 x y 3\n");
}

// ---------------------------------------------------------------------------
// zsh/net/tcp
// ---------------------------------------------------------------------------

TEST(ModulesM11, ZtcpNotLoadedByDefault) {
  Shell sh;
  EXPECT_NE(sh.run("ztcp").rc, 0);
}

TEST(ModulesM11, ZtcpListenConnectAcceptRoundTrip) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/net/tcp\n"
      "ztcp -l 0; l=$REPLY\n"
      "port=${${(s: :)$(ztcp -L)}[4]}\n"
      "ztcp 127.0.0.1 $port; c=$REPLY\n"
      "ztcp -a $l; a=$REPLY\n"
      "print -u $c hello; read -u $a line; print -r -- got:$line\n"
      "ztcp | wc -l | tr -d ' '\n"
      "ztcp -c; ztcp | wc -l | tr -d ' '");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(r.out, "got:hello\n3\n0\n");
}

TEST(ModulesM11, ZtcpAcceptTestWithoutPendingConnection) {
  Shell sh;
  auto r = sh.run("zmodload zsh/net/tcp; ztcp -l 0; ztcp -at $REPLY; print $?; ztcp -c");
  EXPECT_EQ(r.out, "1\n");
}

TEST(ModulesM11, ZtcpCloseUnknownFdFails) {
  Shell sh;
  auto r = sh.run("zmodload zsh/net/tcp; ztcp -c 97");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: ztcp: fd 97 not found in tcp table\n");
}

// ---------------------------------------------------------------------------
// zsh/zftp, against a scripted server on loopback.
// ---------------------------------------------------------------------------

namespace {

struct FakeFtp {
  int listen_fd = -1;
  int port = 0;
  s::thread th;
  s::map<str, str> files{{"hello.txt", "line1\nline2\n"}};
  str cwd = "/home/ftp";

  static int listen_any(int& port_out) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a);
    ::listen(fd, 1);
    socklen_t len = sizeof a;
    ::getsockname(fd, reinterpret_cast<struct sockaddr*>(&a), &len);
    port_out = ntohs(a.sin_port);
    return fd;
  }

  FakeFtp() {
    listen_fd = listen_any(port);
    th = s::thread([this] { serve(); });
  }
  ~FakeFtp() {
    if (th.joinable()) th.join();
    ::close(listen_fd);
  }

  static void send(int fd, const str& text) { (void)::write(fd, text.data(), text.size()); }

  void serve() {
    int c = ::accept(listen_fd, nullptr, nullptr);
    if (c < 0) return;
    send(c, "220 fake ftp ready\r\n");
    str buf;
    int data_listen = -1;
    char type = 'A';
    for (;;) {
      char ch;
      ssize_t n = ::read(c, &ch, 1);
      if (n <= 0) break;
      if (ch != '\n') { buf += ch; continue; }
      if (not buf.empty() and buf.back() == '\r') buf.pop_back();
      str line = s::move(buf);
      buf.clear();
      str verb = line.substr(0, line.find(' '));
      str arg  = line.find(' ') == str::npos ? "" : line.substr(line.find(' ') + 1);
      auto data = [&] { int d = ::accept(data_listen, nullptr, nullptr); ::close(data_listen); return d; };
      if (verb == "USER")      send(c, "331 password please\r\n");
      else if (verb == "PASS") send(c, arg == "secret" ? "230 logged in\r\n" : "530 no\r\n");
      else if (verb == "SYST") send(c, "215 UNIX Type: L8\r\n");
      else if (verb == "PWD")  send(c, "257 \"" + cwd + "\" is current directory\r\n");
      else if (verb == "TYPE") { type = arg[0]; send(c, "200 type set\r\n"); }
      else if (verb == "CWD")  { cwd += "/" + arg; send(c, "250 ok\r\n"); }
      else if (verb == "MKD")  { files[arg + "/"] = ""; send(c, "257 created\r\n"); }
      else if (verb == "DELE") {
        send(c, files.erase(arg) ? "250 deleted\r\n" : "550 no such file\r\n");
      } else if (verb == "SIZE") {
        auto it = files.find(arg);
        send(c, it == files.end() ? str("550 no\r\n") : "213 " + s::to_string(it->second.size()) + "\r\n");
      } else if (verb == "MDTM") {
        send(c, "213 20260102030405\r\n");
      } else if (verb == "PASV") {
        int dport = 0;
        data_listen = listen_any(dport);
        send(c, "227 Entering Passive Mode (127,0,0,1," + s::to_string(dport / 256) + "," +
                    s::to_string(dport % 256) + ")\r\n");
      } else if (verb == "LIST" or verb == "NLST") {
        send(c, "150 here it comes\r\n");
        int d = data();
        for (const auto& [name, _] : files) send(d, name + "\r\n");
        ::close(d);
        send(c, "226 done\r\n");
      } else if (verb == "RETR") {
        auto it = files.find(arg);
        if (it == files.end()) { ::close(data_listen); send(c, "550 no such file\r\n"); continue; }
        send(c, "150 sending\r\n");
        int d = data();
        str body = it->second;
        if (type == 'A') {
          str crlf;
          for (char x : body) { if (x == '\n') crlf += '\r'; crlf += x; }
          body = crlf;
        }
        send(d, body);
        ::close(d);
        send(c, "226 done\r\n");
      } else if (verb == "STOR") {
        send(c, "150 go ahead\r\n");
        int d = data();
        str body;
        char b[512];
        while ((n = ::read(d, b, sizeof b)) > 0) body.append(b, static_cast<s::size_t>(n));
        ::close(d);
        files[arg] = body;
        send(c, "226 stored\r\n");
      } else if (verb == "QUIT") {
        send(c, "221 bye\r\n");
        break;
      } else {
        send(c, "502 not implemented\r\n");
      }
    }
    ::close(c);
  }
};

}  // namespace

TEST(ModulesM11, ZftpSessionAgainstFakeServer) {
  FakeFtp srv;
  Shell sh;
  str port = s::to_string(srv.port);
  auto r = sh.run(
      "zmodload zsh/zftp; ZFTP_VERBOSE=\n"
      "zftp open 127.0.0.1:" + port + " anon secret || print open failed\n"
      "print -r -- $ZFTP_HOST $ZFTP_PORT $ZFTP_USER $ZFTP_SYSTEM $ZFTP_PWD $ZFTP_TYPE\n"
      "zftp ls\n"
      "zftp ascii; zftp get hello.txt\n"
      "print -r -- payload | zftp put up.txt; print put:$?\n"
      "zftp remote hello.txt\n"
      "zftp cd sub; print -r -- $ZFTP_PWD\n"
      "zftp delete nope; print delete:$? $ZFTP_CODE\n"
      "zftp test; print test:$?\n"
      "zftp close; print -r -- ${ZFTP_HOST-unset}\n"
      "zftp test; print test:$?");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(r.out,
            "127.0.0.1 " + port + " anon UNIX Type: L8 /home/ftp I\n"
            "hello.txt\n"
            "line1\nline2\n"
            "put:0\n"
            "12 20260102030405\n"
            "/home/ftp/sub\n"
            "delete:1 550\n"
            "test:0\n"
            "unset\n"
            "test:1\n");
  // ASCII mode: the line ends go over the wire as CRLF.
  EXPECT_EQ(srv.files["up.txt"], "payload\r\n");
}

TEST(ModulesM11, ZftpRequiresConnection) {
  Shell sh;
  auto r = sh.run("zmodload zsh/zftp; zftp ls");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: zftp: not connected.\n");
}

TEST(ModulesM11, ZftpParamsAndSessions) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zftp\n"
      "zftp params host.example user pw; zftp params\n"
      "zftp session other; print -r -- $ZFTP_SESSION; zftp session\n"
      "zftp rmsession other; print -r -- $ZFTP_SESSION");
  EXPECT_EQ(r.out, "host.example\nuser\n********\nother\ndefault\nother\ndefault\n");
}

// ---------------------------------------------------------------------------
// zsh/termcap, zsh/terminfo, zsh/curses, zsh/nearcolor
// ---------------------------------------------------------------------------

#ifdef JFSH_HAVE_CURSES

TEST(ModulesM11, EchotiReadsTerminfo) {
  Shell sh;
  auto r = sh.run("TERM=xterm-256color; zmodload zsh/terminfo\n"
                  "echoti colors; echoti am; echoti bold | od -An -c | tr -d ' \\n'; print");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(r.out, "256\nyes\n033[1m\n");
}

TEST(ModulesM11, EchotiUnknownCapability) {
  Shell sh;
  auto r = sh.run("TERM=xterm; zmodload zsh/terminfo; echoti nosuchcap");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: echoti: no such terminfo capability: nosuchcap\n");
}

TEST(ModulesM11, EchotiWithParameters) {
  Shell sh;
  auto r = sh.run("TERM=xterm; zmodload zsh/terminfo; echoti cup 4 9 | od -An -c | tr -d ' \\n'; print");
  EXPECT_EQ(r.out, "033[5;10H\n");
}

TEST(ModulesM11, TerminfoHash) {
  Shell sh;
  auto r = sh.run("TERM=xterm-256color; zmodload zsh/terminfo\n"
                  "print -r -- $terminfo[colors] ${+terminfo[nosuch]} ${#${(k)terminfo}[@]}");
  ASSERT_EQ(r.rc, 0) << r.err;
  auto words = r.out;
  EXPECT_TRUE(words.starts_with("256 0 ")) << words;
  EXPECT_GT(s::atoi(words.substr(6).c_str()), 50);
}

TEST(ModulesM11, EchotcAndTermcapHash) {
  Shell sh;
  auto r = sh.run("TERM=xterm; zmodload zsh/termcap\n"
                  "echotc co; echotc am; print -r -- $termcap[Co]\n"
                  "echotc cm 9 4 | od -An -c | tr -d ' \\n'; print\n"
                  "echotc zz");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "80\nyes\n8\n033[5;10H\n");
  EXPECT_EQ(r.err, "jfsh: echotc: no such capability: zz\n");
}

TEST(ModulesM11, ZcursesNeedsInit) {
  Shell sh;
  auto r = sh.run("zmodload zsh/curses; zcurses addwin w 1 1 0 0");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: zcurses: addwin: not initialised\n");
}

TEST(ModulesM11, ZcursesWindowsAndPosition) {
  Shell sh;
  auto r = sh.run(
      "TERM=xterm; zmodload zsh/curses\n"
      "zcurses init\n"
      "zcurses addwin w 3 10 1 2; zcurses string w hi; zcurses attr w bold\n"
      "zcurses position w pos; wins=($zcurses_windows)\n"
      "zcurses addwin w 1 1 0 0; dup=$?\n"
      "zcurses delwin w; after=($zcurses_windows)\n"
      "zcurses end");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(sh.run("print -r -- $pos / $wins / $dup / $after").out,
            "0 2 1 2 3 10 / stdscr w / 1 / stdscr\n");
}

#endif  // JFSH_HAVE_CURSES

TEST(ModulesM11, NearcolorMapsTruecolorPromptColours) {
  Shell sh;
  auto esc = [&](const str& src) {
    return sh.run(src + " | od -An -c | tr -d ' \\n'; print").out;
  };
  EXPECT_EQ(esc("print -nP '%F{#ff0000}'"), "033[38;2;255;0;0m\n");
  sh.run("zmodload zsh/nearcolor");
  EXPECT_EQ(esc("TERM=xterm-256color; print -nP '%F{#ff0000}'"), "033[38;5;196m\n");
  EXPECT_EQ(esc("TERM=xterm-256color; print -nP '%K{#808080}'"), "033[48;5;244m\n");
  EXPECT_EQ(esc("TERM=vt100; print -nP '%F{#00ff00}'"), "033[92m\n");
  sh.run("zmodload -u zsh/nearcolor");
  EXPECT_EQ(esc("print -nP '%F{#ff0000}'"), "033[38;2;255;0;0m\n");
}

// ---------------------------------------------------------------------------
// zsh/mapfile and zsh/db/gdbm
// ---------------------------------------------------------------------------

TEST(ModulesM11, MapfileReadsWritesAndRemoves) {
  Shell sh;
  TempDir td;
  td.file("in.txt", "from disk\n", sf::perms::owner_read | sf::perms::owner_write);
  str d = td.path.string();
  auto r = sh.run(
      "zmodload zsh/mapfile\n"
      "print -rn -- \"$mapfile[" + d + "/in.txt]\"\n"
      "mapfile[" + d + "/out.txt]='written'\n"
      "mapfile[" + d + "/out.txt]+=' more'\n"
      "print -r -- ${+mapfile[" + d + "/none]}\n"
      "unset 'mapfile[" + d + "/in.txt]'");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(r.out, "from disk\n0\n");
  EXPECT_EQ(read_text(td.path / "out.txt"), "written more");
  EXPECT_FALSE(sf::exists(td.path / "in.txt"));
}

TEST(ModulesM11, MapfileKeysAreFilesInCwd) {
  Shell sh;
  TempDir td;
  td.file("a", "1", sf::perms::owner_read | sf::perms::owner_write);
  td.file("b", "2", sf::perms::owner_read | sf::perms::owner_write);
  sf::create_directory(td.path / "dir");
  auto r = run_in(sh, td.path, "zmodload zsh/mapfile; print -r -- ${(ok)mapfile}");
  EXPECT_EQ(r.out, "a b\n");
}

TEST(ModulesM11, ZtieStoresAndReopens) {
  Shell sh;
  TempDir td;
  str db = (td.path / "db").string();
  auto r = sh.run(
      "zmodload zsh/db/gdbm\n"
      "ztie -d db/gdbm -f " + db + " D\n"
      "D[x]=1; D[y]=two; D[z]=3; unset 'D[z]'\n"
      "print -r -- ${(ok)D} $D[y]\n"
      "zgdbmpath D; print -r -- ${REPLY:t}\n"
      "zuntie D; print -r -- ${+D}\n"
      "ztie -r -d db/gdbm -f " + db + " E; print -r -- $E[x] $E[y]\n"
      "zuntie E; print zuntie:$?; zuntie -u E; print zuntie-u:$?");
#if defined(JFSH_HAVE_GDBM) || defined(JFSH_HAVE_NDBM) || defined(__APPLE__)
  EXPECT_EQ(r.out, "x y two\ndb\n0\n1 two\nzuntie:1\nzuntie-u:0\n") << r.err;
#endif
}

TEST(ModulesM11, ZtieRejectsOtherTypes) {
  Shell sh;
  auto r = sh.run("zmodload zsh/db/gdbm; ztie -d db/other -f x P");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: ztie: unsupported database type: db/other\n");
}

// ---------------------------------------------------------------------------
// zsh/private
// ---------------------------------------------------------------------------

TEST(ModulesM11, PrivateHiddenFromCallees) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/private\n"
      "p=outer\n"
      "g() { print -r -- g:$p; p=set-by-g }\n"
      "f() { private p=inner; g; print -r -- f:$p }\n"
      "f; print -r -- top:$p");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(r.out, "g:outer\nf:inner\ntop:set-by-g\n");
}

TEST(ModulesM11, PrivateArraysAndCalleeLocals) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/private\n"
      "g() { local a=(g); print -r -- g:$a }\n"
      "f() { private a=(1 2 3); g; print -r -- f:$#a }\n"
      "f; print -r -- top:${a-unset}");
  EXPECT_EQ(r.out, "g:g\nf:3\ntop:unset\n");
}

// ---------------------------------------------------------------------------
// zsh/zpty
// ---------------------------------------------------------------------------

TEST(ModulesM11, ZptyRunsWritesAndReads) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zpty\n"
      "zpty P 'print first; read x; print got:$x'\n"
      "zpty -r P line; print -r -- ${line%$'\\r\\n'}\n"
      "zpty -w P second\n"
      "zpty -r P out '*got:*'$'\\n'; print -r -- ${out%$'\\r\\n'}\n"
      "zpty -rt P out2; print -r -- rc:$? ${#out2}\n"
      "zpty -L\n"
      "zpty -d P; zpty | wc -l | tr -d ' '");
  EXPECT_EQ(r.rc, 0) << r.err;
  // Echo is off, so the read sees only the command's output; -t finds
  // nothing more waiting.
  EXPECT_EQ(r.out, "first\ngot:second\nrc:1 0\nzpty P print first; read x; print got:$x\n0\n");
}

TEST(ModulesM11, ZptyTestAndUnknownName) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zpty\n"
      "zpty Q 'sleep 5'; zpty -t Q; print running:$?\n"
      "zpty -d Q; zpty -w Q x");
  EXPECT_EQ(r.out, "running:0\n");
  EXPECT_EQ(r.err, "jfsh: zpty: no such pty command: Q\n");
}

TEST(ModulesM11, ZptyNotABuiltinWithoutModule) {
  Shell sh;
  EXPECT_NE(sh.run("zpty x true").rc, 0);
}

// ---------------------------------------------------------------------------
// zsh/zprof
// ---------------------------------------------------------------------------

TEST(ModulesM11, ZprofCountsCallsAndClears) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zprof\n"
      "inner() { : }\n"
      "outer() { inner; inner; inner }\n"
      "outer; outer\n"
      "zprof");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_NE(r.out.find("num  calls"), str::npos);
  // Calls column per function in the summary table.
  EXPECT_NE(r.out.find("    6 "), str::npos) << r.out;
  EXPECT_NE(r.out.find("  inner\n"), str::npos);
  EXPECT_NE(r.out.find("  outer\n"), str::npos);
  // The call graph part lists outer as inner's caller (6 of inner's 6).
  EXPECT_NE(r.out.find("6/6"), str::npos) << r.out;
  sh.run("zprof -c");
  EXPECT_EQ(sh.run("zprof | wc -l | tr -d ' '").out, "2\n");
  // Unloading stops the profiling.
  sh.run("zmodload -u zsh/zprof");
  EXPECT_TRUE(sh.ctx.hooks.function.empty());
}

// ---------------------------------------------------------------------------
// zsh/attr and zsh/cap
// ---------------------------------------------------------------------------

TEST(ModulesM11, AttrSetGetListDelete) {
  Shell sh;
  TempDir td;
  str f = td.file("f", "x", sf::perms::owner_read | sf::perms::owner_write).string();
  auto r = sh.run(
      "zmodload zsh/attr\n"
      "zsetattr " + f + " user.jfsh hello || exit 7\n"
      "zgetattr " + f + " user.jfsh; zgetattr " + f + " user.jfsh val; print -r -- $val\n"
      "zlistattr " + f + " names; for n in $names; do [[ $n == user.jfsh ]] && print -r -- $n; done\n"
      "zdelattr " + f + " user.jfsh; zgetattr " + f + " user.jfsh 2>/dev/null; print $?");
  if (r.rc == 7) GTEST_SKIP() << "no xattr support on the temp filesystem";
  EXPECT_EQ(r.out, "hello\nhello\nuser.jfsh\n1\n");
}

TEST(ModulesM11, CapBuiltinsExist) {
  Shell sh;
  auto r = sh.run("zmodload zsh/cap; whence -w cap getcap setcap");
  EXPECT_EQ(r.out, "cap: builtin\ngetcap: builtin\nsetcap: builtin\n");
#ifndef JFSH_HAVE_LIBCAP
  auto c = sh.run("cap");
  EXPECT_EQ(c.rc, 1);
  EXPECT_EQ(c.err, "jfsh: cap: not supported on this system\n");
#endif
}

// ---------------------------------------------------------------------------
// zsh/compctl
// ---------------------------------------------------------------------------

TEST(ModulesM11, CompctlListsAndDeletes) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/compctl\n"
      "compctl -k '(alpha beta)' foo\n"
      "compctl -f -/ bar baz\n"
      "compctl -L\n"
      "compctl + bar; compctl -L bar baz");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_EQ(r.out,
            "compctl -f -/ bar\n"
            "compctl -f -/ baz\n"
            "compctl -k '(alpha beta)' foo\n"
            "compctl -f -/ baz\n");
}

TEST(ModulesM11, CompctlUnsupportedExtended) {
  Shell sh;
  auto r = sh.run("zmodload zsh/compctl; compctl -x 's[-]' -k '(a)' -- foo");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: compctl: unsupported option: -x (extended completion)\n");
}

TEST(ModulesM11, CompctlFeedsCompletion) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/compctl\n"
      "colors=(red green grey)\n"
      "compctl -k colors paint\n"
      "_gen() { reply=(${1}one ${1}two) }\n"
      "compctl -K _gen gen");
  ASSERT_EQ(r.rc, 0) << r.err;
  auto& eng = *sh.ctx.zle->completion;
  auto words = [&](strv line) {
    vec<str> v;
    for (const auto& m : eng.complete(line, line.size()).matches) v.push_back(m.word);
    return v;
  };
  EXPECT_EQ(words("paint gr"), (vec<str>{"green", "grey"}));
  EXPECT_EQ(words("gen x"), (vec<str>{"xone", "xtwo"}));
}

// ---------------------------------------------------------------------------
// zsh/clone, zsh/watch, zsh/newuser
// ---------------------------------------------------------------------------

TEST(ModulesM11, CloneRunsOnAnotherTerminal) {
  int master = -1, slave = -1;
  char name[256];
  ASSERT_EQ(::openpty(&master, &slave, name, nullptr, nullptr), 0);
  TempDir td;
  str out = (td.path / "out").string();
  str cmd = str(JFSH_BINARY) + " -f -c 'zmodload zsh/clone; clone " + name +
            "; if [[ $TTY == " + name + " ]]; then print -r -- child:$TTY > " + out +
            "; else print parent:${!:+set}; fi'";
  s::FILE* p = ::popen(cmd.c_str(), "r");
  ASSERT_NE(p, nullptr);
  char buf[128] = {};
  str got;
  while (s::fgets(buf, sizeof buf, p)) got += buf;
  ::pclose(p);
  EXPECT_EQ(got, "parent:set\n");
  for (int i = 0; i < 100 and read_text(out).empty(); ++i) ::usleep(20000);
  EXPECT_EQ(read_text(out), "child:" + str(name) + "\n");
  ::close(master);
  ::close(slave);
}

TEST(ModulesM11, WatchLogListsCurrentLogins) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload zsh/watch; log").rc, 1);   // $watch unset
  // As many lines as utmpx has user sessions.
  int n = 0;
  ::setutxent();
  while (struct utmpx* u = ::getutxent()) if (u->ut_type == USER_PROCESS) ++n;
  ::endutxent();
  auto r = sh.run("watch=(all); WATCHFMT='%n|%(a.in.out)'; log");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(static_cast<int>(s::count(r.out.begin(), r.out.end(), '\n')), n);
  if (n) EXPECT_NE(r.out.find("|in\n"), str::npos);
  EXPECT_EQ(sh.run("watch=(nobody-here-xyz); log").out, "");
  EXPECT_EQ(sh.ctx.hooks.before_prompt.size(), 1u);
  sh.run("zmodload -u zsh/watch");
  EXPECT_TRUE(sh.ctx.hooks.before_prompt.empty());
}

TEST(ModulesM11, NewuserWritesStarterRc) {
  Shell sh;
  TempDir td;
  str d = td.path.string();
  auto r = sh.run("ZDOTDIR=" + d + "; zmodload zsh/newuser\n"
                  "zsh-newuser-install <<< $'1\\n2'");
  EXPECT_EQ(r.rc, 0) << r.err;
  EXPECT_NE(r.out.find("Wrote " + d + "/.jfshrc with the classic prompt."), str::npos) << r.out;
  str rc = read_text(td.path / ".jfshrc");
  EXPECT_NE(rc.find("PS1='%n@%m %~ %# '"), str::npos) << rc;
  EXPECT_EQ(sh.get("PS1"), "%n@%m %~ %# ");
  // With a startup file there, it does nothing (unless forced).
  auto again = sh.run("zsh-newuser-install </dev/null");
  EXPECT_EQ(again.out, "");
}

TEST(ModulesM11, NewuserQuitAndBlank) {
  Shell sh;
  TempDir td;
  str d = td.path.string();
  sh.run("ZDOTDIR=" + d + "; zmodload zsh/newuser");
  sh.run("zsh-newuser-install <<< q");
  EXPECT_FALSE(sf::exists(td.path / ".jfshrc"));
  sh.run("zsh-newuser-install <<< 0");
  EXPECT_EQ(read_text(td.path / ".jfshrc"), "# Created by zsh-newuser-install.\n");
}

}  // namespace jfsh::test
