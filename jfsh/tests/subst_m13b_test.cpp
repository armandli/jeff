// M13 Phase 13b expansion tests: ${^x} / RC_EXPAND_PARAM, the subscript
// flags (r) (R) (i) (I) (e) (k) (K) (w) (s) (n) (b), padding (l) (r),
// (%) and (e), the :c and :P modifiers, globbing of unquoted pattern
// characters in ${x:-word}, and a failed ${x?} ending the shell.
// Expected values are what zsh 5.9 prints under `zsh -f -c`.

#include <string>

#include <gtest/gtest.h>

#include <shell_fixture.h>

namespace jfsh::test {

// ---------------------------------------------------------------------------
// ${^x}, ${^^x}, RC_EXPAND_PARAM.
// ---------------------------------------------------------------------------

TEST(Subst13b, CaretDistributesWordAroundElements) {
  Shell sh;
  auto r = sh.run("a=(1 2); print -l x${^a}y");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x1y\nx2y\n");
}

TEST(Subst13b, CaretTwiceMakesCartesianProduct) {
  Shell sh;
  auto r = sh.run("a=(1 2) b=(p q); print -l ${^a}-${^b}");
  EXPECT_EQ(r.out, "1-p\n1-q\n2-p\n2-q\n");
}

TEST(Subst13b, CaretOnEmptyArrayRemovesWord) {
  Shell sh;
  auto r = sh.run("a=(); print -l x${^a}y end");
  EXPECT_EQ(r.out, "end\n");
}

TEST(Subst13b, CaretKeepsQuotedPrefix) {
  Shell sh;
  auto r = sh.run("a=(1 2); print -l \"p\"${^a}q");
  EXPECT_EQ(r.out, "p1q\np2q\n");
}

TEST(Subst13b, CaretInDoubleQuotesJoinsPlainArray) {
  Shell sh;
  auto r = sh.run("a=(1 2); print -l \"x${^a}y\"");
  EXPECT_EQ(r.out, "x1 2y\n");
}

TEST(Subst13b, CaretInDoubleQuotesWithAtDistributes) {
  Shell sh;
  auto r = sh.run("a=(1 2); print -l \"${^a[@]}z\"");
  EXPECT_EQ(r.out, "1z\n2z\n");
}

TEST(Subst13b, CaretAppliesAfterSplitting) {
  Shell sh;
  auto r = sh.run("a=('a b' c); print -l ${^=a}z");
  EXPECT_EQ(r.out, "az\nbz\ncz\n");
}

TEST(Subst13b, RcExpandParamOptionAndDoubleCaretOff) {
  Shell sh;
  auto r = sh.run("a=(1 2); setopt rcexpandparam; print -l x${a}y; print -l x${^^a}y");
  EXPECT_EQ(r.out, "x1y\nx2y\nx1\n2y\n");
}

TEST(Subst13b, CaretResultsAreGlobbed) {
  Shell sh;
  TempDir dir;
  auto r = sh.run("(cd " + dir.path.string() + "; touch a1 b1; a=(a b); print -l ${^a}*)");
  EXPECT_EQ(r.out, "a1\nb1\n");
}

TEST(Subst13b, CaretWithPaddingFlag) {
  Shell sh;
  auto r = sh.run("a=(1 2); print -l ${(l:3::0:)^a}x");
  EXPECT_EQ(r.out, "001x\n002x\n");
}

// ---------------------------------------------------------------------------
// Subscript flags on arrays.
// ---------------------------------------------------------------------------

TEST(Subst13b, SubscriptReverseAndIndex) {
  Shell sh;
  auto r = sh.run("a=(foo bar baz bar); echo ${a[(r)ba*]} ${a[(R)ba*]} "
                  "${a[(i)ba*]} ${a[(I)ba*]} ${a[(i)zz]} ${a[(I)zz]} ${a[(r)zz]}x");
  EXPECT_EQ(r.out, "bar bar 2 4 5 0 x\n");
}

TEST(Subst13b, SubscriptExactFlag) {
  Shell sh;
  auto r = sh.run("a=(foo 'b*' bar); echo ${a[(i)b*]} ${a[(ie)b*]} ${a[(re)b*]}");
  EXPECT_EQ(r.out, "2 2 b*\n");
}

TEST(Subst13b, SubscriptNthMatchAndBeginAt) {
  Shell sh;
  auto r = sh.run("a=(x1 y x2 x3); echo ${a[(in:2:)x*]} ${a[(rn:2:)x*]} "
                  "${a[(ib:2:)x*]} ${a[(rb:2:)x*]}");
  EXPECT_EQ(r.out, "3 x2 3 x2\n");
}

TEST(Subst13b, SubscriptBackwardNthAndBeginAt) {
  Shell sh;
  auto r = sh.run("a=(a1 b1 a2 b2 a3); echo ${a[(Rn:2:)a*]} ${a[(In:2:)a*]} "
                  "${a[(Ib:4:)a*]} ${a[(Rb:4:)a*]}");
  EXPECT_EQ(r.out, "a2 3 3 a2\n");
}

TEST(Subst13b, SubscriptPatternFromParameterIsActiveUnlessQuoted) {
  Shell sh;
  auto r = sh.run("p='b*'; a=(foo bar); echo ${a[(r)$p]} ${a[(r)\"$p\"]}x");
  EXPECT_EQ(r.out, "bar x\n");
}

TEST(Subst13b, SubscriptIndexUnderKshArrays) {
  Shell sh;
  auto r = sh.run("a=(foo bar); setopt ksharrays; echo ${a[(i)b*]} ${a[(i)z*]} ${a[(I)z*]}");
  EXPECT_EQ(r.out, "1 2 0\n");
}

// ---------------------------------------------------------------------------
// Subscript flags on scalars and associations.
// ---------------------------------------------------------------------------

TEST(Subst13b, SubscriptOnScalarSearchesPositions) {
  Shell sh;
  auto r = sh.run("s='hello world'; echo ${s[(i)o]} ${s[(I)o]} ${s[(r)o*]} "
                  "${s[(i)z]} ${s[(I)z]}");
  EXPECT_EQ(r.out, "5 8 o 12 0\n");
}

TEST(Subst13b, SubscriptWordFlags) {
  Shell sh;
  auto r = sh.run("s='a b  c'; echo ${s[(w)2]} ${s[(w)-1]}; t=a:b:c; echo ${t[(ws.:.)2]}");
  EXPECT_EQ(r.out, "b c\nb\n");
}

TEST(Subst13b, SubscriptOnAssociation) {
  Shell sh;
  auto r = sh.run("typeset -A h; h=(k1 v1 k2 v2 x vx); "
                  "print -l \"i:${h[(i)v*]}\" \"ik:${h[(i)k*]}\" \"k:${h[(k)k1]}\" "
                  "\"K:${h[(K)k*]}\"; print -l ${(o)h[(R)v*]} ${(o)h[(I)k*]}");
  EXPECT_EQ(r.out, "i:\nik:k1\nk:v1\nK:\nv1\nv2\nvx\nk1\nk2\n");
}

TEST(Subst13b, SubscriptKeysAsPatterns) {
  Shell sh;
  auto r = sh.run("typeset -A h; h=('a*' 1 'b?' 2); echo ${h[(k)abc]} ${h[(k)bz]} ${(o)h[(K)b?]}");
  EXPECT_EQ(r.out, "1 2 2\n");
}

// ---------------------------------------------------------------------------
// Padding.
// ---------------------------------------------------------------------------

TEST(Subst13b, PadLeftAndRightTruncate) {
  Shell sh;
  auto r = sh.run("x=abc; print -r -- \"${(l:5:)x}|${(r:5:)x}|${(l:2:)x}|${(r:2:)x}\"");
  EXPECT_EQ(r.out, "  abc|abc  |bc|ab\n");
}

TEST(Subst13b, PadWithFillAndOnceStrings) {
  Shell sh;
  auto r = sh.run("x=abc; print -r -- \"${(l:5::-::+:)x}|${(r:5::-::+:)x}\"; "
                  "x=a; print -r -- \"${(l:6::12::xyz:)x}|${(r:6::12::xyz:)x}|"
                  "${(l:3::12::xyz:)x}|${(r:3::12::xyz:)x}\"");
  EXPECT_EQ(r.out, "-+abc|abc+-\n12xyza|axyz12|yza|axy\n");
}

TEST(Subst13b, PadMultiCharFillAlignment) {
  Shell sh;
  auto r = sh.run("x=a; print -r -- \"${(l:6::ab:)x}|${(r:6::ab:)x}|${(l:4::abc:)x}|${(r:4::abc:)x}\"");
  EXPECT_EQ(r.out, "bababa|aababa|abca|aabc\n");
}

TEST(Subst13b, PadWidthIsArithmeticAndDelimitersVary) {
  Shell sh;
  auto r = sh.run("x=abc; n=4; print -r -- \"${(l:n+1:)x}\" \"${(l.5.)x}\" \"${(l<5><->)x}\"");
  EXPECT_EQ(r.out, "  abc   abc --abc\n");
}

TEST(Subst13b, PadBothSidesCentres) {
  Shell sh;
  auto r = sh.run("x=abc; print -r -- \"${(l:4:r:4:)x}|\"; x=ab; print -r -- \"${(l:7::-:r:7::+:)x}|\"");
  EXPECT_EQ(r.out, "   abc  |\n------ab++++++|\n");
}

TEST(Subst13b, PadArrayPerElementOrJoinedInQuotes) {
  Shell sh;
  auto r = sh.run("a=(foo bar); print -r -- \"${(l:6:)a}|\" ${(l:2:)a}");
  EXPECT_EQ(r.out, "oo bar| oo ar\n");
}

// ---------------------------------------------------------------------------
// (e) and (%).
// ---------------------------------------------------------------------------

TEST(Subst13b, EFlagReExpandsKeepingQuotes) {
  Shell sh;
  auto r = sh.run("y=world; x='hello $y $((1+2)) \"q\"'; print -r -- ${(e)x}");
  EXPECT_EQ(r.out, "hello world 3 \"q\"\n");
}

TEST(Subst13b, EFlagRunsCommandSubstitutionPerElement) {
  Shell sh;
  auto r = sh.run("x='$(echo hi)'; print -r -- ${(e)x}; a=('$y' b); y=Y; print -l ${(e)a}");
  EXPECT_EQ(r.out, "hi\nY\nb\n");
}

TEST(Subst13b, PercentFlagExpandsPromptEscapes) {
  Shell sh;
  TempDir dir;
  auto r = sh.run("x='a%%b'; print -r -- ${(%)x}; (HOME=" + dir.path.string()
                  + "; cd ~; x='%~'; print -r -- ${(%)x} ${(%%)x})");
  EXPECT_EQ(r.out, "a%b\n~ ~\n");
}

// ---------------------------------------------------------------------------
// :c and :P modifiers.
// ---------------------------------------------------------------------------

TEST(Subst13b, CModifierFindsCommandPath) {
  Shell sh;
  auto r = sh.run("x=ls; [[ ${x:c} == /*/ls ]] && echo ok; y=nosuchcmd_xyz; echo ${y:c}");
  EXPECT_EQ(r.out, "ok\nnosuchcmd_xyz\n");
}

TEST(Subst13b, PModifierResolvesPath) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  auto r = sh.run("mkdir " + d + "/real; ln -s real " + d + "/link; x=" + d
                  + "/link/../link/.; [[ ${x:P} == $(cd " + d + "/real; pwd -P) ]] && echo ok");
  EXPECT_EQ(r.out, "ok\n");
}

