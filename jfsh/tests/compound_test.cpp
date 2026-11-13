// Compound commands and functions (M4): if, loops, case, select, repeat,
// subshells, brace groups, always blocks, (( )), function definitions and
// calls, local/typeset scoping and read, run end to end through
// Executor::exec_string.  The expected output is what zsh prints.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/exec/executor.h>

#include "shell_fixture.h"

namespace jfsh {
namespace {

using test::Shell;
using test::TempDir;

std::string out(Shell& sh, std::string_view src) { return sh.run(src).out; }

// ---- if ---------------------------------------------------------------------

TEST(Compound, IfTakesTheFirstBranchThatSucceeds) {
  Shell sh;
  EXPECT_EQ(out(sh, "if true; then echo a; fi"), "a\n");
  EXPECT_EQ(out(sh, "if false; then echo 1; elif true; then echo 2; else echo 3; fi"), "2\n");
  EXPECT_EQ(out(sh, "if false; then echo 1; elif false; then echo 2; else echo 3; fi"), "3\n");
  EXPECT_EQ(out(sh, "if\n  false\nthen\n  echo 1\nfi\necho after"), "after\n");
}

TEST(Compound, IfStatus) {
  Shell sh;
  EXPECT_EQ(sh.run("if false; then :; fi").rc, 0);
  EXPECT_EQ(sh.run("if true; then false; fi").rc, 1);
  EXPECT_EQ(out(sh, "if false; then :; fi; echo $?"), "0\n");
}

// ---- while / until ----------------------------------------------------------

TEST(Compound, WhileAndUntilLoops) {
  Shell sh;
  EXPECT_EQ(out(sh, "n=0; while (( n < 3 )); do echo $n; (( n++ )); done"), "0\n1\n2\n");
  EXPECT_EQ(out(sh, "until (( n == 0 )); do n=$((n-1)); done; echo $n"), "0\n");
  // A body that never runs leaves status 0.
  EXPECT_EQ(out(sh, "while false; do :; done; echo $?"), "0\n");
}

// ---- for --------------------------------------------------------------------

TEST(Compound, ForLoops) {
  Shell sh;
  EXPECT_EQ(out(sh, "for x in a b c; do echo $x; done"), "a\nb\nc\n");
  EXPECT_EQ(out(sh, "for x (p q) echo $x"), "p\nq\n");
  EXPECT_EQ(out(sh, "for i j in 1 2 3; do echo $i-$j; done"), "1-2\n3-\n");
  EXPECT_EQ(out(sh, "for x in {1..3} *.no-such(N); do :; done; echo $x"), "3\n");
  EXPECT_EQ(out(sh, "for x in in do done; do echo $x; done"), "in\ndo\ndone\n");
  EXPECT_EQ(out(sh, "for x in; do echo no; done; echo $?"), "0\n");
}

TEST(Compound, ForWithoutAListLoopsOverThePositionalParameters) {
  Shell sh;
  sh.ctx.positional = {"a", "b"};
  EXPECT_EQ(out(sh, "for x; do echo $x; done"), "a\nb\n");
  EXPECT_EQ(out(sh, "for x\ndo\necho $x\ndone"), "a\nb\n");
}

TEST(Compound, ArithmeticFor) {
  Shell sh;
  EXPECT_EQ(out(sh, "for ((i=0; i<3; i++)) echo $i"), "0\n1\n2\n");
  EXPECT_EQ(out(sh, "for ((i=0, j=10; i<2; i++, j--)); do echo $i:$j; done"),
            "0:10\n1:9\n");
  EXPECT_EQ(out(sh, "n=2; for ((i=0; i<$n; i++)) { echo x }"), "x\nx\n");
  // An empty condition is true; break ends it.
  EXPECT_EQ(out(sh, "for ((i=0;;i++)) { (( i == 2 )) && break; echo $i }"), "0\n1\n");
}

TEST(Compound, Repeat) {
  Shell sh;
  EXPECT_EQ(out(sh, "repeat 3 echo r"), "r\nr\nr\n");
  EXPECT_EQ(out(sh, "n=2; repeat $n+1; do echo x; done"), "x\nx\nx\n");
}

// ---- break / continue (bug B18) ----------------------------------------------

TEST(Compound, BreakAndContinue) {
  Shell sh;
  EXPECT_EQ(out(sh, "for i in 1 2 3; do (( i == 2 )) && continue; echo $i; done"), "1\n3\n");
  EXPECT_EQ(out(sh, "for i in 1 2 3; do (( i == 2 )) && break; echo $i; done"), "1\n");
  EXPECT_EQ(out(sh, "for i in 1 2; do for j in a b; do break 2; done; echo no; done; echo out"),
            "out\n");
  EXPECT_EQ(out(sh, "for i in 1 2; do for j in a b; do (( i == 1 )) && continue 2;"
                    " echo $i$j; done; echo end$i; done"),
            "2a\n2b\nend2\n");
  // Past the outermost loop means the outermost.
  EXPECT_EQ(out(sh, "for i in 1 2; do break 5; done; echo $i"), "1\n");
  EXPECT_EQ(out(sh, "w=0; while true; do (( w++ )); (( w >= 3 )) && break; done; echo $w"),
            "3\n");
}

TEST(Compound, BreakOutsideALoopIsAnError) {
  Shell sh;
  auto r = sh.run("break");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: break: not in while, until, select, or repeat loop\n");
  EXPECT_EQ(sh.run("for i in 1; do continue 0; done").err,
            "jfsh: continue: argument is not positive: 0\n");
}

// zsh lets a function break out of its caller's loop.
TEST(Compound, BreakInsideAFunctionLeavesTheCallersLoop) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { break; }; for i in 1 2; do f; echo a$i; done; echo end"), "end\n");
}

// ---- case -------------------------------------------------------------------

TEST(Compound, CaseMatchesPatterns) {
  Shell sh;
  EXPECT_EQ(out(sh, "case foo in bar) echo 1;; f*) echo 2;; *) echo 3;; esac"), "2\n");
  EXPECT_EQ(out(sh, "case b in (a|b) echo ab;; esac"), "ab\n");
  EXPECT_EQ(out(sh, "case x.c in *.(c|h)) echo src;; esac"), "src\n");
  EXPECT_EQ(out(sh, "case \"a b\" in \"a b\") echo q;; esac"), "q\n");
  EXPECT_EQ(out(sh, "case x in\n  a) echo A ;;\n  x|y)\n    echo XY\n    ;;\nesac"), "XY\n");
  EXPECT_EQ(out(sh, "case z in a) echo no;; esac; echo $?"), "0\n");
}

TEST(Compound, CasePatternCharactersFromQuotesOrExpansionsAreLiteral) {
  Shell sh;
  EXPECT_EQ(out(sh, "case x in \"*\") echo bad;; *) echo good;; esac"), "good\n");
  EXPECT_EQ(out(sh, "v='*'; case x in $v) echo glob;; *) echo literal;; esac"), "literal\n");
  EXPECT_EQ(out(sh, "v='x'; case x in $v) echo yes;; esac"), "yes\n");
}

TEST(Compound, CaseFallThroughAndTestNext) {
  Shell sh;
  EXPECT_EQ(out(sh, "case foo in f*) echo 1;& x) echo 2;; y) echo 3;; esac"), "1\n2\n");
  EXPECT_EQ(out(sh, "case ab in a*) echo 1;| x) echo 2;; *b) echo 3;; esac"), "1\n3\n");
}

// ---- select -----------------------------------------------------------------

TEST(Compound, SelectReadsAChoice) {
  Shell sh;
  auto r = sh.run("select v in a b; do echo \"$v/$REPLY\"; break; done <<< 2");
  EXPECT_EQ(r.out, "b/2\n");
  EXPECT_EQ(r.err, "1) a  2) b  \n?# ");
  // Out of range: the name is empty.  End of input ends the loop.
  EXPECT_EQ(out(sh, "select v in a; do echo \"[$v]\"; done <<< 7"), "[]\n");
}

// ---- subshells, groups, always ------------------------------------------------

TEST(Compound, SubshellIsolatesState) {
  Shell sh;
  EXPECT_EQ(out(sh, "(a=inner; echo $a); echo \"[$a]\""), "inner\n[]\n");
  EXPECT_EQ(out(sh, "(exit 4); echo $?"), "4\n");
  EXPECT_EQ(out(sh, "(echo a; (echo b))"), "a\nb\n");
  EXPECT_EQ(out(sh, "echo $( (echo nested) )"), "nested\n");
  EXPECT_FALSE(sh.ctx.exiting);
}

TEST(Compound, BraceGroupRunsInTheCurrentShell) {
  Shell sh;
  EXPECT_EQ(out(sh, "{ a=1; echo in }; echo $a"), "in\n1\n");
  EXPECT_EQ(out(sh, "{ echo brace }"), "brace\n");
  EXPECT_EQ(out(sh, "if { true }; then echo yes; fi"), "yes\n");
}

TEST(Compound, RedirectionsApplyToTheWholeCommand) {
  Shell sh;
  TempDir dir;
  std::string f = (dir.path / "o").string();
  EXPECT_EQ(out(sh, "{ echo one; echo two } > " + f + "; cat " + f), "one\ntwo\n");
  EXPECT_EQ(out(sh, "for i in 1 2; do echo $i; done > " + f + "; cat " + f), "1\n2\n");
  EXPECT_EQ(out(sh, "{ echo err >&2 } 2>&1 | tr a-z A-Z"), "ERR\n");
  EXPECT_EQ(out(sh, "while read l; do echo \"got $l\"; done <<< $'l1\\nl2'"),
            "got l1\ngot l2\n");
  // The redirection ends with the command.
  EXPECT_EQ(out(sh, "{ echo a } > " + f + "; echo b"), "b\n");
}

TEST(Compound, CompoundCommandsInPipelines) {
  Shell sh;
  EXPECT_EQ(out(sh, "{ echo one; echo two } | wc -l | tr -d ' '"), "2\n");
  EXPECT_EQ(out(sh, "for i in 1 2; do echo $i; done | tr '\\n' ,; echo"), "1,2,\n");
  EXPECT_EQ(out(sh, "echo x | while read l; do echo \"[$l]\"; done"), "[x]\n");
  EXPECT_EQ(out(sh, "x=$(for i in a b; do echo $i; done); echo $x"), "a\nb\n");
}

TEST(Compound, AlwaysBlockRunsAfterTheTryBlock) {
  Shell sh;
  EXPECT_EQ(out(sh, "{ false } always { echo alw }; echo $?"), "alw\n1\n");
  EXPECT_EQ(out(sh, "for i in 1 2; do { break } always { echo cleanup$i }; done; echo $i"),
            "cleanup1\n1\n");
  EXPECT_EQ(out(sh, "f() { { return 3 } always { echo cleanup } }; f; echo $?"),
            "cleanup\n3\n");
}

TEST(Compound, ArithmeticCommand) {
  Shell sh;
  EXPECT_EQ(sh.run("(( 2 > 1 ))").rc, 0);
  EXPECT_EQ(sh.run("(( 0 ))").rc, 1);
  EXPECT_EQ(out(sh, "((x = 5)); echo $x"), "5\n");
  auto r = sh.run("(( 1/0 ))");
  EXPECT_EQ(r.rc, 2);
  EXPECT_NE(r.err.find("division by zero"), std::string::npos);
}

// ---- functions ----------------------------------------------------------------

TEST(Function, DefineAndCall) {
  Shell sh;
  EXPECT_EQ(out(sh, "g() { echo g:$0:$#:$@; }; g x y"), "g:g:2:x y\n");
  EXPECT_EQ(out(sh, "function k { echo kw }; k"), "kw\n");
  EXPECT_EQ(out(sh, "function m() { echo paren }; m"), "paren\n");
  EXPECT_EQ(out(sh, "p() echo short; p"), "short\n");
  EXPECT_EQ(out(sh, "a b () { echo $0 }; a; b"), "a\nb\n");
  EXPECT_EQ(out(sh, "f() {\n  echo multi\n}\nf"), "multi\n");
}

TEST(Function, ComesBeforeBuiltinsAndCommands) {
  Shell sh;
  EXPECT_EQ(sh.run("true() { return 5; }; true").rc, 5);
  EXPECT_EQ(out(sh, "ls() { echo mine; }; ls"), "mine\n");
}

TEST(Function, PositionalParametersArePerCall) {
  Shell sh;
  sh.ctx.positional = {"outer"};
  EXPECT_EQ(out(sh, "f() { echo $1; }; f inner; echo $1 $0"), "inner\nouter jfsh\n");
}

TEST(Function, ReturnStatus) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { return 3; echo no; }; f; echo $?"), "3\n");
  EXPECT_EQ(out(sh, "f() { false; return; }; f; echo $?"), "1\n");
  EXPECT_EQ(out(sh, "f() { for i in 1 2; do return $i; done; }; f; echo $?"), "1\n");
  // At the top level `return` ends the program.
  auto r = sh.run("return 5; echo no");
  EXPECT_EQ(r.rc, 5);
  EXPECT_EQ(r.out, "");
  EXPECT_FALSE(sh.ctx.returning);
}

