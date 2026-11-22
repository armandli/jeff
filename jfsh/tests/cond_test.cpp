// Conditional expressions (M5): `[[ … ]]` and the `test` / `[` builtin, run
// end to end through Executor::exec_string.  The expected output is what
// zsh 5.9 prints (`zsh -f`).

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <shell_fixture.h>

namespace jfsh {
namespace {

using test::Shell;
using test::TempDir;
namespace sf = s::filesystem;

str out(Shell& sh, strv src) { return sh.run(src).out; }

// The output of src with its newlines removed.
str joined(Shell& sh, strv src) {
  str s = out(sh, src);
  s::erase(s, '\n');
  return s;
}

// "T" or "F" for each condition, run one after another.
str truth(Shell& sh, s::initializer_list<strv> conds) {
  str src;
  for (auto c : conds) src += str(c) + " && echo T || echo F; ";
  return joined(sh, src);
}

// A scratch directory with a file `f`, an empty file `e`, a directory `d`
// and a symlink `l` to `f`, which the shell runs in.
struct Files {
  TempDir  dir;
  Shell    sh;
  sf::path old = sf::current_path();
  Files() {
    dir.file("f", "data", sf::perms::owner_read | sf::perms::owner_write);
    dir.file("e", "", sf::perms::owner_all);
    sf::create_directory(dir.path / "d");
    sf::create_symlink(dir.path / "f", dir.path / "l");
    sf::current_path(dir.path);
  }
  ~Files() { sf::current_path(old); }
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
  EXPECT_NE(r.err.find("failed to compile regex"), str::npos);
}

TEST(Cond, NumericComparisonsAreArithmetic) {
  Shell sh;
  sh.run("n=5");
  EXPECT_EQ(truth(sh, {"[[ 1+1 -eq 2 ]]", "[[ n -gt 3 ]]", "[[ 2.5 -lt 3 ]]", "[[ 10 -le 9 ]]",
                       "[[ 0x10 -eq 16 ]]", "[[ 3 -eq 3.0 ]]", "[[ $n -ne 5 ]]", "[[ 4 -ge 4 ]]"}),
            "TTTFTTFT");
  // The error ends the line; the failed side counts as 0, so the status
  // is that of 0 -eq 1 … in zsh too.
  auto r = sh.run("x='a b'; [[ x -eq 0 ]]; echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "");
  EXPECT_NE(r.err.find("operator expected at `b'"), str::npos);
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
str statuses(Shell& sh, s::initializer_list<strv> cmds) {
  str src;
  for (auto c : cmds) src += str(c) + "; echo $?; ";
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

// ---- M14: =~ under REMATCH_PCRE and BASH_REMATCH, [[ -n ]], xtrace --------

TEST(CondM14, RegexSetsMatchArrays) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=abc; [[ $x =~ '(b)(c)' ]] && echo $MATCH $match $MBEGIN $MEND $mbegin $mend"),
            "bc b c 2 3 2 3 2 3\n");
}

TEST(CondM14, BashRematchLeavesMatchAlone) {
  Shell sh;
  EXPECT_EQ(out(sh, "MATCH=old; setopt bashrematch; [[ abc =~ '(b)(x)?' ]] && "
                    "print -l -- \"${BASH_REMATCH[@]}\" \"M=$MATCH\" \"m=$match\" ${#BASH_REMATCH}"),
            "b\nb\n\nM=old\nm=\n3\n");
}

TEST(CondM14, BashRematchUntouchedOnFailure) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt bashrematch; BASH_REMATCH=(q); [[ abc =~ z ]]; echo $? $BASH_REMATCH"),
            "1 q\n");
}

bool have_pcre(Shell& sh) {
  return sh.run("zmodload zsh/pcre && pcre_compile x").rc == 0;
}

TEST(CondM14, RematchPcreUsesPcre) {
  Shell sh;
  if (not have_pcre(sh)) GTEST_SKIP() << "built without PCRE2";
  Shell fresh;
  // The option loads zsh/pcre itself.
  EXPECT_EQ(out(fresh, "setopt rematchpcre; [[ abc =~ 'b(?=c)' ]] && echo $MATCH $MBEGIN $MEND"),
            "b 2 2\n");
  EXPECT_EQ(out(fresh, "setopt rematchpcre; [[ abc =~ '(b)(c)' ]] && echo $match $mbegin $mend"),
            "b c 2 3 2 3\n");
  EXPECT_EQ(out(fresh, "setopt rematchpcre bashrematch; [[ abc =~ '(b)(c)' ]] && echo $BASH_REMATCH"),
            "bc b c\n");
}

TEST(CondM14, RematchPcreBadPatternIsFalse) {
  Shell sh;
  if (not have_pcre(sh)) GTEST_SKIP() << "built without PCRE2";
  auto r = sh.run("setopt rematchpcre; [[ abc =~ '(b' ]]; echo $?");
  EXPECT_EQ(r.out, "1\n");
}

TEST(CondM14, LoneUnaryOperatorIsUnknownCondition) {
  Shell sh;
  auto r = sh.run("echo a; [[ -n ]]; echo b");
  EXPECT_EQ(r.out, "a\n");
  EXPECT_EQ(r.rc, 2);
  EXPECT_NE(r.err.find("unknown condition: -n"), str::npos);
  EXPECT_EQ(sh.run("[[ -n || 1 ]]").rc, 2);
}

TEST(CondM14, QuotedOrExpandedDashWordIsAString) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=-n; [[ $x ]]; echo $?; [[ \"-n\" ]]; echo $?; [[ -n -n ]]; echo $?"),
            "0\n0\n0\n");
}