// ---------------------------------------------------------------------------
// ${x:-word}: unquoted pattern characters in `word` glob.
// ---------------------------------------------------------------------------

TEST(Subst13b, DefaultWordGlobs) {
  Shell sh;
  TempDir dir;
  auto r = sh.run("(cd " + dir.path.string() + "; touch a.md b.md; x=; "
                  "echo ${x:-*.md}; echo ${x:-\"*.md\"}; echo \"${x:-*.md}\"; "
                  "z=1; echo ${z:+*.md})");
  EXPECT_EQ(r.out, "a.md b.md\n*.md\n*.md\na.md b.md\n");
}

TEST(Subst13b, DefaultWordParameterStaysLiteral) {
  Shell sh;
  TempDir dir;
  auto r = sh.run("(cd " + dir.path.string() + "; touch a.md; x=; y='*.md'; "
                  "echo ${x:-$y}; echo ${~x:-$y})");
  EXPECT_EQ(r.out, "*.md\na.md\n");
}

TEST(Subst13b, DefaultWordKeepsQuotedEmptyAndSplits) {
  Shell sh;
  auto r = sh.run("x=; echo ${x:-a b} end; print -l ${x:-''} end; print -l ${x:-\"a b\"}");
  EXPECT_EQ(r.out, "a b end\n\nend\na b\n");
}

// ---------------------------------------------------------------------------
// A failed ${x?} ends a non-interactive shell.
// ---------------------------------------------------------------------------

TEST(Subst13b, FailedQuestionExitsShell) {
  Shell sh;
  auto r = sh.run("echo ${x?boom}; echo after");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  EXPECT_NE(r.err.find("x: boom"), str::npos);
}

TEST(Subst13b, FailedQuestionInFunctionExitsShell) {
  Shell sh;
  auto r = sh.run("f() { echo ${x:?gone}; echo inf; }; f; echo after");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
}

TEST(Subst13b, FailedQuestionInSubshellExitsOnlySubshell) {
  Shell sh;
  auto r = sh.run("( echo ${x?boom} ); echo after $?");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "after 1\n");
}

}  // namespace jfsh::test
