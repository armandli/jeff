// Interactive job control (M6) end to end: the jfsh binary runs in a
// pseudo-terminal and is driven with keystrokes, as a user would.  The
// expected lines are what zsh 5.9 prints for the same keystrokes (`zsh -f
// -i`), with "jfsh:" for "zsh:".

#include <cerrno>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  // `flag` is one more option for the shell (`-l`, `-f`).
  explicit Pty(const std::string& zdotdir = "/nonexistent/jfsh-test-zdotdir",
               const char* flag = nullptr) {
    pid = ::forkpty(&fd, nullptr, nullptr, nullptr);
    if (pid == 0) {
      ::setenv("PS1", "%% ", 1);
      ::setenv("TERM", "dumb", 1);
      ::setenv("ZDOTDIR", zdotdir.c_str(), 1);
      ::unsetenv("HISTFILE");
      // vi editing would follow an $EDITOR that mentions vi.
      ::unsetenv("EDITOR");
      ::unsetenv("VISUAL");
      ::execl(JFSH_BINARY, JFSH_BINARY, "--norouter", "-i", flag, static_cast<char*>(nullptr));
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
  bool key_until(std::string_view key, std::string_view needle, int tries = 20,
                 std::chrono::milliseconds wait = 300ms) {
    for (int i = 0; i < tries; ++i) {
      send(key);
      if (expect(needle, wait)) return true;
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

// The last stage runs in the shell; a command it runs joins the
// pipeline's process group, so `cat` keeps reading the terminal.  A
// background pipeline lists every pid.
TEST(Interactive, LastPipelineStageRunsInTheShell) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo x | read v; echo v=$v");
  EXPECT_TRUE(sh.expect("v=x\r\n")) << sh.out;
  sh.line("cat | while read l; do /bin/echo got $l; done");
  sh.send("hi\n");
  EXPECT_TRUE(sh.expect("got hi\r\n")) << sh.out;
  sh.send("there\n");
  EXPECT_TRUE(sh.expect("got there\r\n")) << sh.out;
  sh.send("\x04");
  sh.line("echo after");
  EXPECT_TRUE(sh.expect("after\r\n")) << sh.out;
  sh.line("sleep 30 | sleep 30 &");
  EXPECT_TRUE(sh.expect("[1] ")) << sh.out;
  sh.line("jobs -l");
  EXPECT_TRUE(sh.expect(" running    sleep 30 | \r\n")) << sh.out;
  EXPECT_TRUE(sh.expect(" running    sleep 30\r\n")) << sh.out;
  sh.line("kill %1");
  EXPECT_TRUE(sh.expect("[1]  + terminated  sleep 30 | sleep 30")) << sh.out;
}

// Ctrl-Z can't suspend a pipeline whose last stage the shell runs: as
// zsh, it says so and continues the stopped stage.
TEST(Interactive, PipelineWithTheLastStageInTheShellCantBeSuspended) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 1 | while read l; do :; done; echo after");
  ASSERT_TRUE(sh.key_until("\x1a", "jfsh: job can't be suspended")) << sh.out;
  EXPECT_TRUE(sh.expect("after\r\n")) << sh.out;
}

// A background pipeline whose stages differ in state is listed a line a
// stage, as zsh's printjob does.
TEST(Interactive, JobsListsStagesInDifferentStates) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 0.1 | sleep 30 &");
  ASSERT_TRUE(sh.expect("[1] ")) << sh.out;
  std::this_thread::sleep_for(500ms);
  sh.line("jobs");
  EXPECT_TRUE(sh.expect("[1]  + done       sleep 0.1 | \r\n"
                        "       running    sleep 30\r\n")) << sh.out;
  sh.line("kill %1");
  EXPECT_TRUE(sh.expect("[1]  + done        sleep 0.1 | \r\n"
                        "       terminated  sleep 30")) << sh.out;
}

// Ctrl-C ends an open of a fifo nobody writes to, and a `read` (status
// 130), and the prompt comes back.
TEST(Interactive, CtrlCEndsFifoOpensAndReads) {
  char tmpl[] = "/tmp/jfsh_fifo_XXXXXX";
  std::string dir = ::mkdtemp(tmpl);
  std::string fifo = dir + "/ff";
  ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("exec 3<" + fifo);
  std::this_thread::sleep_for(300ms);
  sh.send("\x03");
  sh.line("echo after1 $?");
  EXPECT_TRUE(sh.expect("after1 1\r\n")) << sh.out;
  sh.line("read x < " + fifo);
  std::this_thread::sleep_for(300ms);
  sh.send("\x03");
  sh.line("echo after2 $?");
  EXPECT_TRUE(sh.expect("after2 1\r\n")) << sh.out;
  sh.line("read x < <(sleep 100)");
  std::this_thread::sleep_for(300ms);
  sh.send("\x03");
  sh.line("echo after3 $?");
  EXPECT_TRUE(sh.expect("after3 130\r\n")) << sh.out;
  ::unlink(fifo.c_str());
  ::rmdir(dir.c_str());
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
  // zsh 5.9: the `exit` that only warned has status 0, and a stopped job
  // is left to the kernel's SIGHUP of an orphaned group, without a word.
  sh.line("echo st $?");
  EXPECT_TRUE(sh.expect("st 0")) << sh.out;
  // Another command in between: the next `exit` warns again.
  sh.line("exit");
  EXPECT_TRUE(sh.expect("jfsh: you have suspended jobs.")) << sh.out;
  sh.line("exit");
  int status = -1;
  EXPECT_TRUE(sh.exits(status)) << sh.out;
  EXPECT_EQ(sh.out.find("SIGHUPed"), std::string::npos) << sh.out;
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
  // As zsh 5.9: `r` stays event 3 and reruns line 2 as event 4.
  EXPECT_TRUE(sh.expect("echo $HISTCMD\r\n4")) << sh.out;
  sh.line("fc -l");
  EXPECT_TRUE(sh.expect("    1  echo one\r\n    2  echo $HISTCMD\r\n    3  r\r\n"
                        "    4  echo $HISTCMD\r\n"))
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


// ---- M6 review: zsh 5.9's behaviour through a terminal ----------------------------

namespace {

// A trapped Ctrl-C that kills the foreground job runs the trap, and the
// command line goes on (zsh's update_job).
TEST(InteractiveM6Review, TrappedCtrlCRunsTheTrapAndTheLineGoesOn) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'echo got-$((1+1))' INT");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  // Wait for the line to be read: a Ctrl-C while it is typed ahead drops it.
  sh.line("for i in 1 2; do sleep 30; echo i-$((i*10)); done; echo st-$?");
  ASSERT_TRUE(sh.expect("st-$?")) << sh.out;
  std::this_thread::sleep_for(300ms);
  // A loaded machine can take a while to run the trap: wait longer per key.
  ASSERT_TRUE(sh.key_until("\x03", "got-2\r\ni-10\r\n", 10, 1500ms)) << sh.out;
  std::this_thread::sleep_for(300ms);
  ASSERT_TRUE(sh.key_until("\x03", "got-2\r\ni-20\r\nst-0\r\n", 10, 1500ms)) << sh.out;
}

// A TRAPINT that returns non-zero ends the line, with its status.
TEST(InteractiveM6Review, TrapIntReturningNonZeroEndsTheLine) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("TRAPINT() { echo got-$((1+1)); return 1 }");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  sh.line("sleep 30; echo not-$((1+1))");
  ASSERT_TRUE(sh.expect("not-$((1+1))")) << sh.out;
  std::this_thread::sleep_for(300ms);
  ASSERT_TRUE(sh.key_until("\x03", "got-2\r\n")) << sh.out;
  sh.line("echo st-$?");
  EXPECT_TRUE(sh.expect("st-1\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("not-2"), std::string::npos) << sh.out;
}

// A pipeline a signal killed part of is reported a process a line.
TEST(InteractiveM6Review, KilledPipelineIsReportedPerProcess) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sh -c 'kill -9 $$' | cat; echo st-$?");
  EXPECT_TRUE(sh.expect("jfsh: killed     sh -c 'kill -9 $$' | \r\n"
                        "jfsh: done       cat\r\n"
                        "st-0\r\n")) << sh.out;
}

// What a command sets with stty lasts, but echo comes back.
TEST(InteractiveM6Review, SttySettingsLastButEchoComesBack) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("stty intr ^X -echo");
  sh.line("stty -a | grep -o 'intr = ^.'; stty -a | grep -c -- ' -echo '");
  EXPECT_TRUE(sh.expect("intr = ^X\r\n0\r\n")) << sh.out;
  sh.line("stty sane");
}

// `fg %-` shows the job's mark before it became current.
TEST(InteractiveM6Review, FgShowsTheMarkTheJobHad) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30 &");
  sh.line("sleep 31 &");
  ASSERT_TRUE(sh.expect("[2] ")) << sh.out;
  sh.line("fg %-");
  EXPECT_TRUE(sh.expect("[1]  - running    sleep 30\r\n")) << sh.out;
  std::this_thread::sleep_for(200ms);
  ASSERT_TRUE(sh.key_until("\x1a", "jfsh: suspended  sleep 30")) << sh.out;
  sh.line("kill -9 %1 %2");
  EXPECT_TRUE(sh.expect("killed")) << sh.out;
}

