// Conditional expressions (M5): `[[ … ]]` and the `test` / `[` builtin, run
// end to end through Executor::exec_string.  The expected output is what
// zsh 5.9 prints (`zsh -f`).

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/exec/executor.h>

#include "shell_fixture.h"

namespace jfsh {
namespace {

using test::Shell;
using test::TempDir;
namespace fs = std::filesystem;

std::string out(Shell& sh, std::string_view src) { return sh.run(src).out; }

// The output of src with its newlines removed.
std::string joined(Shell& sh, const std::string& src) {
  std::string s = out(sh, src);
  std::erase(s, '\n');
  return s;
}

// "T" or "F" for each condition, run one after another.
std::string truth(Shell& sh, std::initializer_list<std::string_view> conds) {
  std::string src;
  for (auto c : conds) src += std::string(c) + " && echo T || echo F; ";
  return joined(sh, src);
}

// A scratch directory with a file `f`, an empty file `e`, a directory `d`
// and a symlink `l` to `f`, which the shell runs in.
struct Files {
  TempDir  dir;
  Shell    sh;
  fs::path old = fs::current_path();
  Files() {
    dir.file("f", "data", fs::perms::owner_read | fs::perms::owner_write);
    dir.file("e", "", fs::perms::owner_all);
    fs::create_directory(dir.path / "d");
    fs::create_symlink(dir.path / "f", dir.path / "l");
    fs::current_path(dir.path);
  }
  ~Files() { fs::current_path(old); }
};

// ---- [[ ]] strings and patterns ---------------------------------------------

TEST(Cond, StringTests) {
  Shell sh;
  sh.run("x='a b'; e=''");
  EXPECT_EQ(truth(sh, {"[[ -n $x ]]", "[[ -z $e ]]", "[[ $x ]]", "[[ $e ]]", "[[ -z $nope ]]"}),
            "TTTFT");
  // Operands are not split or globbed.
  EXPECT_EQ(truth(sh, {"[[ $x = a\\ b ]]", "[[ \"$x\" == \"a b\" ]]", "[[ * == \\* ]]"}), "TTT");
  EXPECT_EQ(truth(sh, {"[[ b < c ]]", "[[ b > c ]]", "[[ \"a\" < \"b\" ]]"}), "TFT");
}

TEST(Cond, RightSideOfEqualsIsAPattern) {
  Shell sh;
  sh.run("x='a b'; y='a*'");
  EXPECT_EQ(truth(sh, {"[[ $x == a* ]]", "[[ $x == \"a*\" ]]", "[[ $x != b* ]]"}), "TFT");
  EXPECT_EQ(truth(sh, {"[[ abc == (a|x)bc ]]", "[[ abc == a(b|x)c ]]", "[[ a.c == *.(c|h) ]]",
                       "[[ a.x == *.(c|h) ]]"}),
            "TTTF");
  EXPECT_EQ(truth(sh, {"[[ 5 == <1-10> ]]", "[[ ab == a? ]]", "[[ a == [[:alpha:]] ]]"}), "TTT");
  // A parameter's value is literal, unless ${~x} makes it a pattern.
  EXPECT_EQ(truth(sh, {"[[ abc = $y ]]", "[[ abc = $~y ]]", "[[ abc = ${~y} ]]"}), "FTT");
  EXPECT_EQ(truth(sh, {"[[ ab == a${z:-b} ]]", "[[ a == $(echo a) ]]",
                       "b=a; [[ ab == \"$b\"* ]]", "b='*'; [[ abc == \"$b\" ]]"}),
            "TTTF");
}

TEST(Cond, RegexSetsMatchParameters) {
  Shell sh;
  EXPECT_EQ(out(sh, "[[ foobar =~ o+(b)a ]]; echo $? $MATCH $match[1] $MBEGIN $MEND $mbegin $mend"),
            "0 ooba b 2 5 4 4\n");
  // A group that took no part is empty, at -1.
  EXPECT_EQ(out(sh, "[[ foo =~ (o)(x)? ]]; echo ${#match} :$match[2]: $MBEGIN $MEND \"$mbegin\""),
            "2 :: 2 2 2 -1\n");
  // A failed match leaves the parameters alone.
  EXPECT_EQ(out(sh, "[[ foo =~ x ]]; echo $? $MATCH"), "1 o\n");
  EXPECT_EQ(truth(sh, {"[[ foo =~ \"o+\" ]]", "[[ o+ =~ o\\+ ]]", "x=123; [[ $x =~ ^[0-9]+$ ]]",
                       "[[ a =~ [[:alpha:]] ]]"}),
            "TTTT");
  // Offsets count characters, not bytes.
  EXPECT_EQ(out(sh, "[[ héllo =~ l+ ]]; echo $MBEGIN $MEND $MATCH"), "3 4 ll\n");
  auto r = sh.run("[[ foo =~ \"(\" ]]");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("failed to compile regex"), std::string::npos);
}

TEST(Cond, NumericComparisonsAreArithmetic) {
  Shell sh;
  sh.run("n=5");
  EXPECT_EQ(truth(sh, {"[[ 1+1 -eq 2 ]]", "[[ n -gt 3 ]]", "[[ 2.5 -lt 3 ]]", "[[ 10 -le 9 ]]",
                       "[[ 0x10 -eq 16 ]]", "[[ 3 -eq 3.0 ]]", "[[ $n -ne 5 ]]", "[[ 4 -ge 4 ]]"}),
            "TTTFTTFT");
  auto r = sh.run("x='a b'; [[ x -eq 1 ]]");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("operator expected at `b'"), std::string::npos);
}

