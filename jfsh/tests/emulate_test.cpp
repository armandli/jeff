// Tests for M12 emulation modes: `emulate` builtin, invocation-name
// detection, and the per-option behavior changes that go with each mode.
// Expected values come from zsh 5.9 run with the same emulation
// (`zsh -f --emulate sh`).

#include <cstdio>
#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include <shell_fixture.h>

namespace jfsh::test {

// ---------------------------------------------------------------------------
// Builtin: `emulate` with no mode prints the current one.
// ---------------------------------------------------------------------------

TEST(Emulate, PrintsCurrentEmulation) {
  Shell sh;
  auto r = sh.run("emulate");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "zsh\n");
}

TEST(Emulate, SwitchToSh) {
  Shell sh;
  auto r = sh.run("emulate sh; emulate");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "sh\n");
}

TEST(Emulate, SwitchToKsh) {
  Shell sh;
  EXPECT_EQ(sh.run("emulate ksh; emulate").out, "ksh\n");
}

TEST(Emulate, SwitchToCsh) {
  Shell sh;
  EXPECT_EQ(sh.run("emulate csh; emulate").out, "csh\n");
}

TEST(Emulate, UnknownModeFails) {
  Shell sh;
  auto r = sh.run("emulate perl");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown emulation"), std::string::npos);
}

TEST(Emulate, BadOptionFails) {
  Shell sh;
  auto r = sh.run("emulate -Z");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("bad option"), std::string::npos);
}

TEST(Emulate, BashIsAnAliasForSh) {
  Shell sh;
  EXPECT_EQ(sh.run("emulate bash; emulate").out, "sh\n");
}

// ---------------------------------------------------------------------------
// `emulate -R` fully resets; `emulate` without -R only touches `emulate`-
// flagged options.  Both apply the new emulation's defaults.
// ---------------------------------------------------------------------------

TEST(Emulate, DashRFullReset) {
  Shell sh;
  // Pre-set EXTENDED_GLOB (an `emulate`-flagged option).  `emulate -R zsh`
  // puts it back to its zsh default (off).
  auto r = sh.run(
      "setopt extendedglob\n"
      "emulate -R zsh\n"
      "[[ -o extendedglob ]] && echo on || echo off");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "off\n");
}

TEST(Emulate, DashLListsRestoreScript) {
  Shell sh;
  auto r = sh.run("emulate -l");
  EXPECT_EQ(r.rc, 0);
  // Starts with the `emulate -R MODE` line.
  EXPECT_EQ(r.out.substr(0, 10), "emulate -R");
  EXPECT_NE(r.out.find("zsh"), std::string::npos);
}

TEST(Emulate, DashLWithModeDescribesDelta) {
  Shell sh;
  // From zsh to sh, SHWORDSPLIT flips on.
  auto r = sh.run("emulate -l sh");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("emulate -R sh"), std::string::npos);
  EXPECT_NE(r.out.find("setopt shwordsplit"), std::string::npos);
}

// ---------------------------------------------------------------------------
// `emulate -c cmd` runs cmd under the mode and restores the option set.
// ---------------------------------------------------------------------------

TEST(Emulate, DashCRunsCommandUnderMode) {
  Shell sh;
  // Inside -c the mode is sh (SHWORDSPLIT on); after it, back to zsh.
  auto r = sh.run(
      "emulate sh -c 'emulate'\n"
      "emulate");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "sh\nzsh\n");
}

TEST(Emulate, DashCPutsOptionsBack) {
  Shell sh;
  auto r = sh.run(
      "emulate sh -c 'x=\"a b c\"; for w in $x; do echo $w; done'\n"
      "x=\"d e\"; for w in $x; do echo $w; done");
  EXPECT_EQ(r.rc, 0);
  // Inside -c: 3 lines; outside: 1 line.
  EXPECT_EQ(r.out, "a\nb\nc\nd e\n");
}

// ---------------------------------------------------------------------------
// SH_WORD_SPLIT.  Off by default under zsh; on under sh / ksh.
// ---------------------------------------------------------------------------

TEST(Emulate, ZshDoesNotSplitScalarByDefault) {
  Shell sh;
  auto r = sh.run("x=\"a b c\"; for w in $x; do echo $w; done");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "a b c\n");
}