// Signals ignored when an interactive shell starts are not passed on,
// SIGHUP (nohup) included.
TEST(InteractiveM6Review, SignalsIgnoredAtStartAreNotPassedOn) {
  struct sigaction ign{}, old_usr1{}, old_hup{};
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  ::sigaction(SIGUSR1, &ign, &old_usr1);
  ::sigaction(SIGHUP, &ign, &old_hup);
  Pty sh;
  ::sigaction(SIGUSR1, &old_usr1, nullptr);
  ::sigaction(SIGHUP, &old_hup, nullptr);
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sh -c 'kill -USR1 $$; echo alive'; echo st-$?");
  EXPECT_TRUE(sh.expect("st-" + std::to_string(128 + SIGUSR1) + "\r\n")) << sh.out;
  sh.line("sh -c 'kill -HUP $$; echo alive'; echo st-$?");
  EXPECT_TRUE(sh.expect("st-" + std::to_string(128 + SIGHUP) + "\r\n")) << sh.out;
  sh.line("[[ -o hup ]]; echo hup-$?");
  EXPECT_TRUE(sh.expect("hup-0\r\n")) << sh.out;
}

// A SIGCHLD ignored at start keeps the shell's handler: jobs are reported.
TEST(InteractiveM6Review, SigchldIgnoredAtStartStillReportsJobs) {
  struct sigaction ign{}, old{};
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  ::sigaction(SIGCHLD, &ign, &old);
  Pty sh;
  ::sigaction(SIGCHLD, &old, nullptr);
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 0.2 &");
  ASSERT_TRUE(sh.expect("[1] ")) << sh.out;
  std::this_thread::sleep_for(500ms);
  sh.line("echo x-$((1+1))");
  EXPECT_TRUE(sh.expect("[1]  + done       sleep 0.2")) << sh.out;
  EXPECT_TRUE(sh.expect("x-2\r\n")) << sh.out;
}

