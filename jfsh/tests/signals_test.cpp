#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <jfsh/core/jobs.h>
#include <jfsh/core/signals.h>

namespace jfsh {
namespace {

TEST(SignalNames, NameToSigAcceptsBareAndSigPrefixedAnyCase) {
  EXPECT_EQ(SignalTable::name_to_sig("INT"), SIGINT);
  EXPECT_EQ(SignalTable::name_to_sig("SIGINT"), SIGINT);
  EXPECT_EQ(SignalTable::name_to_sig("sigterm"), SIGTERM);
  EXPECT_EQ(SignalTable::name_to_sig("hup"), SIGHUP);
}

TEST(SignalNames, PseudoSignals) {
  EXPECT_EQ(SignalTable::name_to_sig("EXIT"), SIGEXIT);
  EXPECT_EQ(SignalTable::name_to_sig("ZERR"), SIGZERR);
  EXPECT_EQ(SignalTable::name_to_sig("DEBUG"), SIGDEBUG);
}

TEST(SignalNames, UnknownNames) {
  EXPECT_FALSE(SignalTable::name_to_sig("NOPE").has_value());
  EXPECT_FALSE(SignalTable::name_to_sig("SIG").has_value());
  EXPECT_FALSE(SignalTable::name_to_sig("").has_value());
}

TEST(SignalNames, SigToNameRoundTrips) {
  for (int sig : {SIGHUP, SIGINT, SIGTERM, SIGCHLD, SIGWINCH, SIGEXIT}) {
    const char* name = SignalTable::sig_to_name(sig);
    EXPECT_EQ(SignalTable::name_to_sig(name), sig) << name;
  }
  EXPECT_STREQ(SignalTable::sig_to_name(SIGINT), "INT");
  EXPECT_STREQ(SignalTable::sig_to_name(-1), "UNKNOWN");
}

// The self-pipe SIGCHLD handler should deliver a child's exit through
// dispatch_pending(), which calls Jobs.update to drop its slot.
TEST(SignalSelfPipe, SigchldReachesJobTable) {
  Signals.init(false);
  pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) _exit(42);

  int jid = Jobs.alloc(pid, 0, "test-child");
  Process p; p.pid = pid; p.status = ProcStatus::Running;
  Jobs.add_process(jid, std::move(p));

  // Give the child a moment to actually exit so SIGCHLD queues.  Then
  // drain the self-pipe and reap.
  for (int i = 0; i < 100; ++i) {
    Signals.dispatch_pending();
    auto job = Jobs.get(jid);
    if (job and job->get().is_done()) {
      EXPECT_EQ(job->get().procs.front().exit_code, 42);
      return;
    }
    usleep(5'000);
  }
  FAIL() << "SIGCHLD did not propagate through self-pipe within 500 ms";
}

}  // namespace
}  // namespace jfsh
