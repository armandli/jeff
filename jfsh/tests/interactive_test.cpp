// Interactive job control (M6) end to end: the jfsh binary runs in a
// pseudo-terminal and is driven with keystrokes, as a user would.  The
// expected lines are what zsh 5.9 prints for the same keystrokes (`zsh -f
// -i`), with "jfsh:" for "zsh:".

#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

// jfsh -i on the slave side of a fresh pty.
struct Pty {
  pid_t       pid = -1;
  int         fd  = -1;
  std::string out;           // everything read so far
  std::size_t mark = 0;      // where the next expect() starts looking

  // The startup files come from `zdotdir`: by default one that doesn't
  // exist, so the user's own files stay out of the tests.
  explicit Pty(const std::string& zdotdir = "/nonexistent/jfsh-test-zdotdir") {
    pid = ::forkpty(&fd, nullptr, nullptr, nullptr);
    if (pid == 0) {
      ::setenv("PS1", "%% ", 1);
      ::setenv("TERM", "dumb", 1);
      ::setenv("ZDOTDIR", zdotdir.c_str(), 1);
      ::unsetenv("HISTFILE");
      // vi editing would follow an $EDITOR that mentions vi.
      ::unsetenv("EDITOR");
      ::unsetenv("VISUAL");
      ::execl(JFSH_BINARY, JFSH_BINARY, "--norouter", "-i", static_cast<char*>(nullptr));
      _exit(127);
    }
  }
  // The master is closed before the wait: on macOS a dying process blocks
  // closing its tty until unread output drains, so it would never be reaped.
  ~Pty() {
    if (pid > 0) ::kill(pid, SIGKILL);
    if (fd >= 0) ::close(fd);
    if (pid > 0) {
      int st;
      ::waitpid(pid, &st, 0);
    }
  }

  void send(std::string_view s) const {
    while (not s.empty()) {
      ssize_t n = ::write(fd, s.data(), s.size());
      if (n < 0) {
        if (errno == EINTR) continue;
        return;
      }
      s.remove_prefix(static_cast<std::size_t>(n));
    }
  }

  // Reads whatever arrives within `t`.
  void pump(std::chrono::milliseconds t) {
    auto end = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < end) {
      struct pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      char buf[4096];
      ssize_t n = ::read(fd, buf, sizeof buf);
      if (n <= 0) return;
      out.append(buf, static_cast<std::size_t>(n));
    }
  }

  // Waits until `needle` shows up after the last match.
  bool expect(std::string_view needle, std::chrono::milliseconds t = 5000ms) {
    auto end = std::chrono::steady_clock::now() + t;
    for (;;) {
      auto at = out.find(needle, mark);
      if (at != std::string::npos) {
        mark = at + needle.size();
        return true;
      }
      if (std::chrono::steady_clock::now() >= end) return false;
      pump(50ms);
    }
  }

  // Sends `key` until `needle` appears: a key sent before the command has
  // started reaches the line editor instead, which ignores it.
  bool key_until(std::string_view key, std::string_view needle, int tries = 20) {
    for (int i = 0; i < tries; ++i) {
      send(key);
      if (expect(needle, 300ms)) return true;
    }
    return false;
  }

  // Sends a command line and waits for the prompt that follows it.
  void line(std::string_view cmd) {
    send(cmd);
    send("\n");
  }

  bool exits(int& status, std::chrono::milliseconds t = 5000ms) {
    auto end = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < end) {
      pump(50ms);
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        pid = -1;
        return true;
      }
    }
    return false;
  }

  std::string tail() const { return out.substr(mark); }
};

TEST(Interactive, CtrlZSuspendsAndFgResumes) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30");
  ASSERT_TRUE(sh.key_until("\x1a", "jfsh: suspended  sleep 30")) << sh.out;
  sh.line("jobs");
  EXPECT_TRUE(sh.expect("[1]  + suspended  sleep 30")) << sh.out;
  sh.line("bg");
  EXPECT_TRUE(sh.expect("[1]  + continued  sleep 30")) << sh.out;
  sh.line("jobs");
  EXPECT_TRUE(sh.expect("[1]  + running    sleep 30")) << sh.out;
  sh.line("fg");                              // already running
  EXPECT_TRUE(sh.expect("[1]  + running    sleep 30")) << sh.out;
  // Ctrl-C ends the job; $? is 130.
  std::this_thread::sleep_for(200ms);
  sh.send("\x03");
  sh.line("echo status=$?");
  EXPECT_TRUE(sh.expect("status=130")) << sh.out;
}