// Only Ctrl-C owes a newline before the prompt (the terminal echoed ^C
// without one); Ctrl-\ and a TRAPINT that ends the line don't.
TEST(InteractiveM6Review, NoBlankLineAfterQuitOrATrapThatEndsTheLine) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 30; echo next");
  ASSERT_TRUE(sh.expect("echo next")) << sh.out;
  std::this_thread::sleep_for(300ms);
  ASSERT_TRUE(sh.key_until("\x1c", "quit       sleep 30\r\n")) << sh.out;
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  EXPECT_EQ(sh.out.find("sleep 30\r\n\r\n"), std::string::npos) << sh.out;
  sh.line("TRAPINT() { echo t-$((1+1)); return 3 }");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  sh.line("sleep 31");
  ASSERT_TRUE(sh.expect("sleep 31")) << sh.out;
  std::this_thread::sleep_for(300ms);
  ASSERT_TRUE(sh.key_until("\x03", "t-2\r\n")) << sh.out;
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  EXPECT_EQ(sh.out.find("t-2\r\n\r\n"), std::string::npos) << sh.out;
}

}  // namespace

namespace {

// `trap - INT` and `trap - HUP` inside their own trap: SIGINT then ends the
// trap and the line ($? 1), SIGHUP ends the shell after the trap, with 1.
// Neither kills the shell outright.
TEST(InteractiveM6Review, AResetIntOrHupInItsOwnTrapIsTheShellsOwn) {
  {
    Pty sh;
    ASSERT_TRUE(sh.expect("% "));
    sh.line("trap 'echo t-$((1+1)); trap - INT; kill -INT $$; echo after-$((1+1))' INT");
    ASSERT_TRUE(sh.expect("% ")) << sh.out;
    sh.line("kill -INT $$; echo next-$((1+1))");
    EXPECT_TRUE(sh.expect("t-2\r\n")) << sh.out;
    sh.line("echo alive-$?");
    EXPECT_TRUE(sh.expect("alive-1\r\n")) << sh.out;
    EXPECT_EQ(sh.out.find("after-2"), std::string::npos) << sh.out;
    EXPECT_EQ(sh.out.find("next-2"), std::string::npos) << sh.out;
  }
  {
    Pty sh;
    ASSERT_TRUE(sh.expect("% "));
    sh.line("trap 'echo t-$((1+1)); trap - HUP; kill -HUP $$; echo after-$((1+1))' HUP");
    ASSERT_TRUE(sh.expect("% ")) << sh.out;
    sh.line("kill -HUP $$; echo next-$((1+1))");
    EXPECT_TRUE(sh.expect("after-2\r\n")) << sh.out;
    int status = -1;
    ASSERT_TRUE(sh.exits(status)) << sh.out;
    ASSERT_TRUE(WIFEXITED(status)) << sh.out;
    EXPECT_EQ(WEXITSTATUS(status), 1);
  }
}

// Ctrl-C killing a job inside the INT trap: the trap goes on (zsh's
// ZSIG_IGNORED), with no extra newline.
TEST(InteractiveM6Review, CtrlCInsideTheIntTrapLetsItFinish) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'echo int-$((1+1)); sleep 30; echo done-$((1+1))' INT");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  sh.line("sleep 31");
  ASSERT_TRUE(sh.expect("sleep 31")) << sh.out;
  std::this_thread::sleep_for(300ms);
  ASSERT_TRUE(sh.key_until("\x03", "int-2\r\n")) << sh.out;
  std::this_thread::sleep_for(300ms);
  ASSERT_TRUE(sh.key_until("\x03", "^Cdone-2\r\n")) << sh.out;
}

}  // namespace

namespace {

// An untrapped `kill -INT $$` ends the line with 130 and no blank line.
TEST(InteractiveM6Review, KillIntToTheShellGives130) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("kill -INT $$; echo no-$((1+1))");
  sh.line("echo a-$?");
  EXPECT_TRUE(sh.expect("a-130\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("no-2"), std::string::npos) << sh.out;
  EXPECT_EQ(sh.out.find("$((1+1))\r\n\r\n"), std::string::npos) << sh.out;
}

// A trap that fails at the prompt ends its line with a newline of its own,
// then the prompt comes again (zsh's trashzle and errflag reset).
TEST(InteractiveM6Review, AFailedTrapAtThePromptEndsItsLine) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("TRAPUSR1() { echo trapped; return 3 }");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  ::kill(sh.pid, SIGUSR1);
  EXPECT_TRUE(sh.expect("trapped\r\n\r\n")) << sh.out;
  EXPECT_TRUE(sh.expect("% ")) << sh.out;
  sh.line("echo st-$?");
  EXPECT_TRUE(sh.expect("st-")) << sh.out;
}

// A trap's `return` at the prompt ends there, with a newline of its own:
// the next line runs, and $? is the trap's status (zsh 5.9).
TEST(InteractiveM6Review, ATrapReturnAtThePromptEndsThere) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'echo trapped; return 3' USR1");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  ::kill(sh.pid, SIGUSR1);
  EXPECT_TRUE(sh.expect("trapped\r\n\r\n")) << sh.out;
  sh.line("echo one-$?");
  EXPECT_TRUE(sh.expect("one-3\r\n")) << sh.out;
  sh.line("echo two-$?");
  EXPECT_TRUE(sh.expect("two-0\r\n")) << sh.out;
}