TEST(Function, Recursion) {
  Shell sh;
  EXPECT_EQ(out(sh, "fact() { if (( $1 <= 1 )); then echo 1; else"
                    " echo $(( $1 * $(fact $(( $1 - 1 ))) )); fi; }; fact 5"),
            "120\n");
  EXPECT_EQ(out(sh, "rec() { (( $1 > 0 )) || return 0; echo d$1; rec $(( $1 - 1 )); }; rec 2"),
            "d2\nd1\n");
}

TEST(Function, NestingIsLimitedByFuncnest) {
  Shell sh;
  auto r = sh.run("FUNCNEST=20; r() { r; }; r");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: r: maximum nested function level reached; increase FUNCNEST?\n");
  EXPECT_EQ(sh.ctx.func_depth, 0);
}

TEST(Function, AnonymousFunctionRunsAtOnce) {
  Shell sh;
  EXPECT_EQ(out(sh, "() { echo anon $1 $0 } arg"), "anon arg (anon)\n");
  EXPECT_EQ(out(sh, "() { local t=1; echo $t }; echo \"[$t]\""), "1\n[]\n");
}

TEST(Function, RedirectionsAndPrefixAssignments) {
  Shell sh;
  TempDir dir;
  std::string f = (dir.path / "o").string();
  EXPECT_EQ(out(sh, "fr() { echo to-file; }; fr > " + f + "; cat " + f), "to-file\n");
  EXPECT_EQ(out(sh, "X=pre; px() { echo $X; }; X=tmp px; echo $X"), "tmp\npre\n");
  // …and they are exported to what the function runs.
  EXPECT_EQ(out(sh, "pe() { env | grep '^XQ='; }; XQ=1 pe"), "XQ=1\n");
}