TEST(CondM14, XtraceShowsEvaluatedCondition) {
  Shell sh;
  auto r = sh.run("x=1 y='a b'; PS4='+> '; setopt xtrace; "
                  "[[ $x == 1 && -n $y ]]; [[ ! -z $x || $y = a* ]]; [[ 1 == 2 && -n x ]]");
  EXPECT_EQ(r.err, "+> [[ 1 == 1 && -n 'a b' ]]\n+> [[ ! -z 1 ]]\n+> [[ 1 == 2 ]]\n");
}

TEST(CondM14, XtraceShowsArithmetic) {
  Shell sh;
  auto r = sh.run("x=1; PS4='+> '; setopt xtrace; (( x + $x ))");
  EXPECT_EQ(r.err, "+> ((  x + 1  ))\n");
}

// ---- M5 review --------------------------------------------------------------

// "rc|out|err" of src.
str all(Shell& sh, strv src) {
  auto r = sh.run(src);
  return s::to_string(r.rc) + "|" + r.out + "|" + r.err;
}

TEST(CondM5Review, WordCountsAsZshParsesThem) {
  Shell sh;
  // par_cond_double, par_cond_triple and par_cond_multi's messages.
  EXPECT_EQ(all(sh, "[[ x = ]]"), "1||jfsh:1: parse error: condition expected: x\n");
  EXPECT_EQ(all(sh, "[[ x y z ]]"), "1||jfsh:1: condition expected: y\n");
  EXPECT_EQ(all(sh, "[[ x = y z ]]"), "1||jfsh:1: condition expected: x\n");
  EXPECT_EQ(all(sh, "[[ 1 -eq 1 -eq 1 ]]"), "1||jfsh:1: condition expected: 1\n");
  EXPECT_EQ(all(sh, "[[ a = a -o b = b ]]"), "1||jfsh:1: condition expected: a\n");
  // `-x a b…` is a module's condition: unknown when run.
  EXPECT_EQ(all(sh, "[[ -n x -a -n y ]]"), "2||jfsh: unknown condition: -n\n");
  EXPECT_EQ(all(sh, "[[ -n x y ]]"), "2||jfsh: unknown condition: -n\n");
  EXPECT_EQ(all(sh, "[[ -n = ]] && echo t"), "0|t\n|");
  // Only a two-character `-x` alone is a condition missing its operand.
  EXPECT_EQ(all(sh, "[[ -foo ]] && echo t"), "0|t\n|");
  EXPECT_EQ(all(sh, "[[ -1 ]]"), "2||jfsh: unknown condition: -1\n");
  // Newlines may come between any of the words.
  EXPECT_EQ(out(sh, "[[ 3 -eq\n3 ]] && [[ a ==\nb || -n\nx ]] && echo t"), "t\n");
}

TEST(CondM5Review, ErrorsEndTheCommandLine) {
  Shell sh;
  // Status 2 is a shell error, as in zsh's execcond.
  EXPECT_EQ(all(sh, "echo a; [[ -q x ]]; echo $?"), "2|a\n|jfsh: unknown condition: -q\n");
  EXPECT_EQ(all(sh, "[[ -q x ]] || echo no"), "2||jfsh: unknown condition: -q\n");
  EXPECT_EQ(all(sh, "[[ a -pcre-match b ]]"), "2||jfsh: unknown condition: -pcre-match\n");
  EXPECT_EQ(all(sh, "[[ -o nosuch ]]; echo $?"), "0|3\n|jfsh: no such option: nosuch\n");
  // An arithmetic error too, the failed side counting as 0.
  EXPECT_EQ(all(sh, "[[ x -eq 1/0 ]] || echo no"), "0||jfsh: division by zero\n");
  EXPECT_EQ(all(sh, "[[ 1 -eq 1/0 ]] || echo no"), "1||jfsh: division by zero\n");
}