// A trap's `return` in a typed line ends the line with the status of the
// builtin it ran in (zsh 5.9: `kill`'s 0).
TEST(InteractiveM6Review, ATrapReturnInALineKeepsTheBuiltinsStatus) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'echo in; return 4' USR1");
  sh.line("kill -USR1 $$; echo no-$((1+1))");
  sh.line("echo st-$?");
  EXPECT_TRUE(sh.expect("st-0\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("no-2"), std::string::npos) << sh.out;
}

// SIGHUP while a foreground job runs: the shell goes at once, SIGHUPs the
// job and says so, with status 1.
TEST(InteractiveM6Review, HangupDuringAForegroundJobExitsAtOnce) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("sleep 31");
  ASSERT_TRUE(sh.expect("sleep 31")) << sh.out;
  std::this_thread::sleep_for(300ms);
  ::kill(sh.pid, SIGHUP);
  int status = -1;
  ASSERT_TRUE(sh.exits(status, 3000ms)) << sh.out;
  EXPECT_NE(sh.out.find("jfsh: warning: 1 jobs SIGHUPed"), std::string::npos) << sh.out;
  ASSERT_TRUE(WIFEXITED(status)) << sh.out;
  EXPECT_EQ(WEXITSTATUS(status), 1);
}

}  // namespace

namespace {

// read -k and -q read the terminal a character at a time (zsh's
// setcbreak), still echoing what is typed.
TEST(InteractiveM7Review, ReadKAndQEchoWhatIsTyped) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("read -k 2 x");
  ASSERT_TRUE(sh.expect("read -k 2 x")) << sh.out;
  std::this_thread::sleep_for(300ms);
  sh.send("ab");
  // Echoed by the terminal before anything else is sent.
  EXPECT_TRUE(sh.expect("ab")) << sh.out;
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  sh.line("read -q 'y?ok? '");
  ASSERT_TRUE(sh.expect("\nok?")) << sh.out;
  std::this_thread::sleep_for(300ms);
  sh.send("n");
  EXPECT_TRUE(sh.expect(" n")) << sh.out;
  sh.line("echo x-$x y-$y");
  EXPECT_TRUE(sh.expect("x-ab y-n\r\n")) << sh.out;
}

// fc's messages as zsh's, which name fc for history and r too.
TEST(InteractiveM7Review, FcMessagesAndMatching) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo one");
  ASSERT_TRUE(sh.expect("one\r\n")) << sh.out;
  sh.line("echo two");
  ASSERT_TRUE(sh.expect("two\r\n")) << sh.out;
  sh.line("fc -l 100");
  EXPECT_TRUE(sh.expect("fc: no such event: 100\r\n")) << sh.out;
  sh.line("r nosuch");
  EXPECT_TRUE(sh.expect("fc: event not found: nosuch\r\n")) << sh.out;
  sh.line("fc -l 1 2 3");
  EXPECT_TRUE(sh.expect("fc: too many arguments\r\n")) << sh.out;
  sh.line("fc -lm 'echo t*'");
  EXPECT_TRUE(sh.expect("    2  echo two\r\n")) << sh.out;
  // -m matching nothing, listed or run (zsh's fclist).
  sh.line("fc -l -m 'zz*' 1; echo rc-$?");
  EXPECT_TRUE(sh.expect("fc: no matching events found\r\nrc-1\r\n")) << sh.out;
  sh.line("fc -e - -m 'zz*' 1; echo rc-$?");
  EXPECT_TRUE(sh.expect("fc: no matching events found\r\nrc-1\r\n")) << sh.out;
}

// pushd alone with an empty stack goes home and prints the stack, as
// zsh's cd_get_dest; chpwd runs for it.
TEST(InteractiveM7Review, PushdAloneWithAnEmptyStackGoesHome) {
  char tmpl[] = "/tmp/jfsh_m7_XXXXXX";
  ASSERT_NE(::mkdtemp(tmpl), nullptr);
  std::string d = tmpl;
  {
    Pty sh;
    ASSERT_TRUE(sh.expect("% "));
    sh.line("mkdir " + d + "/h; HOME=" + d + "/h; cd " + d);
    ASSERT_TRUE(sh.expect("% ")) << sh.out;
    sh.line("chpwd() { echo hook-${PWD##*/} }; pushd; echo st-$?");
    EXPECT_TRUE(sh.expect("~ ")) << sh.out;
    EXPECT_TRUE(sh.expect("hook-h\r\nst-0\r\n")) << sh.out;
  }
  std::error_code ec;
  std::filesystem::remove_all(d, ec);
}

// ---- M8 review: zsh 5.9's output for the same keystrokes -------------------------

// `r` stays in the history and what it reruns comes after it, run at once
// (zsh's fc: stuff() and loop()); a first `history` lists itself.
TEST(InteractiveM8Review, RerunKeepsItsLineAndHistoryListsItself) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("history");
  EXPECT_TRUE(sh.expect("    1  history\r\n")) << sh.out;
  sh.line("echo one");
  ASSERT_TRUE(sh.expect("one\r\n")) << sh.out;
  sh.line("r; echo after");
  EXPECT_TRUE(sh.expect("echo one\r\none\r\nafter\r\n")) << sh.out;
  sh.line("fc -l");
  EXPECT_TRUE(sh.expect("    1  history\r\n    2  echo one\r\n    3  r; echo after\r\n"
                        "    4  echo one\r\n")) << sh.out;
}

// preexec's $2 and $3 are the parsed line, on one line and in full.
TEST(InteractiveM8Review, PreexecGetsTheParsedLine) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("preexec() { print -r -- \"1[$1]\"; print -r -- \"2[$2]\"; print -r -- \"3[$3]\" }");
  sh.line("for i in 1; do echo $i; done");
  EXPECT_TRUE(sh.expect("1[for i in 1; do echo $i; done]\r\n2[for i in 1; do; echo $i; done]\r\n"
                        "3[for i in 1\r\ndo\r\n\techo $i\r\ndone]\r\n1\r\n")) << sh.out;
}

