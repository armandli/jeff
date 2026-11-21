// M13 Phase 13a expansion tests: the (V), (#), (c), (w), (D), (M)
// parameter flags, the `${x:#pat}` array filter, and the `:s/pat/repl/`
// family of history-style substitution modifiers.  Expected values are
// what zsh 5.9 prints under `zsh -f -c`.

#include <string>

#include <gtest/gtest.h>

#include <shell_fixture.h>

namespace jfsh::test {

// ---------------------------------------------------------------------------
// ${x:#pat}: filter elements out (or keep, with (M)).
// ---------------------------------------------------------------------------

TEST(Subst13, FilterDropMatchingArrayElements) {
  Shell sh;
  auto r = sh.run("a=(foo bar baz); echo ${a:#b*}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "foo\n");
  // In double quotes the array is joined first, as in zsh.
  EXPECT_EQ(sh.run("echo \"${a:#b*}\"").out, "foo bar baz\n");
}

TEST(Subst13, FilterKeepMatchingArrayElementsWithMFlag) {
  Shell sh;
  auto r = sh.run("a=(foo bar baz); echo ${(M)a:#b*}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "bar baz\n");
  EXPECT_EQ(sh.run("echo \"${(M)a:#b*}\" \"${(@M)a:#b*}\"").out, " bar baz\n");
}

TEST(Subst13, FilterScalarMatchingBecomesEmpty) {
  Shell sh;
  auto r = sh.run("x=hello; echo \"[${x:#h*}]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[]\n");
}

TEST(Subst13, FilterScalarNonMatchingSurvives) {
  Shell sh;
  auto r = sh.run("x=hello; echo \"[${x:#q*}]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[hello]\n");
}

TEST(Subst13, FilterArrayUnquotedKeepsSplit) {
  Shell sh;
  auto r = sh.run("a=(foo bar baz); print -l ${a:#b*}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "foo\n");
}

TEST(Subst13, FilterArrayWithMKeepsMatchesSplit) {
  Shell sh;
  auto r = sh.run("a=(foo bar baz); print -l ${(M)a:#b*}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "bar\nbaz\n");
}

TEST(Subst13, FilterLeavesNonMatchesWhenAllMatch) {
  Shell sh;
  // No surviving elements; `print -l` on an empty arg list still emits
  // one trailing newline, matching zsh's behaviour.
  auto r = sh.run("a=(bar baz); echo \"[${a:#b*}]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[]\n");
}

// ---------------------------------------------------------------------------
// (V) — visible representation of non-printables.
// ---------------------------------------------------------------------------

TEST(Subst13, VFlagEscapesTab) {
  Shell sh;
  // print -r keeps the (V)-produced backslash escapes visible; echo's
  // default BSD_ECHO-off setting would re-interpret them back to raw.
  auto r = sh.run("x=$'hi\\tthere'; print -r \"${(V)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hi\\tthere\n");
}

TEST(Subst13, VFlagEscapesNewline) {
  Shell sh;
  auto r = sh.run("x=$'one\\ntwo'; print -r \"${(V)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "one\\ntwo\n");
}

TEST(Subst13, VFlagRendersEmptyAsSingleQuotes) {
  Shell sh;
  auto r = sh.run("x=; echo \"[${(V)x}]\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "['']\n");
}

TEST(Subst13, VFlagPassesPrintablesThrough) {
  Shell sh;
  auto r = sh.run("x=hello; echo \"${(V)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hello\n");
}

// ---------------------------------------------------------------------------
// (#) — arithmetic-evaluate each element.
// ---------------------------------------------------------------------------

TEST(Subst13, ArithFlagScalar) {
  Shell sh;
  auto r = sh.run("x='3+4'; echo \"${(#)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "7\n");
}

TEST(Subst13, ArithFlagArrayPerElement) {
  Shell sh;
  auto r = sh.run("a=('1+1' '2*3' '10/2'); echo \"${(#)a}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "2 6 5\n");
}

TEST(Subst13, ArithFlagBadExpressionGivesZero) {
  Shell sh;
  auto r = sh.run("x='not-math'; echo \"${(#)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0\n");
}

// ---------------------------------------------------------------------------
// (c) — character count (per element).
// ---------------------------------------------------------------------------

TEST(Subst13, CountCScalar) {
  Shell sh;
  auto r = sh.run("x=hello; echo \"${(c)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "5\n");
}

TEST(Subst13, CountCArray) {
  Shell sh;
  auto r = sh.run("a=(a bb ccc); echo \"${(c)a}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1 2 3\n");
}

// ---------------------------------------------------------------------------
// (w) — word count per element.
// ---------------------------------------------------------------------------

TEST(Subst13, CountWScalar) {
  Shell sh;
  auto r = sh.run("x='one two three'; echo \"${(w)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "3\n");
}

TEST(Subst13, CountWArray) {
  Shell sh;
  auto r = sh.run("a=('a b' 'c' 'd e f'); echo \"${(w)a}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "2 1 3\n");
}

// ---------------------------------------------------------------------------
// (D) — abbreviate $HOME as ~ in paths.
// ---------------------------------------------------------------------------

TEST(Subst13, DFlagAbbreviatesHome) {
  Shell sh;
  auto r = sh.run("HOME=/home/user; x=/home/user/docs; echo \"${(D)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "~/docs\n");
}

TEST(Subst13, DFlagLeavesNonHomePathsAlone) {
  Shell sh;
  auto r = sh.run("HOME=/home/user; x=/etc/hosts; echo \"${(D)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "/etc/hosts\n");
}

TEST(Subst13, DFlagRequiresBoundary) {
  Shell sh;
  // /home/user_two shouldn't be abbreviated: the match must end at a /.
  auto r = sh.run("HOME=/home/user; x=/home/user_two/x; echo \"${(D)x}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "/home/user_two/x\n");
}

// ---------------------------------------------------------------------------
// :s/pat/repl/, :gs/pat/repl/, :& history-style substitution modifiers.
// ---------------------------------------------------------------------------

TEST(Subst13, SubstModifierReplacesFirst) {
  Shell sh;
  auto r = sh.run("x='hello hello'; echo \"${x:s/hello/HI/}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "HI hello\n");
}

TEST(Subst13, GsSubstModifierReplacesAll) {
  Shell sh;
  auto r = sh.run("x='hello hello'; echo \"${x:gs/hello/HI/}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "HI HI\n");
}

TEST(Subst13, SubstModifierAlternativeDelim) {
  Shell sh;
  auto r = sh.run("x=/usr/local/bin; echo \"${x:gs|/|:|}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, ":usr:local:bin\n");
}

TEST(Subst13, AmpRepeatsLastSubst) {
  Shell sh;
  auto r = sh.run("x='a.b.c.d'; echo \"${x:s/./-/:&}\"");
  EXPECT_EQ(r.rc, 0);
  // :s replaces first '.', :& repeats the same pattern/repl on the result.
  EXPECT_EQ(r.out, "a-b-c.d\n");
}

TEST(Subst13, SubstChainsWithHistoryModifiers) {
  Shell sh;
  auto r = sh.run("x=/a/b/c.txt; echo \"${x:t:s/txt/md/}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "c.md\n");
}

}  // namespace jfsh::test