TEST(Interactive, CtrlCEndsTheCommandLineButNotTheShell) {
  // B23: Ctrl-C used to kill jfsh along with the foreground command.
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30; echo not-reached");
  std::this_thread::sleep_for(300ms);
  sh.send("\x03");
  sh.line("echo alive");
  ASSERT_TRUE(sh.expect("alive\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("not-reached\r\n"), std::string::npos) << sh.out;
  // A loop of builtins runs in the shell itself; Ctrl-C stops it too.
  sh.line("while true; do :; done");
  std::this_thread::sleep_for(300ms);
  sh.send("\x03");
  sh.line("echo loop-over");
  EXPECT_TRUE(sh.expect("loop-over\r\n")) << sh.out;
}

TEST(Interactive, CtrlCAtThePromptDropsTheLine) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.send("echo half");
  sh.send("\x03");
  sh.line("if true; then");
  ASSERT_TRUE(sh.expect("> ")) << sh.out;
  sh.send("\x03");
  sh.line("echo whole");
  EXPECT_TRUE(sh.expect("whole\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("half\r\n"), std::string::npos) << sh.out;
}

TEST(Interactive, BackgroundJobsAreReportedAtThePrompt) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 0.2 &");
  EXPECT_TRUE(sh.expect("[1] ")) << sh.out;
  // NOTIFY: reported while the prompt waits, without pressing a key.
  EXPECT_TRUE(sh.expect("[1]  + done       sleep 0.2")) << sh.out;
  sh.line("false &");
  EXPECT_TRUE(sh.expect("[1]  + exit 1     false")) << sh.out;
  sh.line("sleep 30 &");
  sh.line("kill %1");
  EXPECT_TRUE(sh.expect("[1]  + terminated  sleep 30")) << sh.out;
  sh.line("sleep 30 &");
  sh.line("kill -STOP %1");
  EXPECT_TRUE(sh.expect("[1]  + suspended (signal)  sleep 30")) << sh.out;
  sh.line("kill %1");
  EXPECT_TRUE(sh.expect("[1]  + terminated  sleep 30")) << sh.out;
}

TEST(Interactive, PipelinesAreOneJob) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30 | cat");
  ASSERT_TRUE(sh.key_until("\x1a", "jfsh: suspended  sleep 30 | cat")) << sh.out;
  sh.line("jobs -l");
  EXPECT_TRUE(sh.expect(" suspended  sleep 30 | \r\n")) << sh.out;
  EXPECT_TRUE(sh.expect(" suspended  cat")) << sh.out;
  sh.line("kill %1");
  EXPECT_TRUE(sh.expect("[1]  + terminated  sleep 30 | cat")) << sh.out;
}

TEST(Interactive, SuspendingACommandInALoopGivesThePromptBack) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30; echo after-z");
  ASSERT_TRUE(sh.key_until("\x1a", "jfsh: suspended  sleep 30")) << sh.out;
  EXPECT_TRUE(sh.expect("after-z\r\n")) << sh.out;   // a list goes on, as in zsh
  sh.line("while true; do sleep 30; echo iter; done");
  ASSERT_TRUE(sh.key_until("\x1a", "jfsh: suspended  sleep 30")) << sh.out;
  sh.line("echo prompt-is-back");
  EXPECT_TRUE(sh.expect("prompt-is-back\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("iter\r\n"), std::string::npos) << sh.out;
  sh.line("kill %1 %2");
}

TEST(Interactive, TrapsRunAtThePrompt) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'echo got-usr1' USR1");
  ASSERT_TRUE(sh.expect("% "));
  std::this_thread::sleep_for(200ms);
  ::kill(sh.pid, SIGUSR1);
  EXPECT_TRUE(sh.expect("got-usr1")) << sh.out;
}

TEST(Interactive, ExitWarnsAboutSuspendedJobsThenHangsThemUp) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30");
  ASSERT_TRUE(sh.key_until("\x1a", "jfsh: suspended  sleep 30")) << sh.out;
  sh.line("exit");
  EXPECT_TRUE(sh.expect("jfsh: you have suspended jobs.")) << sh.out;
  sh.line("exit");
  EXPECT_TRUE(sh.expect("jfsh: warning: 1 jobs SIGHUPed")) << sh.out;
  int status = 0;
  EXPECT_TRUE(sh.exits(status)) << sh.out;
}

TEST(Interactive, ExitAfterJobsGoesAhead) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30 &");
  sh.line("jobs");
  EXPECT_TRUE(sh.expect("[1]  + running    sleep 30")) << sh.out;
  sh.line("exit");
  EXPECT_TRUE(sh.expect("jfsh: warning: 1 jobs SIGHUPed")) << sh.out;
  int status = 0;
  EXPECT_TRUE(sh.exits(status)) << sh.out;
}

TEST(Interactive, ExitTrapRunsWhenTheShellExits) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'echo bye-now' EXIT");
  sh.line("exit 3");
  EXPECT_TRUE(sh.expect("bye-now")) << sh.out;
  int status = 0;
  ASSERT_TRUE(sh.exits(status)) << sh.out;
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 3);
}

}  // namespace

// ---- M8: the interactive loop ---------------------------------------------------

