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

// Known gap: glob.cpp's comment promises [...] ranges but pattern_match treats '['
// literally. Enable once bracket expressions are implemented (TODO glob/extglob).
TEST(GlobPatternMatch, DISABLED_BracketExpressions) {
  EXPECT_TRUE(pattern_match("file[0-9].txt", "file7.txt"));
  EXPECT_FALSE(pattern_match("file[0-9].txt", "fileX.txt"));
  EXPECT_TRUE(pattern_match("[!a]*", "bcd"));
}

TEST(GlobHasGlobChars, DetectsUnescapedMetacharacters) {
  EXPECT_TRUE(has_glob_chars("*.cpp"));
  EXPECT_TRUE(has_glob_chars("a?c"));
  EXPECT_TRUE(has_glob_chars("[ab]"));
  EXPECT_FALSE(has_glob_chars("plain/path.txt"));
  EXPECT_FALSE(has_glob_chars("escaped\\*star"));
  EXPECT_FALSE(has_glob_chars(""));
}

}  // namespace
}  // namespace jfsh