TEST(CondM5Review, NumericOperandsAreExpandedWords) {
  Shell sh;
  EXPECT_EQ(truth(sh, {"[[ 3 -eq '1 + 2' ]]", "[[ \"1+1\" -lt 3 ]]", "x=' 3 '; [[ $x -eq 3 ]]"}),
            "TTT");
  EXPECT_EQ(all(sh, "[[ 3 -eq '1 +' ]]; echo after"),
            "1||jfsh: bad math expression: operand expected at end of string\n");
}

TEST(CondM5Review, UnaryTests) {
  Shell sh;
  EXPECT_EQ(truth(sh, {"a=(1 2); [[ -v a[2] ]]", "[[ -v a[3] ]]", "[[ -v a[-1] ]]",
                       "typeset -A h; h[k]=1; [[ -v h[k] ]]", "[[ -v h[z] ]]",
                       "set -- a; [[ -v 1 ]]", "[[ -v 2 ]]"}),
            "TFTTFTF");
  // -o takes an option letter; -t an arithmetic descriptor.
  EXPECT_EQ(truth(sh, {"[[ -o e ]]", "set -e; [[ -o e ]]", "set +e; [[ -t 1+1000 ]]"}), "FTF");
  EXPECT_EQ(all(sh, "[[ -o @ ]]; echo $?"), "0|3\n|jfsh: no such option: @\n");
  sh.opts.set(OPT_POSIXBUILTINS);
  EXPECT_EQ(all(sh, "[[ -o nosuch ]]; echo $?"), "0|1\n|");
}

TEST(TestM5Review, ParsedAsZshDoes) {
  Shell sh;
  EXPECT_EQ(all(sh, "test x y"), "2||jfsh: parse error: condition expected: x\n");
  EXPECT_EQ(all(sh, "test x y z"), "2||jfsh: condition expected: y\n");
  EXPECT_EQ(all(sh, "test x y z w"), "2||jfsh: condition expected: x\n");
  EXPECT_EQ(all(sh, "test '(' x y"), "2||jfsh: parse error: condition expected: x\n");
  EXPECT_EQ(all(sh, "test -q x y"), "2||jfsh: test: unknown condition: -q\n");
  EXPECT_EQ(all(sh, "test '(' x"), "2||jfsh: test: argument expected\n");
  EXPECT_EQ(all(sh, "test '(' ')'"), "2||jfsh: test: argument expected\n");
  EXPECT_EQ(all(sh, "test x ')'"), "2||jfsh: test: too many arguments\n");
  EXPECT_EQ(all(sh, "test -a x"), "2||jfsh: test: too many arguments\n");
  // A missing last operand is an empty string.
  EXPECT_EQ(statuses(sh, {"test x -a", "[ x -a ]", "test 1 -eq 1 -a", "test x -o"}), "1110");
  // ksh's `[ -t ]` is `[ -t 1 ]`, whose output here is no terminal.
  EXPECT_EQ(statuses(sh, {"test -t", "test -n -t"}), "10");
  // POSIX 2024, where zsh 5.9 says "too many arguments" / "condition
  // expected": kept on purpose.
  EXPECT_EQ(statuses(sh, {"test -o errexit", "test a '<' b", "test b '>' a"}), "100");
}

TEST(TestM5Review, Integers) {
  Shell sh;
  // zstrtol: blanks and a sign in front, nothing after; empty is 0.
  EXPECT_EQ(statuses(sh, {"test 1 -eq ''", "test ' +1' -eq 1", "test -0 -eq 0"}), "100");
  EXPECT_EQ(all(sh, "test ' 1 ' -eq 1"), "2||jfsh: test: integer expression expected:  1 \n");
  EXPECT_EQ(all(sh, "test 99999999999999999999 -gt 1; echo $?"),
            "0|1\n|jfsh: number truncated after 19 digits: 99999999999999999999\n");
}