// $LINENO counts the lines read; a function counts from its definition.
TEST(InteractiveM8Review, LinenoAndVerbose) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo L$LINENO");
  ASSERT_TRUE(sh.expect("L1\r\n")) << sh.out;
  sh.line("echo L$LINENO");
  EXPECT_TRUE(sh.expect("L2\r\n")) << sh.out;
  sh.line("f() { echo F$LINENO; }; f");
  EXPECT_TRUE(sh.expect("F0\r\n")) << sh.out;
  sh.line("set -v");
  sh.line("echo V$((1))");
  EXPECT_TRUE(sh.expect("echo V$((1))\r\nV1\r\n")) << sh.out;
}

// IGNORE_EOF: each Ctrl-D says how to leave; the tenth leaves.
TEST(InteractiveM8Review, IgnoreEofAndLogout) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("logout");
  EXPECT_TRUE(sh.expect("logout: not login shell\r\n")) << sh.out;
  sh.line("setopt ignoreeof");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  for (int i = 0; i < 9; ++i) {
    std::this_thread::sleep_for(100ms);
    sh.send("\x04");
    ASSERT_TRUE(sh.expect("jfsh: use 'exit' to exit.\r\n")) << i << sh.out;
  }
  int st = 0;
  EXPECT_FALSE(sh.exits(st, 300ms)) << sh.out;
  sh.send("\x04");
  ASSERT_TRUE(sh.exits(st)) << sh.out;
  EXPECT_TRUE(WIFEXITED(st) and WEXITSTATUS(st) == 0) << st;
}

// A login shell's logout file sees $? as it was before `exit`, and an
// `exit` in it sets the shell's status (zsh's zexit).
TEST(InteractiveM8Review, LogoutFileStatus) {
  char tmpl[] = "/tmp/jfsh_m8_XXXXXX";
  ASSERT_NE(::mkdtemp(tmpl), nullptr);
  std::string d = tmpl;
  std::ofstream(d + "/.jfshenv") << ":\n";
  std::ofstream(d + "/.jfsh_logout") << "echo LO $?; exit 7\n";
  {
    Pty sh(d, "-l");
    ASSERT_TRUE(sh.expect("% "));
    sh.line("exit 4");
    EXPECT_TRUE(sh.expect("LO 0\r\n")) << sh.out;
    int st = 0;
    ASSERT_TRUE(sh.exits(st)) << sh.out;
    EXPECT_TRUE(WIFEXITED(st) and WEXITSTATUS(st) == 7) << st;
  }
  std::filesystem::remove_all(d);
}

// Without RCS (-f) the history file is neither read nor written.
TEST(InteractiveM8Review, NoHistoryFileWithoutRcs) {
  char tmpl[] = "/tmp/jfsh_m8_XXXXXX";
  ASSERT_NE(::mkdtemp(tmpl), nullptr);
  std::string d = tmpl;
  {
    Pty sh(d, "-f");
    ASSERT_TRUE(sh.expect("% "));
    sh.line("HISTFILE=" + d + "/h; SAVEHIST=10");
    sh.line("echo saved");
    ASSERT_TRUE(sh.expect("saved\r\n")) << sh.out;
    sh.line("exit");
    int st = 0;
    ASSERT_TRUE(sh.exits(st)) << sh.out;
  }
  EXPECT_FALSE(std::filesystem::exists(d + "/h"));
  std::filesystem::remove_all(d);
}

// preexec's $1 is the line read, also when the history drops it.
TEST(InteractiveM8Review, PreexecGetsTheLineTheHistoryDrops) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("setopt histignoredups histignorespace; preexec() { print -r -- \"1[$1]\" }");
  sh.line("echo a");
  ASSERT_TRUE(sh.expect("1[echo a]\r\na\r\n")) << sh.out;
  sh.line("echo a");
  EXPECT_TRUE(sh.expect("1[echo a]\r\na\r\n")) << sh.out;
  sh.line(" echo sp");
  EXPECT_TRUE(sh.expect("1[ echo sp]\r\nsp\r\n")) << sh.out;
}

// `jfsh > file` on a terminal: the line editor draws on the terminal (zsh's
// SHTTY), and only the commands' output goes to the file.
TEST(InteractiveM8Review, EditorUsesTheTerminalWhenStdoutIsAFile) {
  char tmpl[] = "/tmp/jfsh_m8_XXXXXX";
  ASSERT_NE(::mkdtemp(tmpl), nullptr);
  std::string d = tmpl;
  std::string file = d + "/out";
  int fd = -1;
  pid_t pid = ::forkpty(&fd, nullptr, nullptr, nullptr);
  if (pid == 0) {
    int o = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(o, 1);
    ::close(o);
    ::setenv("PS1", "P> ", 1);
    ::setenv("TERM", "xterm", 1);
    ::setenv("ZDOTDIR", "/nonexistent/jfsh-test-zdotdir", 1);
    ::unsetenv("HISTFILE");
    ::execl(JFSH_BINARY, JFSH_BINARY, "--norouter", "-f", static_cast<char*>(nullptr));
    _exit(127);
  }
  std::string got;
  auto pump = [&](std::chrono::milliseconds t, std::string_view until) {
    auto end = std::chrono::steady_clock::now() + t;
    while (std::chrono::steady_clock::now() < end and got.find(until) == std::string::npos) {
      struct pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 20) <= 0) continue;
      char buf[4096];
      ssize_t n = ::read(fd, buf, sizeof buf);
      if (n <= 0) break;
      got.append(buf, static_cast<std::size_t>(n));
    }
  };
  pump(5000ms, "P> ");
  EXPECT_NE(got.find("P> "), std::string::npos) << got;
  ::write(fd, "echo hi\n", 8);
  pump(3000ms, "hi\r\n");
  ::write(fd, "exit\n", 5);
  int st = 0;
  bool reaped = false;
  for (int i = 0; i < 100 and not reaped; ++i) {
    pump(50ms, "\x01");
    reaped = ::waitpid(pid, &st, WNOHANG) == pid;
  }
  if (not reaped) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &st, 0);
  }
  ::close(fd);
  std::ifstream f(file);
  std::string written((std::istreambuf_iterator<char>(f)), {});
  EXPECT_EQ(written, "hi\n");
  std::filesystem::remove_all(d);
}