TEST(Function, BodyOutlivesTheDefiningProgram) {
  Shell sh;
  sh.run("keep() { echo kept $1; }");
  EXPECT_EQ(out(sh, "keep 1"), "kept 1\n");
  sh.run("keep() { echo redefined; }");
  EXPECT_EQ(out(sh, "keep"), "redefined\n");
  EXPECT_EQ(out(sh, "self() { self() { echo second; }; echo first; }; self; self"),
            "first\nsecond\n");
}

// ---- local / typeset -----------------------------------------------------------

TEST(Typeset, LocalIsScopedToTheFunction) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=1; f() { local x=2; echo $x; }; f; echo $x"), "2\n1\n");
  // A new local is set and empty.
  EXPECT_EQ(out(sh, "f() { local y; echo \"${+y}[$y]\"; }; f"), "1[]\n");
  EXPECT_EQ(out(sh, "f() { local v=outer; g() { echo $v; }; g; }; f"), "outer\n");
  // Assignment inside a function to an undeclared name is global.
  EXPECT_EQ(out(sh, "f() { glob=set; }; f; echo $glob"), "set\n");
}

TEST(Typeset, ValuesAreNotSplit) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { local s=$(echo a b) t=x; echo $s; echo $t; }; f"), "a b\nx\n");
  EXPECT_EQ(out(sh, "f() { local a=(1 2 3); echo $#a $a[2]; }; f"), "3 2\n");
  EXPECT_EQ(out(sh, "f() { local -a arr; arr=(one two); echo ${#arr} $arr[2]; }; f"),
            "2 two\n");
}