TEST(TestM5Review, DeepArgumentListsDontCrash) {
  test::on_stack(1 << 20, [] {
    Shell sh;
    str bangs, ors = "test x", parens = "test";
    for (int k = 0; k < 20000; ++k) bangs += " !";
    for (int k = 0; k < 20000; ++k) ors += " -o x -a x";
    EXPECT_EQ(sh.run("test" + bangs + " x").rc, 0);
    EXPECT_EQ(sh.run(ors).rc, 0);
    for (int k = 0; k < 200000; ++k) parens += " '('";
    parens += " x";
    for (int k = 0; k < 200000; ++k) parens += " ')'";
    auto r = sh.run(parens);
    EXPECT_EQ(r.rc, 2);
    EXPECT_EQ(r.err, "jfsh: test: condition nested too deeply\n");
  });
}

// ---- M5 review, round 2 ------------------------------------------------------

TEST(CondM5Review2, NumericOperands) {
  Shell sh;
  // A flagged subscript in an operand, as in $(( )).
  EXPECT_EQ(truth(sh, {"a=(x y); [[ a[(i)y] -eq 2 ]]"}), "T");
  // NaN is unequal to everything.
  EXPECT_EQ(truth(sh, {"[[ NaN -eq NaN ]]", "[[ NaN -ne NaN ]]", "[[ NaN -le NaN ]]",
                       "[[ NaN -ge 1 ]]", "[[ NaN -lt 1 ]]"}),
            "FTFFF");
}

TEST(CondM5Review2, ErrorsInEvalAndLoops) {
  Shell sh;
  // An error ends just the eval, which gives its status.
  EXPECT_EQ(all(sh, "eval '[[ -q x ]]; echo ineval'; echo $?; echo end"),
            "0|2\nend\n|jfsh: unknown condition: -q\n");
  EXPECT_EQ(all(sh, "eval 'echo $(( 1/0 )); echo in'; echo $?"),
            "0|1\n|jfsh: division by zero\n");
  // A loop an error ended has status 1.
  EXPECT_EQ(sh.run("for i in 1 2; do [[ -q x ]]; done").rc, 1);
  EXPECT_EQ(sh.run("while true; do [[ -q x ]]; done").rc, 1);
  EXPECT_EQ(sh.run("f() { for i in 1; do [[ -q x ]]; done; }; f").rc, 1);
  EXPECT_EQ(sh.run("{ [[ -q x ]] }").rc, 2);
}

TEST(CondM5Review2, ParseErrors) {
  Shell sh;
  EXPECT_EQ(all(sh, "[[ - ]]"), "1||jfsh:1: condition expected: -\n");
  EXPECT_EQ(all(sh, "[[ x ! ]]"), "1||jfsh:1: parse error near `!'\n");
}

// ---- M5 review, round 3 ------------------------------------------------------

TEST(CondM5Review3, LoopStatusAfterAnError) {
  Shell sh;
  // zsh's execwhile: an error in `while`'s condition keeps the last body's
  // status; `until` and the bodies give 1.
  EXPECT_EQ(sh.run("while [[ -q x ]]; do :; done").rc, 0);
  EXPECT_EQ(sh.run("i=0; while [[ $((i++)) -lt 1 || -q x ]]; do :; done").rc, 0);
  EXPECT_EQ(sh.run("until [[ -q x ]]; do :; done").rc, 1);
  EXPECT_EQ(sh.run("until false; do [[ -q x ]]; done").rc, 1);
  EXPECT_EQ(sh.run("for ((i = 0; i < 1/0; i++)); do :; done").rc, 1);
  EXPECT_EQ(sh.run("for ((i = 1/0; i < 1; i++)); do :; done").rc, 1);
  EXPECT_EQ(sh.run("for ((i = 0; i < 2; i += 1/0)); do :; done").rc, 1);
  EXPECT_EQ(sh.run("repeat 1/0 echo x").rc, 1);
  EXPECT_EQ(sh.run("repeat 2 [[ -q x ]]").rc, 1);
  // select keeps the 2.
  EXPECT_EQ(sh.run("select x in a; do [[ -q x ]]; done <<< 1").rc, 2);
}

TEST(CondM5Review3, HardErrorsInEvalEndTheLine) {
  Shell sh;
  sh.opts.set(OPT_INTERACTIVE);
  // `${x?}` is zsh's hard error: it ends more than the eval …
  EXPECT_EQ(all(sh, "eval \"echo \\${x?boom}\"; echo after"), "1||jfsh: x: boom\n");
  // … where another error ends just the eval, in the same session.
  EXPECT_EQ(all(sh, "eval '[[ -q x ]]'; echo after=$?"),
            "0|after=2\n|jfsh: unknown condition: -q\n");
}

}  // namespace
}  // namespace jfsh