TEST(Cond, LogicShortCircuitsAndGroups) {
  Shell sh;
  EXPECT_EQ(truth(sh, {"[[ -n a && -z \"\" ]]", "[[ -n \"\" || -n a ]]", "[[ ! -n \"\" ]]",
                       "[[ ( -n a || -n b ) && -z c ]]", "[[ ! -z a && -z a ]]",
                       "[[ a == b && b == b || c == c ]]", "[[ a == b || c == d && e == e ]]",
                       "[[ ! ( -n a ) ]]", "[[ 2 -eq 2 && ( 3 -gt 4 || 5 -lt 6 ) ]]"}),
            "TTTFFTFFT");
  // The right side only runs when it decides the result.
  EXPECT_EQ(out(sh, "[[ -n a || $(echo ran >&2) == x ]]; [[ -z a && $(echo ran2 >&2) == x ]]; echo ok"),
            "ok\n");
  EXPECT_EQ(sh.run("[[ -n a || $(echo ran >&2) == x ]]").err, "");
  EXPECT_EQ(out(sh, "[[ a == b ||\n  c == c ]] && echo multi"), "multi\n");
}

TEST(Cond, WorksAsACommandAnywhere) {
  Shell sh;
  EXPECT_EQ(out(sh, "i=0; while [[ $i -lt 3 ]]; do (( i++ )); done; echo $i"), "3\n");
  EXPECT_EQ(out(sh, "f() { [[ -n $1 ]] && echo \"got $1\" || echo none; }; f x; f"),
            "got x\nnone\n");
  EXPECT_EQ(out(sh, "! [[ a == b ]]; echo $?; [[ a = b ]] || [[ c = c ]]; echo $?"), "0\n0\n");
  EXPECT_EQ(out(sh, "echo $( [[ a == a ]] && echo inner )"), "inner\n");
  EXPECT_EQ(out(sh, "[[ a == b ]] 2>/dev/null || echo redir"), "redir\n");
  // Reserved words are operands inside [[ ]].
  EXPECT_EQ(truth(sh, {"[[ -n if ]]", "[[ done == done ]]"}), "TT");
}

TEST(Cond, UnknownConditionsAndOptions) {
  Shell sh;
  auto r = sh.run("[[ -q x ]]");
  EXPECT_EQ(r.rc, 2);
  EXPECT_EQ(r.err, "jfsh: unknown condition: -q\n");
  EXPECT_EQ(sh.run("[[ a -foo b ]]").rc, 2);
  r = sh.run("[[ -o nosuch ]]");
  EXPECT_EQ(r.rc, 3);
  EXPECT_EQ(r.err, "jfsh: no such option: nosuch\n");
  sh.opts.set(OPT_PIPEFAIL);
  sh.opts.unset(OPT_NOMATCH);
  EXPECT_EQ(truth(sh, {"[[ -o pipefail ]]", "[[ -o PIPE_FAIL ]]", "[[ -o nomatch ]]",
                       "[[ -o nonomatch ]]", "[[ -o no_pipe_fail ]]"}),
            "TTFTF");
  EXPECT_EQ(truth(sh, {"x=1; [[ -v x ]]", "[[ -v nope ]]", "typeset -A h; [[ -v h ]]"}), "TFT");
}

// ---- [[ ]] files --------------------------------------------------------------