// The shell's terminal fd is its own (zsh's FDT_INTERNAL): a redirection
// can't use it and `{fd}>&-` won't close it.
TEST(InteractiveM8Review, TheShellsTerminalFdIsInternal) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo hi10 >&10; echo st=$?");
  EXPECT_TRUE(sh.expect("10: bad file descriptor\r\nst=1\r\n")) << sh.out;
  sh.line("fd=10; exec {fd}>&-; echo st=$?");
  EXPECT_TRUE(sh.expect("file descriptor 10 used by shell, not closed\r\nst=1\r\n")) << sh.out;
  sh.line("echo alive");
  EXPECT_TRUE(sh.expect("alive\r\n")) << sh.out;
}

// Ctrl-C at the prompt or at PS2 leaves $? 130; a trapped one doesn't.
TEST(InteractiveM8Review, CtrlCAtThePromptSetsStatus) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("true");
  ASSERT_TRUE(sh.expect("% "));
  std::this_thread::sleep_for(200ms);
  sh.send("abc");
  std::this_thread::sleep_for(200ms);
  sh.send("\x03");
  std::this_thread::sleep_for(200ms);
  sh.line("echo st=$?");
  EXPECT_TRUE(sh.expect("st=130\r\n")) << sh.out;
  sh.line("if true");
  ASSERT_TRUE(sh.expect("> ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("\x03");
  std::this_thread::sleep_for(200ms);
  sh.line("echo st3=$?");
  EXPECT_TRUE(sh.expect("st3=130\r\n")) << sh.out;
  // send-break (^G) ends the line with 1 unless $? already says so.
  sh.line("true");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("abc\x07");
  std::this_thread::sleep_for(200ms);
  sh.line("echo st4=$?");
  EXPECT_TRUE(sh.expect("st4=1\r\n")) << sh.out;
  sh.line("(exit 5)");
  std::this_thread::sleep_for(200ms);
  sh.send("abc\x07");
  std::this_thread::sleep_for(200ms);
  sh.line("echo st5=$?");
  EXPECT_TRUE(sh.expect("st5=5\r\n")) << sh.out;
}

// With INT ignored the interrupt key does nothing; trapped, its trap runs
// and the line goes on, unless a TRAPINT returns non-zero, which ends the
// line with that status (zsh's SIGINT handler and dotrap).
TEST(InteractiveM8Review, CtrlCAtThePromptRunsTheIntTrap) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap '' INT; true");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("echo a");
  sh.send("\x03");
  std::this_thread::sleep_for(300ms);
  sh.line("b; echo st=$?");
  EXPECT_TRUE(sh.expect("ab\r\nst=0\r\n")) << sh.out;
  sh.line("trap 'echo T' INT; false");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("\x03");
  EXPECT_TRUE(sh.expect("T\r\n")) << sh.out;
  std::this_thread::sleep_for(300ms);
  sh.line("echo st2=$?");
  EXPECT_TRUE(sh.expect("st2=1\r\n")) << sh.out;
  sh.line("trap - INT; TRAPINT() { echo TI $1; return 3 }");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("yz");
  sh.send("\x03");
  EXPECT_TRUE(sh.expect("TI 2")) << sh.out;
  std::this_thread::sleep_for(300ms);
  sh.line("echo st3=$?");
  EXPECT_TRUE(sh.expect("st3=3\r\n")) << sh.out;
}

// An `exit` in the INT trap at the prompt leaves at once, with nothing
// typed after the Ctrl-C.  As zsh 5.9's zexit from the signal handler, the
// history is saved and the EXIT trap runs, but a login shell reads no
// logout file and zshexit doesn't run (EX, status 3).
TEST(InteractiveM8Review, ExitInTheIntTrapAtThePromptLeavesAtOnce) {
  char tmpl[] = "/tmp/jfsh_m8_XXXXXX";
  ASSERT_NE(::mkdtemp(tmpl), nullptr);
  std::string d = tmpl;
  std::ofstream(d + "/.jfshrc") << "HISTFILE=" << d << "/hf; SAVEHIST=10\n";
  std::ofstream(d + "/.jfsh_logout") << "echo LOGOUT\n";
  {
    Pty sh(d, "-l");
    ASSERT_TRUE(sh.expect("% "));
    sh.line("zshexit() { echo Z$((6*7)) }; trap 'echo EX' EXIT; trap 'exit 3' INT");
    ASSERT_TRUE(sh.expect("% ")) << sh.out;
    std::this_thread::sleep_for(200ms);
    sh.send("abc");
    sh.send("\x03");
    EXPECT_TRUE(sh.expect("EX\r\n")) << sh.out;
    int st = 0;
    ASSERT_TRUE(sh.exits(st)) << sh.out;
    EXPECT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 3);
    EXPECT_EQ(sh.out.find("LOGOUT"), std::string::npos) << sh.out;
    EXPECT_EQ(sh.out.find("Z42"), std::string::npos) << sh.out;
  }
  std::ifstream hf(d + "/hf");
  std::string saved((std::istreambuf_iterator<char>(hf)), std::istreambuf_iterator<char>());
  EXPECT_NE(saved.find("trap 'exit 3' INT"), std::string::npos) << saved;
  std::filesystem::remove_all(d);
}