namespace {

// A $ZDOTDIR holding a .jfshrc, removed afterwards.
struct RcDir {
  std::string path;
  explicit RcDir(std::string_view rc) {
    char tmpl[] = "/tmp/jfsh_rc_XXXXXX";
    path = ::mkdtemp(tmpl);
    std::string file = path + "/.jfshrc";
    if (std::FILE* f = std::fopen(file.c_str(), "w")) {
      std::fwrite(rc.data(), 1, rc.size(), f);
      std::fclose(f);
    }
  }
  ~RcDir() {
    std::string file = path + "/.jfshrc";
    ::unlink(file.c_str());
    ::rmdir(path.c_str());
  }
};

}  // namespace

TEST(Interactive, JfshrcHooksAndPromptReload) {
  RcDir rc("precmd() { print -n '<pre>' }\n"
           "preexec() { print -r -- \"<exec $1>\" }\n");
  Pty sh(rc.path);
  // precmd's output doesn't end in a newline, so PROMPT_SP marks it with
  // an inverse `%` and the prompt starts on the next line, as in zsh.
  ASSERT_TRUE(sh.expect("<pre>\x1b[1m\x1b[7m%")) << sh.out;
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  sh.line("echo hi");
  EXPECT_TRUE(sh.expect("<exec echo hi>")) << sh.out;
  EXPECT_TRUE(sh.expect("hi")) << sh.out;
  EXPECT_TRUE(sh.expect("<pre>")) << sh.out;
  EXPECT_TRUE(sh.expect("% ")) << sh.out;
  // A new $PS1 takes effect at the next prompt (B38).
  sh.line("PS1='new> '");
  EXPECT_TRUE(sh.expect("<pre>")) << sh.out;
  EXPECT_TRUE(sh.expect("new> ")) << sh.out;
  sh.line("exit");
  int st;
  EXPECT_TRUE(sh.exits(st)) << sh.out;
}

TEST(Interactive, LineIsInHistoryWhileItRuns) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo one");
  ASSERT_TRUE(sh.expect("one"));
  sh.line("echo $HISTCMD");
  EXPECT_TRUE(sh.expect("2\r\n")) << sh.out;
  sh.line("r");
  EXPECT_TRUE(sh.expect("echo $HISTCMD\r\n3")) << sh.out;   // r reran line 2 as event 3
  sh.line("fc -l");
  EXPECT_TRUE(sh.expect("    1  echo one\r\n    2  echo $HISTCMD\r\n    3  echo $HISTCMD\r\n"))
      << sh.out;
}

TEST(Interactive, SchedRunsBeforeThePrompt) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sched +0 echo fired");
  EXPECT_TRUE(sh.expect("fired")) << sh.out;
  EXPECT_TRUE(sh.expect("% ")) << sh.out;
}

// ---- M9: the line editor in the real shell ------------------------------------------

TEST(Interactive, TabCompletionAndHistoryKeys) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.send("ech\t");
  ASSERT_TRUE(sh.expect("echo ")) << sh.out;
  sh.line("first-word");
  EXPECT_TRUE(sh.expect("first-word\r\n")) << sh.out;
  // Ctrl-R finds it again; Enter runs it.
  sh.send("\x12" "first");
  EXPECT_TRUE(sh.expect("bck-i-search: first_")) << sh.out;
  sh.send("\r");
  EXPECT_TRUE(sh.expect("first-word\r\n")) << sh.out;
  // Up arrow, then edit.
  sh.send("\x1b[A\x17" "again\r");
  EXPECT_TRUE(sh.expect("again\r\n")) << sh.out;
}

TEST(Interactive, BangHistoryIsShownThenRun) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo one two");
  ASSERT_TRUE(sh.expect("one two\r\n"));
  sh.line("echo !!:2 !$");
  EXPECT_TRUE(sh.expect("echo two two\r\ntwo two\r\n")) << sh.out;
  sh.line("echo !nosuch");
  EXPECT_TRUE(sh.expect("jfsh: event not found: nosuch")) << sh.out;
  // HIST_VERIFY: the expansion comes back to be edited.
  sh.line("setopt histverify");
  sh.line("echo !!:0x");
  EXPECT_TRUE(sh.expect("% echo setoptx")) << sh.out;
  sh.send("\r");
  EXPECT_TRUE(sh.expect("setoptx\r\n")) << sh.out;
}

TEST(Interactive, RcBindingsAndWidgets) {
  RcDir rc("bindkey -s '^Xh' 'echo from-macro\\n'\n"
           "up() { BUFFER=${(U)BUFFER} }\nzle -N up\nbindkey '^Xu' up\n");
  Pty sh(rc.path);
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  sh.send("\x18h");
  EXPECT_TRUE(sh.expect("from-macro\r\n")) << sh.out;
  sh.send("echo shout\x18u\r");
  EXPECT_TRUE(sh.expect("SHOUT\r\n")) << sh.out;
}

