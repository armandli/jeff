#include <chrono>
#include <clocale>
#include <random>

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
  // As in zsh, case-insensitive matching (NO_CASE_GLOB, (#i)) leaves
  // brackets alone.
  EXPECT_FALSE(pattern_match("[A-Z]", "q", /*case_insensitive=*/true));
  EXPECT_TRUE(pattern_match("[^a]", "A", /*case_insensitive=*/true));
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

TEST(GlobPatternMatch, BackreferencesWithMultibyteAndLongSubjects) {
  // (#b) groups count bytes, but `?` takes a whole character.
  auto pat = CompiledPattern::compile("(#b)(?)(*)", false, true, true);
  PatternMatch pm;
  ASSERT_TRUE(pat.matches("\xc3\xa9t\xc3\xa9", &pm));
  ASSERT_EQ(pm.groups.size(), 2u);
  EXPECT_EQ(pm.groups[0], (s::pair<s::size_t, s::size_t>{0, 2}));
  EXPECT_EQ(pm.groups[1], (s::pair<s::size_t, s::size_t>{2, 5}));
  // A subject that cannot match is refused without backtracking, which
  // would take quadratic time here, and one far too long for the stack to
  // backtrack through fails cleanly or still matches.
  str big(200000, 'a');
  EXPECT_FALSE(CompiledPattern::compile("(#b)(*)(*)c", false, true).matches(big));
  EXPECT_FALSE(CompiledPattern::compile("(#b)(a#)(a#)c", false, true).matches(big));
  EXPECT_TRUE(CompiledPattern::compile("(#b)(a#)", false, true).matches(big));
  EXPECT_EQ(CompiledPattern::compile("(#b)(a#)b", false, true).match_prefix(big + "b", true),
            big.size() + 1);
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

// M13 Phase 13c.1: zsh 5.9's semantics; expected values are zsh's.

// A UTF-8 LC_CTYPE for one test, for towlower() and friends.
struct Utf8Ctype {
  Utf8Ctype() : saved{s::setlocale(LC_CTYPE, nullptr)} {
    for (const char* name : {"en_US.UTF-8", "C.UTF-8", "en_US.utf8"})
      if (s::setlocale(LC_CTYPE, name)) { ok = true; break; }
  }
  ~Utf8Ctype() { s::setlocale(LC_CTYPE, saved.c_str()); }

  str  saved;
  bool ok = false;
};

CompiledPattern ext(strv p, bool mb = false) {
  return CompiledPattern::compile(p, false, true, mb);
}

using Span = s::pair<s::size_t, s::size_t>;

TEST(GlobPattern13c, CaretAnywhere) {
  EXPECT_TRUE(ext("a^x").matches("ab"));
  EXPECT_TRUE(ext("*^b").matches("ab"));
  EXPECT_TRUE(ext("(#b)^x").matches("ab"));
  EXPECT_TRUE(ext("(#i)^x").matches("ab"));
  EXPECT_FALSE(ext("(#i)^x").matches("X"));
  // `^` negates up to the next `|`, `)` or `~`.
  auto first = ext("^a|b");
  EXPECT_FALSE(first.matches("a"));
  EXPECT_TRUE(first.matches("b"));
  EXPECT_TRUE(first.matches("c"));
  auto second = ext("a|^b");
  EXPECT_TRUE(second.matches("a"));
  EXPECT_FALSE(second.matches("b"));
  EXPECT_TRUE(second.matches("c"));
  for (strv x : {"a", "b", "c"}) EXPECT_FALSE(ext("a~^b").matches(x)) << x;
  EXPECT_TRUE(ext("^").matches("x"));
  EXPECT_FALSE(ext("^").matches(""));
  EXPECT_TRUE(ext("^^a").matches("a"));
  EXPECT_FALSE(ext("^^a").matches("b"));
  EXPECT_EQ(ext("a^b").match_prefix("ab", false), 1u);
  // Without EXTENDED_GLOB it is a character.
  EXPECT_TRUE(CompiledPattern::compile("a^x").matches("a^x"));
}

TEST(GlobPattern13c, FirstFoundEnd) {
  // The longest form is where zsh's greedy search first gets to the end
  // of the pattern; the shortest keeps its meaning.
  EXPECT_EQ(ext("(a|aa)").match_prefix("aaa", true), 1u);
  EXPECT_EQ(ext("(aa|a)").match_prefix("aaa", true), 2u);
  EXPECT_EQ(ext("(a|ab)").match_prefix("abc", true), 1u);
  EXPECT_EQ(ext("*(a|ab)").match_prefix("abab", true), 3u);
  EXPECT_EQ(ext("*(a|ab)").match_prefix("abab", false), 1u);
  EXPECT_EQ(ext("(a|aa)#").match_prefix("aaab", true), 3u);
  EXPECT_EQ(ext("x").match_prefix("abc", true), str::npos);
  PatternMatch pm;
  EXPECT_EQ(ext("(#b)(a|ab)(b|)").match_prefix("abab", true, &pm), 2u);
  EXPECT_EQ(pm.groups, (vec<Span>{{0, 1}, {1, 2}}));
  EXPECT_EQ(ext("(#b)(*)(b|bc)").match_prefix("abcabc", true, &pm), 5u);
  EXPECT_EQ(pm.groups, (vec<Span>{{0, 4}, {4, 5}}));
  // From a position, with (#s) and (#e) at the ends of the whole text,
  // which `notstart` and `notend` turn off.
  EXPECT_EQ(ext("(#s)a").match_prefix("aa", 1, true), str::npos);
  EXPECT_EQ(ext("a(#e)|a").match_prefix("aa", 1, true), 1u);
  PrefixSearch notend{false, true}, notstart{true, false};
  EXPECT_EQ(ext("(#e)").match_prefix("aa", 2, true, nullptr, &notend), str::npos);
  EXPECT_EQ(ext("(#s)").match_prefix("aa", 0, true, nullptr, &notstart), str::npos);
}

TEST(GlobPattern13c, BadPatterns) {
  for (strv p : {"(#X)ab", "(#c2)a", "(#a)ac", "(#a255)a", "#a", "a###", "*#", "*##", "*(#c2)",
                 "a(#cx)", "a(#c2", "a#(#c2)", "a(#i)(#c2)", "(#ic2)a", "(#se)a", "(#s)#",
                 "(#i", "^#", "a|#", "a|(#X)", "a~(#X)", "(#i)(#X)a"}) {
    auto pat = ext(p);
    EXPECT_TRUE(pat.bad()) << p;
    EXPECT_FALSE(pat.matches("a")) << p;
    EXPECT_EQ(pat.match_prefix("ab", true), str::npos) << p;
  }
  for (strv p : {"(#a254)a", "(#)a", "(#q.)a", "a(#c,2)", "(#a 1)a", "(#ia1)a", "(#s)a"}) {
    auto pat = ext(p);
    EXPECT_FALSE(pat.bad()) << p;
    EXPECT_TRUE(pat.matches("a")) << p;
  }
  // Only EXTENDED_GLOB has globbing flags.
  EXPECT_FALSE(CompiledPattern::compile("(#X)ab").bad());
}

TEST(GlobPattern13c, CaseFlags) {
  Utf8Ctype loc;
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  // "\xc3\x89" is É, "\xc3\xa9" é.
  EXPECT_TRUE(ext("(#i)\xc3\xa9", true).matches("\xc3\x89"));
  EXPECT_TRUE(ext("(#i)\xc3\x89", true).matches("\xc3\xa9"));
  EXPECT_TRUE(ext("(#l)\xc3\xa9", true).matches("\xc3\x89"));
  EXPECT_FALSE(ext("(#l)\xc3\x89", true).matches("\xc3\xa9"));
  EXPECT_TRUE(ext("(#i)\xc3\xa9#", true).matches("\xc3\x89\xc3\x89"));
  EXPECT_FALSE(ext("(#i)[a]").matches("A"));
  EXPECT_FALSE(ext("(#i)[a-z]").matches("A"));
  EXPECT_FALSE(ext("(#l)[A]").matches("a"));
  EXPECT_FALSE(ext("(#i)[\xc3\xa9]", true).matches("\xc3\x89"));
  EXPECT_TRUE(ext("(#i)[^a]").matches("A"));
  EXPECT_FALSE(CompiledPattern::compile("[a]*", true).matches("ABD"));
  EXPECT_TRUE(CompiledPattern::compile("a*", true).matches("ABD"));
}

TEST(GlobPattern13c, EmptyRepeats) {
  PatternMatch pm;
  ASSERT_TRUE(ext("(#b)(|a)##b").matches("b", &pm));
  EXPECT_EQ(pm.groups, (vec<Span>{{0, 0}}));
  ASSERT_TRUE(ext("(#b)a(x|)#").matches("a", &pm));
  EXPECT_EQ(pm.groups, (vec<Span>{{1, 1}}));
  ASSERT_TRUE(ext("(#b)(a|b|)#").matches("ab", &pm));
  EXPECT_EQ(pm.groups, (vec<Span>{{2, 2}}));
  ASSERT_TRUE(ext("(#b)(|a)(#c2,3)b").matches("ab", &pm));
  EXPECT_EQ(pm.groups, (vec<Span>{{1, 1}}));
  EXPECT_TRUE(ext("(#a1)(|a)##b").matches("b"));
  EXPECT_EQ(ext("(|a##)").match_prefix("aaa", true), 0u);
}

TEST(GlobPattern13c, CountedRepeatsAgree) {
  // The VM alone accepts these; zsh's counter, which the backtracker
  // copies, does not, and the backtracker decides.
  EXPECT_FALSE(ext("(?|cb)(#c1,2)").matches("cba"));
  EXPECT_FALSE(ext("(#b)(?|cb)(#c1,2)").matches("cba"));
  EXPECT_EQ(ext("(?|cb)(#c1,2)").match_prefix("cba", true), 2u);
  EXPECT_EQ(ext("(?|cb)(#c1,2)").match_prefix("cba", false), 1u);
  EXPECT_FALSE(ext("(b(a|ba)#)(#c2)").matches("babaa"));
  PatternMatch pm;
  ASSERT_TRUE(ext("(#b)(*b(a|ba)#)(#c2)").matches("babaa", &pm));
  EXPECT_EQ(pm.groups, (vec<Span>{{1, 5}, {4, 5}}));
}

TEST(GlobPattern13c, NineGroups) {
  PatternMatch pm;
  ASSERT_TRUE(ext("(#b)(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)").matches("abcdefghijk", &pm));
  ASSERT_EQ(pm.groups.size(), 9u);
  EXPECT_EQ(pm.groups[8], (Span{8, 9}));
  EXPECT_TRUE(ext("(#b)(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)").backrefs());
}

TEST(GlobPattern13c, BytesAndCharacters) {
  // é is \xc3\xa9; a byte of it alone in the pattern is no character.
  EXPECT_FALSE(ext("(#b)\xc3(*)\xa9", true).matches("\xc3\xa9"));
  EXPECT_FALSE(ext("(|\xc3)\xa9", true).matches("\xc3\xa9"));
  EXPECT_TRUE(ext("\xc3\xa9", true).matches("\xc3\xa9"));
  // zsh's `*` (or x#) before literal text that ends a branch compares that
  // text with the last bytes of a whole match.
  EXPECT_TRUE(ext("(|\xc3)?#\xa9", true).matches("\xc3\xa9"));
  EXPECT_TRUE(ext("*\xa9", true).matches("\xc3\xa9"));
  EXPECT_TRUE(ext("x|*\xa9", true).matches("\xc3\xa9"));
  EXPECT_FALSE(ext("(*\xa9)", true).matches("\xc3\xa9"));
  EXPECT_FALSE(ext("*\xa9~x", true).matches("\xc3\xa9"));
  EXPECT_EQ(ext("*\xa9", true).match_prefix("\xc3\xa9", true), str::npos);
  // Without MULTIBYTE everything is bytes.
  EXPECT_TRUE(ext("(|\xc3)\xa9", false).matches("\xc3\xa9"));
}

TEST(GlobPattern13c, LongSubjects) {
  // One-character repeats run in a loop, so their groups are found on
  // subjects far longer than the backtracker could recurse through.
  str big(200000, 'a');
  PatternMatch pm;
  ASSERT_TRUE(ext("(#b)(a#)").matches(big, &pm));
  EXPECT_EQ(pm.groups, (vec<Span>{{0, big.size()}}));
  EXPECT_EQ(ext("a#").match_prefix(big, true), big.size());
  // The VM, on which the backtracker of 13c.1 gave up.
  EXPECT_EQ(ext("(a|b)#").match_prefix(big, true), big.size());
  str some(2000, 'a');
  EXPECT_EQ(ext("*a*a*a*c|a*").match_prefix(some, true), some.size());
}

// Policy: exclusions follow the documented set semantics (see
// Pattern13c.ExclusionsFollowSetSemantics); zsh fails here.
TEST(GlobPattern13c, ExclusionsFollowSetSemantics) {
  EXPECT_TRUE(ext("(b|ba)~b").matches("ba"));
  EXPECT_TRUE(ext("(ba|b)~b").matches("ba"));
}

// M13 Phase 13c.2: the VM against the backtracker, which copies zsh's
// matcher, on random patterns and subjects.

// Random patterns in the style of the differential tester against zsh.
// Inside `^` and `~` there are no repeats: there the VM follows the set
// semantics of the policy (Pattern13c.ExclusionsFollowSetSemantics),
// and the backtracker, sharing its repeats' marks with the rest of the
// match, does not quite.
struct PatternGen {
  int pick(int n) { return s::uniform_int_distribution<int>(0, n - 1)(rng); }
  double unit() { return s::uniform_real_distribution<double>(0, 1)(rng); }

  str atom(int depth) {
    double r = unit();
    if (depth > 2) r *= 0.6;
    if (r < 0.28) return str(1, "ab"[pick(2)]);
    if (r < 0.36) return "*";
    if (r < 0.42) return "?";
    if (r < 0.46) return pick(2) ? "[ab]" : "[^a]";
    if (r < 0.49) return pick(2) ? "(#s)" : "(#e)";
    if (r < 0.53) return str(1, "ab"[pick(2)]) + (inner ? "" : pick(2) ? "#" : "##");
    if (r < 0.55) return pick(2) ? "<1-12>" : "<->";
    if (r < 0.57 and cases) return pick(2) ? "(#i)A" : "B(#I)";
    int n = 1 + pick(3);
    str g = "(";
    for (int a = 0; a < n; ++a) {
      bool neg = excl and unit() < 0.2;
      str alt = (neg ? "^" : "") + nested(depth + 1, neg);
      if (excl and unit() < 0.15 and not alt.empty()) alt += "~" + nested(depth + 1, true);
      g += (a ? "|" : "") + alt;
    }
    g += ")";
    if (inner or not loops) return g;
    double q = unit();
    if (q < 0.2) g += "#";
    else if (q < 0.3) g += "##";
    else if (q < 0.4 and counts)
      g += "(#c" + s::to_string(pick(3)) + "," + s::to_string(1 + pick(3)) + ")";
    return g;
  }
  str seq(int depth) {
    str q;
    for (int n = pick(4); n > 0; --n) q += atom(depth);
    return q;
  }
  // A branch, inside `^` or `~` when `in`.
  str nested(int depth, bool in) {
    inner += in;
    str q = seq(depth);
    inner -= in;
    return q;
  }
  str subject() {
    str x;
    for (int n = pick(length + 1); n > 0; --n) x += "abAB1"[pick(5)];
    return x;
  }

  s::mt19937 rng;
  bool       excl   = false;
  bool       counts = false;
  bool       cases  = false;
  bool       loops  = true;   // repeats of groups
  int        inner  = 0;
  int        length = 6;      // of subjects, at most
  bool       groups = true;   // the groups asked for
};

// Every way of matching, by `pat` and by the backtracker, without the
// groups (where the VM caches its steps as a DFA's); cases where the
// backtracker gives up are left out.
void agree_bare(const CompiledPattern& pat, strv x, const str& what) {
  auto bt = pat.engine(PatternEngine::Backtrack);
  PrefixSearch gave;
  bt.match_prefix(x, 0, true, nullptr, &gave);
  bt.match_prefix(x, 0, false, nullptr, &gave);
  if (gave.gave_up) return;
  ASSERT_EQ(pat.matches(x), bt.matches(x)) << what;
  for (bool longest : {true, false})
    ASSERT_EQ(pat.match_prefix(x, longest), bt.match_prefix(x, longest)) << what << " " << longest;
}

// Every way of matching, by `pat` and by the backtracker; cases where the
// backtracker gives up are left out.
void agree(const CompiledPattern& pat, strv x, const str& what) {
  auto bt = pat.engine(PatternEngine::Backtrack);
  PatternMatch a, b;
  PrefixSearch gave;
  bt.match_prefix(x, 0, true, nullptr, &gave);
  bt.match_prefix(x, 0, false, nullptr, &gave);
  if (gave.gave_up) return;
  bool m = pat.matches(x, &a);
  ASSERT_EQ(m, bt.matches(x, &b)) << what;
  if (m) ASSERT_EQ(a.groups, b.groups) << what;
  for (bool longest : {true, false})
    for (s::size_t from : {s::size_t{0}, s::size_t{1}}) {
      if (from > x.size()) continue;
      PrefixSearch s1, s2;
      s::size_t e = pat.match_prefix(x, from, longest, &a, &s1);
      ASSERT_EQ(e, bt.match_prefix(x, from, longest, &b, &s2)) << what << " " << longest << from;
      if (e != str::npos) ASSERT_EQ(a.groups, b.groups) << what << " " << longest << from;
    }
  // A window, with (#e) still at the end of all of x.
  if (not x.empty()) {
    bool w = pat.matches(x, 0, x.size() - 1, &a);
    ASSERT_EQ(w, bt.matches(x, 0, x.size() - 1, &b)) << what << " window";
    if (w) ASSERT_EQ(a.groups, b.groups) << what << " window";
  }
}

void agree_on(PatternGen gen, int patterns, PatternEngine engine = PatternEngine::Auto) {
  for (int i = 0; i < patterns; ++i) {
    str p = "(#b)" + gen.seq(0);
    if (gen.excl and gen.unit() < 0.15) p = "(#b)^" + gen.nested(0, true);
    if (gen.excl and gen.unit() < 0.15) p += "~" + gen.nested(1, true);
    auto pat = ext(p).engine(engine);
    for (int j = 0; j < 6; ++j) {
      str x = gen.subject();
      if (gen.groups) agree(pat, x, "'" + p + "' on '" + x + "'");
      else agree_bare(pat, x, "'" + p + "' on '" + x + "'");
      if (::testing::Test::HasFatalFailure()) return;
    }
  }
}

// As patterns match (with the VM, or the backtracker where zsh's quirks
// need it), against the backtracker.
TEST(GlobPattern13c2, MatchesAsTheBacktracker) {
  agree_on(PatternGen{s::mt19937{1}}, 4000);
}

TEST(GlobPattern13c2, MatchesAsTheBacktrackerWithExclusions) {
  PatternGen gen{s::mt19937{2}};
  gen.excl = true;
  agree_on(gen, 4000);
}

TEST(GlobPattern13c2, MatchesAsTheBacktrackerWithCountsAndCase) {
  PatternGen gen{s::mt19937{3}};
  gen.counts = gen.cases = true;
  agree_on(gen, 4000);
}

TEST(GlobPattern13c2, MatchesAsTheBacktrackerWithAll) {
  PatternGen gen{s::mt19937{4}};
  gen.excl = gen.counts = gen.cases = true;
  agree_on(gen, 4000);
}

// On longer subjects, without the groups: the VM's cached steps.
TEST(GlobPattern13c2, MatchesAsTheBacktrackerOnLongerSubjects) {
  PatternGen gen{s::mt19937{6}};
  gen.excl = gen.counts = true;
  gen.length = 24;
  gen.groups = false;
  agree_on(gen, 250);
}

// The same with the groups: the cached steps' threads and registers.
TEST(GlobPattern13c2, MatchesAsTheBacktrackerOnLongerSubjectsWithGroups) {
  // No `^`, `~` or counts: their state registers keep steps from being
  // cached.
  PatternGen gen{s::mt19937{7}};
  gen.length = 40;
  agree_on(gen, 600);
}

// Without repeats of groups nothing goes to the backtracker: the VM alone.
TEST(GlobPattern13c2, VmAloneAgreesWithTheBacktracker) {
  PatternGen gen{s::mt19937{5}};
  gen.excl = gen.cases = true;
  gen.loops = false;
  agree_on(gen, 4000, PatternEngine::Vm);
}

// Where zsh's quirks need the backtracker, the VM alone differs: zsh's
// marks of an inner repeat outlive the outer repeat's, and its counter
// keeps a stale start.  The expected values are zsh's.
TEST(GlobPattern13c2, QuirksTheVmLeavesToTheBacktracker) {
  PatternMatch pm;
  auto marks = ext("(#b)(((|?)#)|()a)#");
  ASSERT_TRUE(marks.matches("ba", &pm));
  EXPECT_EQ(pm.groups, (vec<Span>{{2, 2}, {2, 2}, {2, 2}, {1, 1}}));
  ASSERT_TRUE(marks.engine(PatternEngine::Vm).matches("ba", &pm));
  EXPECT_EQ(pm.groups[3], (Span{str::npos, str::npos}));
  EXPECT_FALSE(ext("(?|cb)(#c1,2)").matches("cba"));
  EXPECT_TRUE(ext("(?|cb)(#c1,2)").engine(PatternEngine::Vm).matches("cba"));
}

// How long these take, with room for a slow machine.
template<typename F>
double seconds(F f) {
  auto t = s::chrono::steady_clock::now();
  f();
  return s::chrono::duration<double>(s::chrono::steady_clock::now() - t).count();
}

TEST(GlobPattern13c2, LinearOnLongSubjects) {
  str z(200000, 'a');
  str zc = z + "c";
  PatternMatch pm;
  EXPECT_LT(seconds([&] {
              EXPECT_FALSE(ext("(#b)(*)(*)c").matches(z + "b", &pm));
              ASSERT_TRUE(ext("(#b)(*)(*)c").matches(zc, &pm));
              EXPECT_EQ(pm.groups, (vec<Span>{{0, z.size()}, {z.size(), z.size()}}));
              EXPECT_FALSE(ext("(#b)(a#)(a#)c").matches(z + "b", &pm));
              ASSERT_TRUE(ext("(#b)(a#)(a#)c").matches(zc, &pm));
              EXPECT_EQ(pm.groups, (vec<Span>{{0, z.size()}, {z.size(), z.size()}}));
              EXPECT_EQ(ext("(a|b)#").match_prefix(z, true), z.size());
              EXPECT_EQ(ext("(#b)(a|b)#").match_prefix(z, true, &pm), z.size());
              EXPECT_EQ(pm.groups, (vec<Span>{{z.size() - 1, z.size()}}));
            }),
            5.0);
  // A large count: a counter, not a set of positions per repeat.
  EXPECT_LT(seconds([&] {
              ASSERT_TRUE(ext("(#b)(a|b)(#c1,300000)").matches(z, &pm));
              EXPECT_EQ(pm.groups, (vec<Span>{{z.size() - 1, z.size()}}));
              EXPECT_FALSE(ext("(a|b)(#c1,150000)").matches(z));
              EXPECT_EQ(ext("(a|b)(#c1,150000)").match_prefix(z, true), 150000u);
            }),
            5.0);
  // Flags leaked past a count, on a chain of repeats too long for the
  // backtracker (zsh: 0).
  str leak(20000, 'a');
  EXPECT_LT(seconds([&] { EXPECT_TRUE(ext("((#i)A)(#c1,200000)B").matches(leak + "b")); }), 5.0);
}

TEST(GlobPattern13c2, PolynomialOnNestedCarets) {
  // Each `^` after a `*` is one run for all its starts.  ^*(^*b) matches
  // nothing, so this is *c (zsh agrees on short subjects).
  str w(1000, 'a');
  auto pat = ext("*(^*(^*(^*b)))c");
  EXPECT_LT(seconds([&] {
              EXPECT_FALSE(pat.matches(w + "c" + w));
              EXPECT_TRUE(pat.matches(w + "b" + w + "c"));
            }),
            10.0);
}

TEST(GlobPattern13c2, ShapesAfterStars) {
  str e(8000, 'a');
  EXPECT_LT(seconds([&] {
              // A count that cannot reach its maximum is one state.
              EXPECT_FALSE(ext("*(a)(#c1,100000)c").matches(e + "bc"));
              EXPECT_TRUE(ext("*(a)(#c1,100000)c").matches(e + "c"));
              // The starts of a context that go on alike are one.
              EXPECT_TRUE(ext("*(^*b)c").matches(e + "c"));
              EXPECT_TRUE(ext("(#b)*(^*b)c").matches(e + "c"));
              PatternMatch pm;
              ASSERT_TRUE(ext("(#b)(*)(*~*b)c").matches(e + "c", &pm));
              EXPECT_EQ(pm.groups, (vec<Span>{{0, e.size()}, {e.size(), e.size()}}));
            }),
            5.0);
  // The usual shapes, as a DFA's steps.
  str path = "b";
  while (path.size() < 200000) path += "ab/c.d";
  str z(200000, 'a');
  EXPECT_LT(seconds([&] {
              for (int i = 0; i < 20; ++i) {
                EXPECT_TRUE(ext("b*").matches(path));
                EXPECT_EQ(ext("*/").match_prefix(path, true), path.size() - 3);
                EXPECT_TRUE(ext("(*)(*)c").matches(z + "c"));
                EXPECT_TRUE(ext("a##").matches(z));
                PatternMatch pm;
                ASSERT_TRUE(ext("(#b)(*)(*)c").matches(z + "c", &pm));
                EXPECT_EQ(pm.groups, (vec<Span>{{0, z.size()}, {z.size(), z.size()}}));
              }
            }),
            5.0);
}

TEST(GlobPattern13c2, StrayContinuationBytesStayLinear) {
  Utf8Ctype loc;
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  str x;
  for (int i = 0; i < 20000; ++i) x += '\xa9';
  x += 'A';
  EXPECT_LT(seconds([&] {
              EXPECT_EQ(ext("*[[:upper:]]", true).match_prefix(x, false), x.size());
              EXPECT_EQ(ext("*", true).match_prefix(x, false), 0u);
            }),
            5.0);
}

TEST(GlobPattern13c2, PrefixSearchesStayFast) {
  // ${x//…} tries every start: each is short.
  str x;
  for (int i = 0; i < 15000; ++i) x += "ab";
  EXPECT_LT(seconds([&] {
              for (strv p : {"a", "(#b)(a)", "(a|ab)"}) {
                auto pat = ext(p);
                PatternMatch pm;
                s::size_t found = 0;
                for (s::size_t b = 0; b < x.size(); ++b)
                  if (pat.match_prefix(x, b, true, &pm) != str::npos) ++found;
                EXPECT_EQ(found, x.size() / 2) << p;
              }
              // With a character no match lacks, a start without it is no
              // run.
              auto pat = ext("(#b)(*)(*)c");
              for (s::size_t b = 0; b < x.size(); ++b)
                EXPECT_EQ(pat.match_prefix(x, b, true), str::npos);
            }),
            5.0);
}

}  // namespace
}  // namespace jfsh
