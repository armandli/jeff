#include <gtest/gtest.h>

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
  EXPECT_EQ(p.match_prefix("abc", true), std::string::npos);
}

TEST(GlobHasGlobChars, DetectsUnescapedMetacharacters) {
  EXPECT_TRUE(has_glob_chars("*.cpp"));
  EXPECT_TRUE(has_glob_chars("a?c"));
  EXPECT_TRUE(has_glob_chars("[ab]"));
  EXPECT_FALSE(has_glob_chars("plain/path.txt"));
  EXPECT_FALSE(has_glob_chars("escaped\\*star"));
  EXPECT_FALSE(has_glob_chars(""));
  EXPECT_FALSE(has_glob_chars("["));          // the test command
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
