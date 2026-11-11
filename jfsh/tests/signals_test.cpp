#include <csignal>

#include <gtest/gtest.h>

#include <jfsh/core/signals.hh>

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

}  // namespace
}  // namespace jfsh