// Ctrl-C in vared goes through the INT trap as at the prompt (zsh 5.9):
// untrapped it ends the command line with $? 130; a trap that returns
// ends it with that status; a trap that doesn't break lets editing go on.
TEST(InteractiveM8Review, CtrlCInVaredRunsTheIntTrap) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  std::this_thread::sleep_for(200ms);
  sh.line("x=abc; vared x; echo st=$? x=$x");
  ASSERT_TRUE(sh.expect("abc")) << sh.out;
  std::this_thread::sleep_for(300ms);
  sh.send("d");
  sh.send("\x03");
  std::this_thread::sleep_for(300ms);
  sh.line("echo s1=$? x=$x");
  EXPECT_TRUE(sh.expect("s1=130 x=abc\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("st=1"), std::string::npos) << sh.out;   // the rest didn't run
  sh.line("trap 'echo T' INT; x=abc; vared x; echo st=$? x=$x");
  std::this_thread::sleep_for(300ms);
  sh.send("d");
  sh.send("\x03");
  EXPECT_TRUE(sh.expect("T")) << sh.out;
  std::this_thread::sleep_for(300ms);
  sh.send("\r");
  EXPECT_TRUE(sh.expect("st=0 x=abcd\r\n")) << sh.out;
  sh.line("trap - INT; TRAPINT() { return 6 }; vared x; echo st=$?");
  std::this_thread::sleep_for(300ms);
  sh.send("\x03");
  std::this_thread::sleep_for(300ms);
  sh.line("echo s2=$?");
  EXPECT_TRUE(sh.expect("s2=6\r\n")) << sh.out;
  // In a loop the trap's status stays; untrapped, the loop ends with 1,
  // as after an error (zsh's execfor and execwhile).
  std::this_thread::sleep_for(200ms);
  sh.line("for i in 1 2; do vared x; echo i=$i; done");
  std::this_thread::sleep_for(500ms);
  sh.send("\x03");
  std::this_thread::sleep_for(300ms);
  sh.line("echo s3=$?");
  EXPECT_TRUE(sh.expect("s3=6\r\n")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.line("unfunction TRAPINT; while true; do vared x; done");
  std::this_thread::sleep_for(500ms);
  sh.send("\x03");
  std::this_thread::sleep_for(300ms);
  sh.line("echo s4=$?");
  EXPECT_TRUE(sh.expect("s4=1\r\n")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.line("f() { for i in 1; do vared x; done }; f");
  std::this_thread::sleep_for(500ms);
  sh.send("\x03");
  std::this_thread::sleep_for(300ms);
  sh.line("echo s5=$?");
  EXPECT_TRUE(sh.expect("s5=1\r\n")) << sh.out;
}

// At end of input the EXIT trap sees the line after the last one read; and
// preexec's $1 is the line as HIST_REDUCE_BLANKS keeps it.
TEST(InteractiveM8Review, LinenoAtEofAndReducedBlanks) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("setopt histreduceblanks; preexec() { print -r -- \"1[$1]\" }");
  sh.line("echo \"a  b\"   c   ");
  EXPECT_TRUE(sh.expect("1[echo \"a  b\" c]\r\na  b c\r\n")) << sh.out;
  sh.line("trap 'echo T $LINENO' EXIT");
  sh.line(":");
  ASSERT_TRUE(sh.expect("1[:]\r\n")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("\x04");
  EXPECT_TRUE(sh.expect("T 5\r\n")) << sh.out;
  int st = 0;
  EXPECT_TRUE(sh.exits(st)) << sh.out;
}

}  // namespace

// ---- M9 review --------------------------------------------------------------
//
// Expected values from zsh -f -i in a pty (scratchpad m9/p/p_m8h.py and
// p_bang.py).

// zsh's history expansion leaves a here-document's body alone.
TEST(InteractiveM9Review, BangInAHereDocumentStays) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo a1");
  ASSERT_TRUE(sh.expect("a1\r\n")) << sh.out;
  sh.line("cat <<E");
  sh.line("x !! y");
  sh.line("E");
  EXPECT_TRUE(sh.expect("\r\nx !! y\r\n")) << sh.out;
}

// A trap run at the interrupt key writes where the cursor is, and an
// `exit` there leaves the screen as it is (zsh's handler doesn't redraw).
TEST(InteractiveM9Review, ExitInATrapAtThePromptLeavesTheScreen) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'echo TR; exit 3' INT; trap 'echo EX' EXIT");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("abc");
  ASSERT_TRUE(sh.expect("abc")) << sh.out;
  sh.send("\x03");
  EXPECT_TRUE(sh.expect("\r\x1b[5CTR\r\n\r\nEX\r\n")) << sh.out;
  int st = 0;
  ASSERT_TRUE(sh.exits(st)) << sh.out;
  EXPECT_EQ(WEXITSTATUS(st), 3);
}

// $TMOUT: SIGALRM when no line comes in time; its trap can exit.
TEST(InteractiveM9Review, TmoutSendsAlrmAtThePrompt) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("TMOUT=1; trap 'echo AL; exit 4' ALRM");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  EXPECT_TRUE(sh.expect("AL\r\n", 4000ms)) << sh.out;
  int st = 0;
  ASSERT_TRUE(sh.exits(st)) << sh.out;
  EXPECT_EQ(WEXITSTATUS(st), 4);
}

