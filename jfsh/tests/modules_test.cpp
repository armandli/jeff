// Tests for the M10 module framework and the shipped Phase-1 modules
// (zsh/datetime, zsh/mathfunc).  Uses the in-process Shell fixture; expected
// outputs match zsh 5.9's `zsh -f -c`.

#include <cmath>

#include <gtest/gtest.h>

#include <shell_fixture.h>

namespace jfsh::test {

// ---------------------------------------------------------------------------
// Framework: zmodload.  main.cpp auto-loads zsh/datetime and zsh/mathfunc.
// ---------------------------------------------------------------------------

TEST(Modules, ZmodloadListsLoadedModulesByDefault) {
  Shell sh;
  auto r = sh.run("zmodload");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("zsh/datetime"),  std::string::npos);
  EXPECT_NE(r.out.find("zsh/mathfunc"),  std::string::npos);
}

TEST(Modules, ZmodloadDashLShowsSourceableForm) {
  Shell sh;
  auto r = sh.run("zmodload -L");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("zmodload zsh/datetime"), std::string::npos);
  EXPECT_NE(r.out.find("zmodload zsh/mathfunc"), std::string::npos);
}

TEST(Modules, ZmodloadDashERetursSuccessWhenLoaded) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -e zsh/datetime").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/not-a-real-module").rc, 1);
}

TEST(Modules, ZmodloadUnloadMarksModuleAbsent) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -e zsh/mathfunc").rc, 0);
  EXPECT_EQ(sh.run("zmodload -u zsh/mathfunc").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/mathfunc").rc, 1);
  // Re-load restores it.
  EXPECT_EQ(sh.run("zmodload zsh/mathfunc").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/mathfunc").rc, 0);
}

TEST(Modules, ZmodloadDashAAddsAliases) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -A date=zsh/datetime").rc, 0);
  // Loading through the alias finds the real module.
  auto r = sh.run("zmodload -e date");
  EXPECT_EQ(r.rc, 0);
}

TEST(Modules, ZmodloadBadFlagReportsAndFails) {
  Shell sh;
  auto r = sh.run("zmodload -Z");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("bad option: -Z"), std::string::npos);
}

TEST(Modules, ZmodloadLoadUnknownModuleFails) {
  Shell sh;
  auto r = sh.run("zmodload zsh/not-a-real-thing");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("failed to load module"), std::string::npos);
}

TEST(Modules, ZmodloadUnloadUnknownModuleFails) {
  Shell sh;
  auto r = sh.run("zmodload -u zsh/not-a-real-thing");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("no such module"), std::string::npos);
}

TEST(Modules, ZmodloadUnloadWithDashIgnoresMissing) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -ui zsh/not-a-real-thing").rc, 0);
}

// ---------------------------------------------------------------------------
// zsh/mathfunc.
// ---------------------------------------------------------------------------

TEST(Modules, MathfuncSqrtOfFour) {
  Shell sh;
  auto r = sh.run("echo $((sqrt(4)))");
  EXPECT_EQ(r.rc, 0);
  // zsh renders a float with 17 sig figs; 2.0 round-trips as "2.".
  EXPECT_EQ(r.out, "2.\n");
}

TEST(Modules, MathfuncSinZero) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((sin(0)))").out, "0.\n");
}

TEST(Modules, MathfuncIntTruncatesTowardZero) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((int(3.7)))").out, "3\n");
  EXPECT_EQ(sh.run("echo $((int(-3.7)))").out, "-3\n");
}

TEST(Modules, MathfuncFloatPromotes) {
  Shell sh;
  // float(3) → 3.0, which prints as "3."
  EXPECT_EQ(sh.run("echo $((float(3)))").out, "3.\n");
}

TEST(Modules, MathfuncAtan2OfOneOne) {
  Shell sh;
  auto r = sh.run("echo $((atan2(1, 1)))");
  EXPECT_EQ(r.rc, 0);
  // atan2(1,1) = π/4 ≈ 0.78539816339744828
  EXPECT_NE(r.out.find("0.78539816339"), std::string::npos);
}

TEST(Modules, MathfuncHypotOfThreeFour) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((hypot(3,4)))").out, "5.\n");
}

TEST(Modules, MathfuncAbsHandlesIntAndFloat) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((abs(-5)))").out,    "5\n");
  EXPECT_EQ(sh.run("echo $((abs(-2.5)))").out,  "2.5\n");
}

TEST(Modules, MathfuncUnknownFunctionIsAnError) {
  Shell sh;
  auto r = sh.run("echo $((noSuchFunc(1)))");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown math function"), std::string::npos);
}

TEST(Modules, MathfuncWrongArityIsAnError) {
  Shell sh;
  auto r = sh.run("echo $((sqrt(1,2)))");
  EXPECT_NE(r.rc, 0);
}

TEST(Modules, MathfuncCombinesWithOperatorsAndParams) {
  Shell sh;
  auto r = sh.run("x=9; echo $((sqrt(x) + 1))");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "4.\n");
}

TEST(Modules, MathfuncUnloadRemovesFunctions) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -u zsh/mathfunc").rc, 0);
  auto r = sh.run("echo $((sqrt(4)))");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown math function"), std::string::npos);
}

TEST(Modules, LetUsesMathfunc) {
  Shell sh;
  // `let x = sqrt(16)` assigns a float to x; jfsh prints floats in
  // typeset -F form (10 decimals) when read back as a scalar.
  auto r = sh.run("let 'x = sqrt(16)'; echo $x");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out.substr(0, 3), "4.0");
}

// ---------------------------------------------------------------------------
// zsh/datetime — the strftime builtin.
// ---------------------------------------------------------------------------

TEST(Modules, StrftimeFormatsEpoch) {
  Shell sh;
  // Pass a fixed epoch time (midnight 1970-01-01 UTC) — but result depends
  // on local TZ, so just check the shape with %s (which is the input).
  auto r = sh.run("strftime '%s' 1700000000");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1700000000\n");
}

TEST(Modules, StrftimeDashSStoresInVariable) {
  Shell sh;
  auto r = sh.run("strftime -s t '%s' 1700000000; echo $t");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1700000000\n");
}

TEST(Modules, StrftimeNeedsAFormat) {
  Shell sh;
  auto r = sh.run("strftime");
  EXPECT_NE(r.rc, 0);
}

TEST(Modules, StrftimeBadOption) {
  Shell sh;
  auto r = sh.run("strftime -Q '%s'");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("bad option"), std::string::npos);
}

TEST(Modules, EpochSecondsIsIntegerWithPositiveValue) {
  Shell sh;
  // $EPOCHSECONDS is installed as a special by install_special_params;
  // the datetime module records it in features.  Either way, it should
  // read as a positive integer.
  auto r = sh.run("echo $(( EPOCHSECONDS > 1000000000 ))");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1\n");
}

// ---------------------------------------------------------------------------
// zmodload -a: autoload mark (recorded; load-on-use not yet wired).
// ---------------------------------------------------------------------------

TEST(Modules, ZmodloadDashAMarksAutoload) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -a zsh/pcre pcre_match").rc, 0);
  auto r = sh.run("zmodload -L");
  // After marking, zmodload -L lists zsh/pcre in autoload form.
  EXPECT_NE(r.out.find("zmodload -a zsh/pcre"), std::string::npos);
}

}  // namespace jfsh::test