TEST(Emulate, ShSplitsScalarByDefault) {
  Shell sh;
  auto r = sh.run("emulate sh; x=\"a b c\"; for w in $x; do echo $w; done");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "a\nb\nc\n");
}

TEST(Emulate, KshSplitsScalarByDefault) {
  Shell sh;
  auto r = sh.run("emulate ksh; x=\"a b c\"; for w in $x; do echo $w; done");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "a\nb\nc\n");
}

TEST(Emulate, EqSplitOverridesInZsh) {
  Shell sh;
  // `${=x}` forces splitting even when SHWORDSPLIT is off.
  auto r = sh.run("x=\"a b c\"; for w in ${=x}; do echo $w; done");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "a\nb\nc\n");
}

// ---------------------------------------------------------------------------
// BSD_ECHO.  Off by default in zsh; on in sh (echo doesn't interpret
// backslash escapes unless -e).
// ---------------------------------------------------------------------------

TEST(Emulate, ZshEchoInterpretsEscapes) {
  Shell sh;
  auto r = sh.run("echo 'hi\\tthere'");
  EXPECT_EQ(r.rc, 0);
  // \t becomes a literal tab.
  EXPECT_NE(r.out.find('\t'), std::string::npos);
}

TEST(Emulate, ShEchoLiteralByDefault) {
  Shell sh;
  auto r = sh.run("emulate sh; echo 'hi\\tthere'");
  EXPECT_EQ(r.rc, 0);
  // Backslash-t stays literal under BSD_ECHO.
  EXPECT_EQ(r.out.find('\t'), std::string::npos);
  EXPECT_NE(r.out.find("\\t"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Options are actually reset when switching emulations.
// ---------------------------------------------------------------------------

TEST(Emulate, ChangingEmulationResetsEmulateFlaggedOptions) {
  Shell sh;
  auto r = sh.run(
      "emulate sh\n"
      "[[ -o shwordsplit ]] && echo on || echo off\n"
      "emulate zsh\n"
      "[[ -o shwordsplit ]] && echo on || echo off");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "on\noff\n");
}

TEST(Emulate, SpecialOptionsAreNotTouched) {
  Shell sh;
  // INTERACTIVE is a special option; emulate shouldn't flip it.
  auto r = sh.run(
      "[[ -o interactive ]] && echo i-on || echo i-off\n"
      "emulate -R sh\n"
      "[[ -o interactive ]] && echo i-on || echo i-off");
  EXPECT_EQ(r.rc, 0);
  // In both lines the status matches whatever the test fixture's shell is
  // (non-interactive), but the two lines must agree.
  auto lines_equal = [&] {
    auto nl = r.out.find('\n');
    return nl != std::string::npos
        and r.out.substr(0, nl) == r.out.substr(nl + 1, r.out.size() - nl - 2);
  };
  EXPECT_TRUE(lines_equal());
}

// ---------------------------------------------------------------------------
// LOCAL_OPTIONS: a function with `emulate -L` restores options on return.
// ---------------------------------------------------------------------------

TEST(Emulate, DashLLocalOptionsRestoredOnReturn) {
  Shell sh;
  auto r = sh.run(
      "f() { emulate -L sh; [[ -o shwordsplit ]] && echo in-on; }\n"
      "f\n"
      "[[ -o shwordsplit ]] && echo out-on || echo out-off");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "in-on\nout-off\n");
}

// ---------------------------------------------------------------------------
// Invocation-name tests.  Run the real `jfsh` binary with argv[0] set to
// `sh` / `ksh` / `csh` and verify the emulation picks up automatically.
// ---------------------------------------------------------------------------

#ifdef JFSH_BINARY
namespace {

struct InvocationRun { int rc; std::string out; };

InvocationRun run_as(std::string name, std::string command) {
  // Put the jfsh binary under a unique directory with the target basename
  // so argv[0]'s basename matches `name` exactly.
  char dir[] = "/tmp/jfsh_invocation_XXXXXX";
  if (not ::mkdtemp(dir)) return {-1, ""};
  std::string dst = std::string(dir) + "/" + name;
  std::string cp  = std::string("cp ") + JFSH_BINARY + " " + dst;
  std::system(cp.c_str());
  std::string cmd = dst + " -c " + "'" + command + "'" + " 2>&1";
  FILE* p = ::popen(cmd.c_str(), "r");
  if (not p) return {-1, ""};
  std::string out;
  char buf[4096];
  while (std::fgets(buf, sizeof buf, p)) out += buf;
  int rc = ::pclose(p);
  std::remove(dst.c_str());
  ::rmdir(dir);
  return {WEXITSTATUS(rc), out};
}

}  // namespace

TEST(Emulate, InvokedAsShSelectsShEmulation) {
  auto r = run_as("sh", "emulate");
  EXPECT_EQ(r.out, "sh\n");
}

TEST(Emulate, InvokedAsKshSelectsKshEmulation) {
  auto r = run_as("ksh", "emulate");
  EXPECT_EQ(r.out, "ksh\n");
}

TEST(Emulate, InvokedAsCshSelectsCshEmulation) {
  auto r = run_as("csh", "emulate");
  EXPECT_EQ(r.out, "csh\n");
}

TEST(Emulate, InvokedAsJfshSelectsZshEmulation) {
  auto r = run_as("jfsh", "emulate");
  EXPECT_EQ(r.out, "zsh\n");
}

TEST(Emulate, InvokedAsBashSelectsShEmulation) {
  auto r = run_as("bash", "emulate");
  EXPECT_EQ(r.out, "sh\n");
}
#endif

// ---------------------------------------------------------------------------
// M12 Phase 12b: KSH_ARRAYS, KSH_ZERO_SUBSCRIPT.
// ---------------------------------------------------------------------------

TEST(Emulate, ZshArraysAreOneBased) {
  Shell sh;
  auto r = sh.run("a=(x y z); echo \"$a[1]/$a[2]/$a[3]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x/y/z\n");
}

TEST(Emulate, ZshIndexZeroIsEmpty) {
  Shell sh;
  auto r = sh.run("a=(x y z); echo \"[$a[0]]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[]\n");
}

TEST(Emulate, ZshNegativeIndexesFromTheEnd) {
  Shell sh;
  auto r = sh.run("a=(x y z); echo \"$a[-1]/$a[-2]/$a[-3]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "z/y/x\n");
}

TEST(Emulate, KshArraysAreZeroBased) {
  Shell sh;
  auto r = sh.run("setopt ksharrays; a=(x y z); echo \"$a[0]/$a[1]/$a[2]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x/y/z\n");
}

TEST(Emulate, KshNegativeIndexesFromTheEnd) {
  Shell sh;
  auto r = sh.run("setopt ksharrays; a=(x y z); echo \"$a[-1]/$a[-2]/$a[-3]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "z/y/x\n");
}

TEST(Emulate, KshIndexOutOfRangeIsEmpty) {
  Shell sh;
  auto r = sh.run("setopt ksharrays; a=(x y z); echo \"[$a[3]]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[]\n");
}

TEST(Emulate, KshZeroSubscriptInZshMapsZeroToOne) {
  Shell sh;
  auto r = sh.run("setopt kshzerosubscript; a=(x y z); echo \"$a[0]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x\n");
}

TEST(Emulate, KshArraysInArithmetic) {
  Shell sh;
  auto r = sh.run(
      "a=(10 20 30)\n"
      "echo \"zsh=$((a[2])) ksh=$(setopt ksharrays; echo $((a[2])))\"");
  EXPECT_EQ(r.rc, 0);
  // zsh: a[2]=20; ksh: a[2]=30.
  EXPECT_EQ(r.out, "zsh=20 ksh=30\n");
}

TEST(Emulate, KshArraysSlicePositive) {
  Shell sh;
  // Slice: zsh `a[2,3]` picks 2nd..3rd; ksh picks 3rd+ off the 0-based
  // range, which is only the 3rd slot.
  auto r = sh.run(
      "a=(w x y z)\n"
      "echo \"${a[2,3]}\" \"$(setopt ksharrays; echo ${a[2,3]})\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x y y z\n");
}

TEST(Emulate, KshArraysLengthStillArraySize) {
  Shell sh;
  // ${#a} is array length under both regimes (not length of $a[0]).
  auto r = sh.run("setopt ksharrays; a=(x y z); echo \"${#a}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "3\n");
}

TEST(Emulate, KshArraysAtGivesAllElements) {
  Shell sh;
  auto r = sh.run("setopt ksharrays; a=(x y z); echo \"${a[@]}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x y z\n");
}

TEST(Emulate, KshArraysScalarIndexAssignment) {
  Shell sh;
  // `(( a[i] = v ))` through the arithmetic evaluator honours KSH_ARRAYS.
  auto r = sh.run(
      "setopt ksharrays; a=(x y z); (( a[0] = 99 )); echo \"$a[0]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "99\n");
}

TEST(Emulate, EmulateKshSwitchesIndexingAutomatically) {
  Shell sh;
  // ksh emulation turns KSH_ARRAYS on by default.
  auto r = sh.run("emulate ksh; a=(x y z); echo \"$a[0]/$a[1]/$a[2]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x/y/z\n");
}

TEST(Emulate, EmulateZshRevertsToOneBased) {
  Shell sh;
  auto r = sh.run(
      "emulate ksh\n"
      "emulate zsh\n"
      "a=(x y z); echo \"$a[1]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x\n");
}

// ---------------------------------------------------------------------------
// M12 Phase 12c: POSIX_TRAPS, SH_NULLCMD / CSH_NULLCMD.
// ---------------------------------------------------------------------------

TEST(Emulate, ZshTrapSetInFunctionSurvivesReturn) {
  Shell sh;
  // Default zsh behaviour: a trap set inside a function stays after
  // the function returns.  `trap` with no args lists it from the caller.
  auto r = sh.run(
      "f() { trap 'echo hit' USR1; }\n"
      "f\n"
      "trap");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("USR1"), std::string::npos);
}

TEST(Emulate, PosixTrapsConfinesTrapToFunction) {
  Shell sh;
  // Under POSIX_TRAPS the trap goes out of scope when `f` returns.
  auto r = sh.run(
      "setopt posixtraps\n"
      "f() { trap 'echo hit' USR1; }\n"
      "f\n"
      "trap");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out.find("USR1"), std::string::npos);
}

TEST(Emulate, LocalTrapsAlsoConfinesTrap) {
  Shell sh;
  // The existing LOCAL_TRAPS path still does what POSIX_TRAPS now also
  // gets you — verify they agree.
  auto r = sh.run(
      "f() { setopt localoptions localtraps; trap 'echo hit' USR1; }\n"
      "f\n"
      "trap");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out.find("USR1"), std::string::npos);
}

TEST(Emulate, ZshRedirectionWithNoCommandIsOk) {
  Shell sh;
  // zsh's default runs $NULLCMD (cat) on `> f` by itself.
  auto r = sh.run("echo hi | > /tmp/jfsh_null_test_$$; echo $?; cat /tmp/jfsh_null_test_$$");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0\nhi\n");
  // Clean up (the subshell created the file).
  std::remove(("/tmp/jfsh_null_test_" + std::to_string(::getpid())).c_str());
}

// SH_NULLCMD (on under sh and ksh emulation) runs `:` for a command of
// nothing but redirections; CSH_NULLCMD refuses it, which ends the
// command line, as zsh does.
TEST(Emulate, ShNullCmdRunsColonForRedirectOnlyCommand) {
  Shell sh;
  TempDir dir;
  str f = (dir.path / "f").string();
  auto r = sh.run("emulate sh; echo hi | > " + f + " || echo refused; cat " + f);
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "");
}

TEST(Emulate, CshNullCmdRejectsRedirectOnlyCommand) {
  Shell sh;
  auto r = sh.run("setopt cshnullcmd; (> /tmp/jfsh_cshnull_$$ 2>&1) || echo refused; echo after");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "refused\nafter\n");
  EXPECT_EQ(r.err, "jfsh: redirection with no command\n");
  r = sh.run("setopt cshnullcmd; > /dev/null || echo refused; echo not-here");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
}

TEST(Emulate, EmulateKshAlsoRunsColonForNullCmd) {
  Shell sh;
  // ksh emulation has SH_NULLCMD on by default.
  auto r = sh.run("emulate ksh; echo hi | > /dev/null || echo refused");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "");
}

TEST(Emulate, EmulateZshRevertsNullCmdBehaviour) {
  Shell sh;
  auto r = sh.run(
      "emulate sh\n"
      "emulate zsh\n"
      "echo hi | > /tmp/jfsh_rev_$$; echo $?; cat /tmp/jfsh_rev_$$");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0\nhi\n");
  std::remove(("/tmp/jfsh_rev_" + std::to_string(::getpid())).c_str());
}

}  // namespace jfsh::test