// which-command and run-help run the alias of their own name (zsh's
// processcmd), with the line pushed for the next prompt.
TEST(InteractiveM9Review, WhichCommandRunsItsName) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("alias which-command='echo WC'");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("ls -l");
  sh.send("\x1b?");
  EXPECT_TRUE(sh.expect("WC ls\r\n")) << sh.out;
  EXPECT_TRUE(sh.expect("% ls -l")) << sh.out;
}

// ---- M9 review, round 1 ------------------------------------------------------
//
// Expected values from zsh -f -i in a pty (scratchpad m9rv1/p/r1.py, r2.py,
// r8.py and m9rv1/p2_*.py).

// A subshell of an interactive shell takes SIGALRM's default action: the
// interactive shell's $TMOUT handler isn't its.
TEST(InteractiveM9Review, SubshellsDieOfAlrm) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("(sh -c 'kill -ALRM $PPID'; echo sub $?); echo after $?");
  EXPECT_TRUE(sh.expect("after 142\r\n")) << sh.out;
  EXPECT_EQ(sh.out.find("sub 0"), std::string::npos) << sh.out;
  sh.line("x=$(sh -c 'kill -ALRM $PPID'; echo in); echo \"cs [$x] $?\"");
  EXPECT_TRUE(sh.expect("cs [] 142\r\n")) << sh.out;
}

// $TMOUT counts for every read the editor does: at PS2 the shell times out
// (no parse error for the unfinished command), and in vared.
TEST(InteractiveM9Review, TmoutAtEveryEditorRead) {
  {
    Pty sh;
    ASSERT_TRUE(sh.expect("% "));
    sh.line("TMOUT=1");
    ASSERT_TRUE(sh.expect("% ")) << sh.out;
    sh.line("if true");
    EXPECT_TRUE(sh.expect("jfsh: timeout\r\n", 5000ms)) << sh.out;
    int st = 0;
    ASSERT_TRUE(sh.exits(st)) << sh.out;
    EXPECT_EQ(WEXITSTATUS(st), 14);
    EXPECT_EQ(sh.out.find("parse error"), std::string::npos) << sh.out;
  }
  {
    Pty sh;
    ASSERT_TRUE(sh.expect("% "));
    sh.line("TMOUT=1; x=a; vared x");
    EXPECT_TRUE(sh.expect("jfsh: timeout\r\n", 5000ms)) << sh.out;
    int st = 0;
    ASSERT_TRUE(sh.exits(st)) << sh.out;
    EXPECT_EQ(WEXITSTATUS(st), 14);
  }
}

// Bang history keeps the quoting of the lines before (zsh's lexer state).
TEST(InteractiveM9Review, BangKeepsQuotesAcrossLines) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo a");
  ASSERT_TRUE(sh.expect("a\r\n")) << sh.out;
  sh.line("echo 'x");
  sh.line("!! y'");
  EXPECT_TRUE(sh.expect("x\r\n!! y\r\n")) << sh.out;
  sh.line("echo b \\");
  sh.line("^y^Y");
  EXPECT_TRUE(sh.expect("b echo x\r\n!! Y\r\n")) << sh.out;
}

// vared -M and -m link main and vicmd for the edit (zsh's savekeymap), and
// a vi insert at its start is one undo step.
TEST(InteractiveM9Review, VaredKeymapsAndViUndo) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("x=ab; vared -M vicmd x; echo \"[$x]\"");
  std::this_thread::sleep_for(200ms);
  sh.send("0x\r");
  EXPECT_TRUE(sh.expect("[b]\r\n")) << sh.out;
  sh.line("bindkey -lL main");
  EXPECT_TRUE(sh.expect("bindkey -A emacs main\r\n")) << sh.out;
  sh.line("x=ab; vared -M nosuch x; echo \"[$x]\"");
  EXPECT_TRUE(sh.expect("vared: no such keymap: nosuch")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("Q\r");
  EXPECT_TRUE(sh.expect("[abQ]\r\n")) << sh.out;
  sh.line("bindkey -v; KEYTIMEOUT=1");
  sh.line("x=ab; vared x; echo \"[$x]\"");
  std::this_thread::sleep_for(200ms);
  sh.send("cd\x1b");
  std::this_thread::sleep_for(200ms);
  sh.send("u\r");
  EXPECT_TRUE(sh.expect("[ab]\r\n")) << sh.out;
}

// A trap run while the editor reads sees the last widget a key ran
// (zsh's bindk) in $WIDGET, and the line in $BUFFER.
TEST(InteractiveM9Review, TrapSeesTheLastWidget) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("trap 'print -r -- \"<$WIDGET|$BUFFER>\"' USR1");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  sh.line("(sleep 1; kill -USR1 $$) &!");
  ASSERT_TRUE(sh.expect("% ")) << sh.out;
  std::this_thread::sleep_for(200ms);
  sh.send("abc\x02");
  EXPECT_TRUE(sh.expect("<backward-char|abc>", 4000ms)) << sh.out;
}

// fc's string events start below the newest entry (hcomsearch) and the
// line being run keeps its number (curline): in a function that adds an
// entry with print -s, the listing runs to that entry.
TEST(InteractiveM9Review, FcStringEventsAndTheCurrentLine) {
  Pty sh;
  ASSERT_TRUE(sh.expect("% "));
  sh.line("echo a");
  sh.line("echo b");
  sh.line("f() { print -s 'echo z'; fc -l echo; }; f");
  EXPECT_TRUE(sh.expect("    2  echo b\r\n    3  f() { print -s 'echo z'; fc -l echo; }; f\r\n"
                        "    4  echo z\r\n")) << sh.out;
}
