// Interactive job control (M6) end to end: the jfsh binary runs in a
// pseudo-terminal and is driven with keystrokes, as a user would.  The
// expected lines are what zsh 5.9 prints for the same keystrokes (`zsh -f
// -i`), with "jfsh:" for "zsh:".

#include <cerrno>
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

  Pty() {
    pid = ::forkpty(&fd, nullptr, nullptr, nullptr);
    if (pid == 0) {
      ::setenv("PS1", "%% ", 1);
      ::setenv("TERM", "dumb", 1);
      ::unsetenv("HISTFILE");
      ::execl(JFSH_BINARY, JFSH_BINARY, "--norouter", "-i", static_cast<char*>(nullptr));
      _exit(127);
    }
  }
  ~Pty() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int st;
      ::waitpid(pid, &st, 0);
    }
    if (fd >= 0) ::close(fd);
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