TEST(Cond, FileTests) {
  Files fx;
  Shell& sh = fx.sh;
  EXPECT_EQ(truth(sh, {"[[ -f f ]]", "[[ -d d ]]", "[[ -e nope ]]", "[[ -L l ]]", "[[ -h l ]]",
                       "[[ -h f ]]", "[[ -a f ]]", "[[ -f d ]]"}),
            "TTFTTFTF");
  EXPECT_EQ(truth(sh, {"[[ -s f ]]", "[[ -s e ]]", "[[ -r f && -w f && ! -x f ]]", "[[ -x e ]]",
                       "[[ -O f ]]", "[[ -c /dev/null ]]", "[[ -p f ]]", "[[ -S f ]]"}),
            "TFTTTTFF");
  EXPECT_EQ(truth(sh, {"[[ f -ef l ]]", "[[ f -ef e ]]", "[[ f -nt nope ]]", "[[ nope -ot f ]]"}),
            "TFFF");
  sh.run("touch -t 202001010000 e");
  EXPECT_EQ(truth(sh, {"[[ f -nt e ]]", "[[ e -ot f ]]", "[[ e -nt f ]]"}), "TTF");
  EXPECT_EQ(truth(sh, {"[[ -t 99 ]]", "[[ -t x ]]"}), "FF");
}

// ---- test / [ -----------------------------------------------------------------

// The status of each command, run one after another.
std::string statuses(Shell& sh, std::initializer_list<std::string_view> cmds) {
  std::string src;
  for (auto c : cmds) src += std::string(c) + "; echo $?; ";
  return joined(sh, src);
}

TEST(Test, ArgumentCountRules) {
  Shell sh;
  EXPECT_EQ(statuses(sh, {"test", "test ''", "test x", "test -n", "test -z", "test !", "test '('"}),
            "1100000");
  EXPECT_EQ(statuses(sh, {"test ! x", "test ! ''", "test -n ''", "test -z ''"}), "1010");
  // With three arguments a binary operator in the middle wins.
  EXPECT_EQ(statuses(sh, {"test = = =", "test -n = -n", "test ! = !", "test x -a ''",
                          "test '' -o ''", "test '(' a ')'"}),
            "000110");
  EXPECT_EQ(statuses(sh, {"test ! a = b", "test ! '(' a = a ')'", "test '(' a = b ')'"}), "011");
}

TEST(Test, StringsNumbersAndLogic) {
  Shell sh;
  EXPECT_EQ(statuses(sh, {"test a = a", "test a '==' a", "test a != a", "test 1 -eq 1",
                          "test ' 2' -eq 2", "test 5 -gt 3", "test -5 -lt 3"}),
            "0010000");
  EXPECT_EQ(statuses(sh, {"test -n a -a -z ''", "test -n '' -o -n a",
                          "test a = a -o a = b -a b = c", "test ! -n a -o -n b"}),
            "0000");
  EXPECT_EQ(statuses(sh, {"[ a = a ]", "[ ]", "[ ! ]", "[ -n x -a '(' a = b -o 1 -lt 2 ')' ]"}),
            "0100");
}

TEST(Test, Errors) {
  Shell sh;
  auto r = sh.run("test 1 -eq x");
  EXPECT_EQ(r.rc, 2);
  EXPECT_EQ(r.err, "jfsh: test: integer expression expected: x\n");
  EXPECT_EQ(sh.run("test 1+1 -eq 2").rc, 2);
  EXPECT_EQ(sh.run("test 2.5 -lt 3").rc, 2);
  r = sh.run("[ a = a");
  EXPECT_EQ(r.rc, 2);
  EXPECT_EQ(r.err, "jfsh: [: ']' expected\n");
  r = sh.run("test -q x");
  EXPECT_EQ(r.rc, 2);
  EXPECT_EQ(r.err, "jfsh: test: unknown condition: -q\n");
  EXPECT_EQ(sh.run("test a b").rc, 2);
  // Bug B19: test used to succeed whatever it was given.
  EXPECT_EQ(out(sh, "[ -f /no/such/file ] && echo wrong || echo right"), "right\n");
}

TEST(Test, Files) {
  Files fx;
  EXPECT_EQ(statuses(fx.sh, {"test -f f", "test ! -f f", "[ -d d ]", "test -e nope", "test -L l",
                             "test -s e", "test f -ef l"}),
            "0101010");
}

}  // namespace
}  // namespace jfsh
