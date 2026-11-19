// M13 Phase 13b tests: the `(#i)` / `(#l)` / `(#I)` inline globbing flags
// and KSH_GLOB's `?(…)`, `*(…)`, `+(…)`, `@(…)` extended-glob operators,
// plus the already-wired `NO_GLOB` (`set -f`) toggle.  Expected outputs
// are what zsh 5.9 prints under `zsh -f -c`.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <shell_fixture.h>

namespace jfsh::test {

namespace sf = std::filesystem;

// Helper: three files inside a scratch TempDir.
struct FooBarBaz {
  TempDir td;
  FooBarBaz() {
    std::ofstream{td.path / "foo"};
    std::ofstream{td.path / "bar"};
    std::ofstream{td.path / "baz"};
  }
  std::string cd() const { return "cd " + td.path.string() + "\n"; }
};

// ---------------------------------------------------------------------------
// Inline globbing flags: (#i) / (#l) / (#I).
// ---------------------------------------------------------------------------

TEST(Glob13, InlineCaseInsensitiveMatchesMixedCase) {
  Shell sh;
  auto r = sh.run("[[ FOO = (#i)foo ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

TEST(Glob13, InlineCaseInsensitiveLowerFlagAlias) {
  Shell sh;
  auto r = sh.run("[[ ABC = (#l)abc ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

TEST(Glob13, CaseSensitiveByDefault) {
  Shell sh;
  auto r = sh.run("[[ FOO = foo ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "no\n");
}

TEST(Glob13, InlineFlagAppliesInParamFilter) {
  Shell sh;
  auto r = sh.run("a=(Foo bar BAZ); echo \"${(M)a:#(#i)foo}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "Foo\n");
}

TEST(Glob13, InlineFlagAppliesInSubstReplace) {
  Shell sh;
  auto r = sh.run("x=HelloWorld; echo \"${x:gs/(#i)l/L/}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "HeLLoWorLd\n");
}

TEST(Glob13, InlineCaseSensitiveResetFlag) {
  Shell sh;
  // Stacking (#i)(#I) resets to case-sensitive; FOO stops matching foo.
  auto r = sh.run("[[ FOO = (#i)(#I)foo ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "no\n");
}

// ---------------------------------------------------------------------------
// KSH_GLOB extended-glob operators.  All require `setopt kshglob`.
// ---------------------------------------------------------------------------

TEST(Glob13, KshGlobAtGroupExactlyOne) {
  Shell sh;
  FooBarBaz fb;
  auto r = sh.run(fb.cd() + "setopt kshglob; echo @(foo|baz)");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "baz foo\n");
}

TEST(Glob13, KshGlobQuestionGroupZeroOrOne) {
  Shell sh;
  FooBarBaz fb;
  // ?(bar) matches "bar"; and because of the implicit empty alt, nothing
  // else in the dir has a zero-match either, so just "bar" shows up.
  auto r = sh.run(fb.cd() + "setopt kshglob; echo ?(bar)");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "bar\n");
}

TEST(Glob13, KshGlobStarGroupZeroOrMore) {
  Shell sh;
  FooBarBaz fb;
  auto r = sh.run(fb.cd() + "setopt kshglob; echo *(foo|bar)");
  EXPECT_EQ(r.rc, 0);
  // *(…) matches any sequence of the inner alternatives.  Among our
  // three files, "foo" and "bar" match; "baz" does not.
  EXPECT_EQ(r.out, "bar foo\n");
}

TEST(Glob13, KshGlobPlusGroupOneOrMore) {
  Shell sh;
  FooBarBaz fb;
  auto r = sh.run(fb.cd() + "setopt kshglob; echo +(bar|baz)");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "bar baz\n");
}

TEST(Glob13, KshGlobInConditionMatch) {
  Shell sh;
  auto r = sh.run("setopt kshglob; [[ foobar = @(foo|bar)* ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

TEST(Glob13, KshGlobPlusMustMatchAtLeastOnce) {
  Shell sh;
  auto r = sh.run("setopt kshglob; [[ '' = +(x) ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "no\n");
}

TEST(Glob13, KshGlobStarMatchesEmpty) {
  Shell sh;
  auto r = sh.run("setopt kshglob; [[ '' = *(x) ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

TEST(Glob13, KshGlobOffByDefault) {
  Shell sh;
  // Without KSH_GLOB, @(foo|bar) is parsed as `@` literal followed by a
  // group — the match against "foobar" fails.
  auto r = sh.run("[[ foobar = @(foo|bar)* ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "no\n");
}

TEST(Glob13, EmulateKshTurnsKshGlobOn) {
  Shell sh;
  // ksh emulation flips KSH_GLOB on by default.
  auto r = sh.run("emulate ksh; [[ foobar = @(foo|bar)* ]] && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

// ---------------------------------------------------------------------------
// NO_GLOB (set -f): disables filesystem globbing.
// ---------------------------------------------------------------------------

TEST(Glob13, NoGlobLeavesPatternLiteral) {
  Shell sh;
  FooBarBaz fb;
  // `setopt noglob` (zsh's name) / `set -F` (letter) disables filesystem
  // globbing: `*` stays literal.  NOMATCH stays on, so the pattern
  // reaches echo as "*".  (POSIX `-f` is `set -o nofcorrect` in zsh;
  // the no-glob name is `noglob` or the letter `F`.)
  auto r = sh.run(fb.cd() + "setopt noglob; echo *");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "*\n");
}

TEST(Glob13, GlobRestoredAfterUnset) {
  Shell sh;
  FooBarBaz fb;
  auto r = sh.run(fb.cd() + "setopt noglob; unsetopt noglob; echo *");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "bar baz foo\n");
}

}  // namespace jfsh::test