TEST(Typeset, Attributes) {
  Shell sh;
  EXPECT_EQ(out(sh, "integer i=3+4; echo $i"), "7\n");
  EXPECT_EQ(out(sh, "typeset -u up=abc; echo $up"), "ABC\n");
  EXPECT_EQ(out(sh, "export EX=5; env | grep '^EX='"), "EX=5\n");
  auto r = sh.run("readonly RO=1; RO=2");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: read-only variable: RO\n");
  EXPECT_EQ(sh.get("RO"), "1");
  EXPECT_EQ(sh.run("typeset -Q x").err, "jfsh: typeset: bad option: -Q\n");
  EXPECT_EQ(sh.run("local 1x=2").err, "jfsh: local: not valid in this context: 1x\n");
}

TEST(Typeset, GlobalFlagUpdatesTheOuterParameter) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { typeset -g G=in; }; f; echo $G"), "in\n");
}

// ---- read -------------------------------------------------------------------

TEST(Read, SplitsALineIntoNames) {
  Shell sh;
  EXPECT_EQ(out(sh, "read a b <<< 'x y  z '; echo \"[$a][$b]\""), "[x][y  z]\n");
  EXPECT_EQ(out(sh, "read <<< '  whole line '; echo \"[$REPLY]\""), "[  whole line ]\n");
  EXPECT_EQ(out(sh, "read -A arr <<< 'p q r'; echo $#arr $arr[3]"), "3 r\n");
  EXPECT_EQ(out(sh, "read a <<< 'x\\ y'; echo $a"), "x y\n");
  EXPECT_EQ(out(sh, "read -r a <<< 'x\\ y'; echo $a"), "x\\ y\n");
  EXPECT_EQ(out(sh, "IFS=: read a b <<< 'p:q'; echo $a $b"), "p q\n");
  EXPECT_EQ(sh.run("read a < /dev/null").rc, 1);
}

// ---- multi-line programs ------------------------------------------------------

TEST(Compound, MultiLineScript) {
  Shell sh;
  EXPECT_EQ(out(sh,
                "sum() {\n"
                "  local t=0\n"
                "  for n in \"$@\"; do\n"
                "    (( t += n ))\n"
                "  done\n"
                "  echo $t\n"
                "}\n"
                "for i in 1 2 3; do\n"
                "  if (( i == 2 )); then\n"
                "    continue\n"
                "  fi\n"
                "  sum $i 10\n"
                "done\n"),
            "11\n13\n");
}

}  // namespace
}  // namespace jfsh
