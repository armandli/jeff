#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/glob/glob.h>

namespace jfsh {
namespace {

TEST(GlobPatternMatch, LiteralsAndWildcards) {
  EXPECT_TRUE(pattern_match("main.cpp", "main.cpp"));
  EXPECT_FALSE(pattern_match("main.cpp", "main.h"));
  EXPECT_TRUE(pattern_match("*.cpp", "main.cpp"));
  EXPECT_TRUE(pattern_match("*.cpp", ".cpp"));
  EXPECT_FALSE(pattern_match("*.cpp", "main.cpp.bak"));
  EXPECT_TRUE(pattern_match("m??n.*", "main.cpp"));
  EXPECT_FALSE(pattern_match("?", ""));
  EXPECT_TRUE(pattern_match("*", ""));
  EXPECT_TRUE(pattern_match("a*b*c", "aXXbYYc"));
  EXPECT_FALSE(pattern_match("a*b*c", "aXXbYY"));
}

TEST(GlobPatternMatch, CaseInsensitive) {
  EXPECT_FALSE(pattern_match("README*", "readme.md"));
  EXPECT_TRUE(pattern_match("README*", "readme.md", /*case_insensitive=*/true));
}

TEST(GlobPatternMatch, BracketExpressions) {
  EXPECT_TRUE(pattern_match("file[0-9].txt", "file7.txt"));
  EXPECT_FALSE(pattern_match("file[0-9].txt", "fileX.txt"));
  EXPECT_TRUE(pattern_match("[!a]*", "bcd"));
  EXPECT_TRUE(pattern_match("[^a]*", "bcd"));
  EXPECT_FALSE(pattern_match("[!a]*", "abc"));
  EXPECT_TRUE(pattern_match("[]x]", "]"));
  EXPECT_TRUE(pattern_match("[[:digit:][:upper:]]?", "Qz"));
  EXPECT_FALSE(pattern_match("[[:digit:]]", "a"));
  EXPECT_TRUE(pattern_match("[A-Z]", "q", /*case_insensitive=*/true));
}

TEST(GlobPatternMatch, EscapesAreLiteral) {
  EXPECT_TRUE(pattern_match("a\\*", "a*"));
  EXPECT_FALSE(pattern_match("a\\*", "ab"));
  EXPECT_TRUE(pattern_match("\\[x]", "[x]"));
}

TEST(GlobPatternMatch, GroupsAndNumericRanges) {
  EXPECT_TRUE(pattern_match("*.(c|h)", "main.h"));
  EXPECT_FALSE(pattern_match("*.(c|h)", "main.o"));
  EXPECT_TRUE(pattern_match("(foo|ba(r|z))", "baz"));
  EXPECT_TRUE(pattern_match("v<1-10>", "v7"));
  EXPECT_FALSE(pattern_match("v<1-10>", "v11"));
  EXPECT_TRUE(pattern_match("v<->", "v123"));
  EXPECT_TRUE(pattern_match("x<5->y", "x50y"));
}

TEST(GlobPatternMatch, ExtendedOperators) {
  EXPECT_TRUE(pattern_match("a#b", "aaab", false, true));
  EXPECT_TRUE(pattern_match("a#b", "b", false, true));
  EXPECT_FALSE(pattern_match("a##b", "b", false, true));
  EXPECT_TRUE(pattern_match("(ab)##", "ababab", false, true));
  EXPECT_TRUE(pattern_match("^*.o", "main.c", false, true));
  EXPECT_FALSE(pattern_match("^*.o", "main.o", false, true));
  EXPECT_TRUE(pattern_match("*.c~test*", "main.c", false, true));
  EXPECT_FALSE(pattern_match("*.c~test*", "test.c", false, true));
  // Without EXTENDED_GLOB the operators are ordinary characters.
  EXPECT_TRUE(pattern_match("a#b", "a#b"));
}

TEST(GlobPatternMatch, PrefixMatches) {
  auto p = CompiledPattern::compile("*/");
  EXPECT_EQ(p.match_prefix("a/b/c", false), 2u);
  EXPECT_EQ(p.match_prefix("a/b/c", true), 4u);
  EXPECT_EQ(p.match_prefix("abc", true), str::npos);
}

TEST(GlobPatternMatch, LongSubjectsAndPatterns) {
  // The matcher keeps a set of positions instead of recursing per
  // character, so neither length reaches the stack.
  str x(200000, 'a');
  EXPECT_TRUE(pattern_match("a##", x, false, true));
  EXPECT_TRUE(pattern_match(str(200000, '?'), x));
  EXPECT_FALSE(pattern_match(str(200001, '?'), x));
  EXPECT_TRUE(pattern_match("*a*a*a*b#", x, false, true));
  EXPECT_FALSE(pattern_match("*b*", x));
}

TEST(GlobPatternMatch, RepeatsAndGroups) {
  EXPECT_TRUE(pattern_match("x(a|aa)#y", "xaaay", false, true));
  EXPECT_FALSE(pattern_match("(abc)##", "abcab", false, true));
  EXPECT_TRUE(pattern_match("(a|)##b", "aab", false, true));
  EXPECT_TRUE(pattern_match("foo<->bar", "foo12bar"));
  EXPECT_FALSE(pattern_match("<1-20>", "123"));
  EXPECT_TRUE(pattern_match("[]]", "]"));
  EXPECT_FALSE(pattern_match("[]", "]"));
  EXPECT_TRUE(pattern_match("[!]", "x"));
}

TEST(GlobPatternMatch, PrefixMatchesWithOperators) {
  auto star = CompiledPattern::compile("a*~*c", false, true, false);
  EXPECT_EQ(star.match_prefix("abcabc", false), 1u);
  EXPECT_EQ(star.match_prefix("abcabc", true), 5u);
  auto neg = CompiledPattern::compile("^a", false, true, false);
  EXPECT_EQ(neg.match_prefix("abc", false), 0u);
  EXPECT_EQ(neg.match_prefix("abc", true), 3u);
  auto rep = CompiledPattern::compile("b#", false, true, false);
  EXPECT_EQ(rep.match_prefix("bbx", true), 2u);
  EXPECT_EQ(rep.match_prefix("bbx", false), 0u);
}

TEST(GlobPatternMatch, MultibyteCharacters) {
  // "é" is two bytes of UTF-8.
  auto any = CompiledPattern::compile("?", false, false, true);
  EXPECT_TRUE(any.matches("\xc3\xa9"));
  EXPECT_FALSE(CompiledPattern::compile("?", false, false, false).matches("\xc3\xa9"));
  EXPECT_TRUE(CompiledPattern::compile("[\xc3\xa9]x", false, false, true).matches("\xc3\xa9x"));
  EXPECT_EQ(CompiledPattern::compile("h?", false, false, true).match_prefix("h\xc3\xa9llo", false), 3u);
}

TEST(GlobCollate, NumericRunsAndTies) {
  EXPECT_LT(collate("x2", "x10", true), 0);
  EXPECT_GT(collate("x2", "x10", false), 0);
  EXPECT_LT(collate("a001", "a01", true), 0);
  EXPECT_LT(collate("9", "10", true), 0);
  EXPECT_EQ(collate("a", "a", true), 0);
}

TEST(GlobHasGlobChars, DetectsUnescapedMetacharacters) {
  EXPECT_TRUE(has_glob_chars("*.cpp"));
  EXPECT_TRUE(has_glob_chars("a?c"));
  EXPECT_TRUE(has_glob_chars("[ab]"));
  EXPECT_FALSE(has_glob_chars("plain/path.txt"));
  EXPECT_FALSE(has_glob_chars("escaped\\*star"));
  EXPECT_FALSE(has_glob_chars(""));
  EXPECT_FALSE(has_glob_chars("["));          // the test command
  EXPECT_TRUE(has_glob_chars("[]"));          // an empty class, as in zsh
  EXPECT_FALSE(has_glob_chars("a#b"));
  EXPECT_TRUE(has_glob_chars("a#b", /*extended=*/true));
  EXPECT_TRUE(has_glob_chars("f<1-3>"));
  EXPECT_FALSE(has_glob_chars("a<b"));
}

TEST(GlobUnescape, RemovesBackslashes) {
  EXPECT_EQ(glob_unescape("a\\*b\\\\c"), "a*b\\c");
}

}  // namespace
}  // namespace jfsh
