// Compound commands and functions (M4): if, loops, case, select, repeat,
// subshells, brace groups, always blocks, (( )), function definitions and
// calls, local/typeset scoping and read, run end to end through
// Executor::exec_string.  The expected output is what zsh prints.

#include <cstdio>

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

str out(Shell& sh, strv src) { return sh.run(src).out; }

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

// As in zsh, the error ends the command line.
TEST(Compound, BreakOutsideALoopIsAnError) {
  Shell sh;
  auto r = sh.run("break; echo after");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: break: not in while, until, select, or repeat loop\n");
  r = sh.run("for i in 1 2; do continue 0; echo $i; done");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: continue: argument is not positive: 0\n");
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
  str f = (dir.path / "o").string();
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
  EXPECT_NE(r.err.find("division by zero"), str::npos);
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
  str f = (dir.path / "o").string();
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
  EXPECT_EQ(sh.run("local 1x=2").err, "jfsh: local: not an identifier: 1x\n");
}

TEST(Typeset, GlobalFlagUpdatesTheOuterParameter) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { typeset -g G=in; }; f; echo $G"), "in\n");
}

// ---- read -------------------------------------------------------------------

TEST(Read, SplitsALineIntoNames) {
  Shell sh;
  EXPECT_EQ(out(sh, "read a b <<< 'x y  z '; echo \"[$a][$b]\""), "[x][y  z]\n");
  EXPECT_EQ(out(sh, "read <<< '  whole line '; echo \"[$REPLY]\""), "[whole line]\n");
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

// ---- M14: short forms, foreach, function redirections -----------------------
// Expected output is what zsh 5.9 prints for the same text in a script file.

TEST(CompoundM14, IfShortFormWithBraces) {
  Shell sh;
  EXPECT_EQ(out(sh, "if [[ 1 == 1 ]] { echo yes }"), "yes\n");
  EXPECT_EQ(out(sh, "if (( 0 )) {\n  echo a\n}\necho z"), "z\n");
}

TEST(CompoundM14, IfShortFormElifElse) {
  Shell sh;
  EXPECT_EQ(out(sh, "if [[ 1 == 2 ]] { echo a } elif [[ 1 == 1 ]] { echo b } else { echo c }"),
            "b\n");
  EXPECT_EQ(out(sh, "if [[ 1 == 2 ]] { echo a } else { echo c }\necho z"), "c\nz\n");
}

TEST(CompoundM14, IfLongFormWithShortElif) {
  Shell sh;
  EXPECT_EQ(out(sh, "if false; then echo a; elif [[ x == x ]] { echo b }\necho z"), "b\nz\n");
}

TEST(CompoundM14, IfShortFormSingleCommand) {
  Shell sh;
  EXPECT_EQ(out(sh, "if [[ 1 == 1 ]] echo hm"), "hm\n");
  EXPECT_EQ(out(sh, "if [[ 1 == 2 ]] echo hm; echo after"), "after\n");
}

TEST(CompoundM14, BraceAfterCommandWordIsAnArgument) {
  Shell sh;
  // As in zsh: `if true {` passes `{` to true, so the `if` is incomplete.
  EXPECT_NE(sh.run("if true { echo yes }").rc, 0);
  EXPECT_EQ(out(sh, "echo {; echo { a b"), "{\n{ a b\n");
}

TEST(CompoundM14, ShortLoopForms) {
  Shell sh;
  EXPECT_EQ(out(sh, "i=0; until (( i >= 2 )) { echo $i; (( i++ )) }"), "0\n1\n");
  EXPECT_EQ(out(sh, "for x (a b) { echo $x }"), "a\nb\n");
  EXPECT_EQ(out(sh, "for x in a b; echo $x"), "a\nb\n");
  EXPECT_EQ(out(sh, "repeat 2 { echo r }"), "r\nr\n");
  EXPECT_EQ(out(sh, "for ((i=0;i<2;i++)) echo $i"), "0\n1\n");
}

TEST(CompoundM14, Foreach) {
  Shell sh;
  EXPECT_EQ(out(sh, "foreach x (a b); echo $x; end"), "a\nb\n");
  EXPECT_EQ(out(sh, "foreach x (a b)\necho $x\nend"), "a\nb\n");
  EXPECT_EQ(out(sh, "foreach x y in a b c d; echo $x-$y; end"), "a-b\nc-d\n");
}

TEST(CompoundM14, EndIsReservedInCommandPosition) {
  Shell sh;
  EXPECT_NE(sh.run("end").rc, 0);
  EXPECT_EQ(out(sh, "echo end; for x in end; do echo $x; done"), "end\nend\n");
}

TEST(CompoundM14, FunctionDefinitionRedirectionsApplyOnEachCall) {
  Shell sh;
  TempDir dir;
  str f = (dir.path / "o").string();
  EXPECT_EQ(out(sh, "g() { echo one; echo two } > " + f + "; g; g; cat " + f),
            "one\ntwo\n");
  EXPECT_EQ(out(sh, "h() { echo e >&2 } 2>&1; h 2>/dev/null"), "e\n");
}

TEST(CompoundM14, FunctionWithRedirectionsPrintsThem) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { echo body } > /dev/null; functions f"),
            "f () {\n\techo body\n} > /dev/null\n");
}

TEST(CompoundM14, EmptyBraceBodyIsAFunction) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() {}; f; echo rc=$?; whence -w f"), "rc=0\nf: function\n");
  EXPECT_EQ(out(sh, "function g {} ; g; echo ok"), "ok\n");
}

TEST(CompoundM14, ProcessSubstitutionAsRedirectionTarget) {
  Shell sh;
  EXPECT_EQ(out(sh, "cat < <(echo ps)"), "ps\n");
  EXPECT_EQ(out(sh, "while read l; do echo got $l; done < <(printf 'a\\nb\\n')"),
            "got a\ngot b\n");
}

TEST(CompoundM14, SubshellInPipelineRunsInTheStageProcess) {
  Shell sh;
  // The grandparent of a command run inside `( … )` is the shell itself
  // when the subshell needs no fork of its own.
  EXPECT_EQ(out(sh, "( : ; sh -c 'ps -o ppid= -p $PPID' ) | tr -d ' ' | "
                    "{ read p; [[ $p == $$ ]] && echo nofork }"),
            "nofork\n");
  EXPECT_EQ(out(sh, "( trap 'echo sub' EXIT; echo in ) | cat; echo out"),
            "in\nsub\nout\n");
}

// ---- M15c: shell features compsys needs ------------------------------------------

// (a|b) and (#i) start a word after the command word.
TEST(Compound15c, GlobGroupArgument) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; f() { echo f; }; f; echo (x|y).c (#i)X.C"),
            "f\n");
}

// function {} runs at once with its arguments.
TEST(Compound15c, AnonymousFunction) {
  Shell sh;
  EXPECT_EQ(out(sh, "function { echo $# $1; } a b; () { echo anon $1; } z"),
            "2 a\nanon z\n");
}

// case word; in, (…) patterns and <n-m> ranges.
TEST(Compound15c, CasePatternForms) {
  Shell sh;
  EXPECT_EQ(out(sh, "case 5 in; (<1-9>) echo num;; esac; case ab in (a|b)b) echo grp;; "
                "esac"),
            "num\ngrp\n");
}

// {echo hi} and f() {echo } open a brace group.
TEST(Compound15c, BraceAfterWord) {
  Shell sh;
  EXPECT_EQ(out(sh, "{echo hi}; f() {echo fn }; f"),
            "hi\nfn\n");
}

// An empty alternative matches the empty string.
TEST(Compound15c, CaseEmptyAlternative) {
  Shell sh;
  EXPECT_EQ(out(sh, "case b in |a|b) echo c1;; esac; case \"\" in |a) echo empty;; "
                "esac; case \"\" in a|) echo e3;; esac"),
            "c1\nempty\ne3\n");
}

// [[ n -gt ((x+4)) ]].
TEST(Compound15c, CondArithmeticOperandInParens) {
  Shell sh;
  EXPECT_EQ(out(sh, "o=1; [[ 6 -gt ((o+4)) ]] && echo c2; "
                "[[ 2 -gt (( o + 2 )) ]] || echo c4"),
            "c2\nc4\n");
}

// foreach takes a do…done or { } body too.
TEST(Compound15c, ForeachWithDoOrBrace) {
  Shell sh;
  EXPECT_EQ(out(sh, "foreach c (x y); do echo $c; done; foreach c (1 2) { echo b$c }"),
            "x\ny\nb1\nb2\n");
}

// a ; ; b runs a and b.
TEST(Compound15c, EmptyCommandsBetweenSeparators) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo a; ; echo b"),
            "a\nb\n");
}

// ((( … )) && …) is a subshell holding (( … )).
TEST(Compound15c, TripleParenIsSubshellAndArithmetic) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=3; if ((( a == 3 )) && [[ x == x ]]) || [[ -d / ]]; "
                "then echo yes; fi; ((a) ; echo s) 2>/dev/null"),
            "yes\ns\n");
}

// In "${…}" a { doesn't pair, \} is }, ' is plain.
TEST(Compound15c, BracesInQuotedParameterOperands) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=; "
                "echo \"${x:-a{b\\}c}\" \"${x:-\\}}\" \"${x:-{a}}\" \"${x:-it's}\" ${x:-{a}}"),
            "a{b}c } {a} it's {a}\n");
}

TEST(Compound15c, ProcessSubstitutionArgumentOfAnonymousFunction) {
  Shell sh;
  EXPECT_EQ(out(sh, "() { cat $1; } =(echo hi)"),
            "hi\n");
}

// {echo} runs echo; {echo hi} too.
TEST(Compound15c, BraceGroupWordClosesTheGroup) {
  Shell sh;
  EXPECT_EQ(out(sh, "{echo}; {echo hi}; {echo a; echo b}"),
            "\nhi\na\nb\n");
}

// After the command word !( … ) is one word.
TEST(Compound15c, BangGroupIsOneWord) {
  Shell sh;
  // `(a*)` is a qualifier list there, as in zsh.
  EXPECT_EQ(sh.run("echo !(a*)").err, "jfsh: number expected\n");
  EXPECT_EQ(out(sh, "! true; echo $?"), "1\n");
}

// An unclosed ( or [ is a bad pattern; a lone [ is a word.
TEST(Compound15c, UnclosedGroupIsABadPattern) {
  Shell sh;
  EXPECT_EQ(sh.run("echo a[b").err, "jfsh: bad pattern: a[b\n");
  EXPECT_EQ(sh.run("echo a (b").err, "jfsh: bad pattern: (b\n");
  EXPECT_EQ(out(sh, "echo ["), "[\n");
}

// (a|b) starts a word in for … in and in arrays.
TEST(Compound15c, GroupWordsInForAndArrays) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=( (x|y)z(N) w ); echo $a; for f in (x|y)z(N) v; do echo $f; done"),
            "w\nv\n");
}

// ---- M4 review fixes (expected values from zsh 5.9) ---------------------------

// The always block runs after an error, which $TRY_BLOCK_ERROR holds: the
// block may clear it to go on.
TEST(CompoundM4Review, AlwaysRunsAfterAnError) {
  Shell sh;
  auto r = sh.run("{ echo $((1/0)); echo no } always { echo al $TRY_BLOCK_ERROR; "
                  "TRY_BLOCK_ERROR=0 }; echo after");
  EXPECT_EQ(r.out, "al 1\nafter\n");
  EXPECT_EQ(r.err, "jfsh: division by zero\n");
  EXPECT_EQ(r.rc, 0);
  r = sh.run("{ echo $((1/0)); echo no } always { echo al $TRY_BLOCK_ERROR }; echo after");
  EXPECT_EQ(r.out, "al 1\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "f() { { echo $((1/0)); echo no } always { echo al $TRY_BLOCK_ERROR; "
                    "TRY_BLOCK_ERROR=0 }; echo after; }; f; echo st $?"),
            "al 1\nafter\nst 0\n");
  EXPECT_EQ(out(sh, "{ false } always { echo $TRY_BLOCK_ERROR }; echo $?"), "0\n1\n");
  EXPECT_EQ(out(sh, "echo $TRY_BLOCK_ERROR $TRY_BLOCK_INTERRUPT; { echo $TRY_BLOCK_ERROR } "
                    "always { echo $TRY_BLOCK_ERROR $TRY_BLOCK_INTERRUPT }; "
                    "echo $TRY_BLOCK_ERROR"),
            "-1 -1\n-1\n0 0\n-1\n");
  // An error in the always block itself is dropped.
  EXPECT_EQ(out(sh, "{ true } always { echo $((1/0)); echo x }; echo after $?"), "after 0\n");
}

// The status is the first block's, also for a `return` in the always
// block; `exit` outside a function and ERR_EXIT leave at once.
TEST(CompoundM4Review, AlwaysStatusReturnAndExit) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { { return 2 } always { return 5 } }; f; echo $?"), "2\n");
  EXPECT_EQ(out(sh, "f() { { echo t } always { return 6 }; echo no }; f; echo $?"), "t\n0\n");
  EXPECT_EQ(out(sh, "f() { { return 2 } always { false } }; f; echo $?"), "2\n");
  EXPECT_EQ(out(sh, "for i in 1 2 3; do { continue } always { echo al$i }; echo no; done"),
            "al1\nal2\nal3\n");
  auto r = sh.run("f() { { exit 3 } always { echo a } }; f; echo no");
  EXPECT_EQ(r.out, "a\n");
  EXPECT_EQ(r.rc, 3);
  r = sh.run("{ exit 3 } always { echo a }");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 3);
  Shell errexit;
  r = errexit.run("setopt errexit; f() { { false } always { echo al } }; f; echo no");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 1);
  // So does ${x?} in a shell that isn't interactive; not in a here-string.
  r = sh.run("f() { { echo ${x?u} } always { echo al } }; f; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(sh.run("( { echo ${x?u} } always { echo al } ); echo after $?").out, "after 1\n");
  EXPECT_EQ(sh.run("cat <<< ${x?u}; echo rc=$?").out, "rc=1\n");
}

// Errors that are fatal in zsh end the command line.
TEST(CompoundM4Review, FatalErrorsEndTheCommandLine) {
  Shell sh;
  auto r = sh.run("break; echo $?");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: break: not in while, until, select, or repeat loop\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "continue; echo $?"), "");
  r = sh.run("for i in 1 2; do break x; echo $i; done");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: break: argument is not positive: 0\n");
  EXPECT_EQ(out(sh, "for i in 1 2; do break -1; echo $i; done"), "");
  r = sh.run("f() { f }; f; echo st $?");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: f: maximum nested function level reached; increase FUNCNEST?\n");
  // FUNCNEST is arithmetic: `abc` is 0.
  r = sh.run("FUNCNEST=abc; f() { echo in }; f; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(sh.run("unset FUNCNEST; FUNCNEST=1+1; f() { echo $1; f $(($1+1)) }; f 1").out,
            "1\n2\n");
  sh.run("unset FUNCNEST");
  EXPECT_EQ(out(sh, "for ((i=0; i<1/0; i++)); do :; done; echo $?"), "");
  EXPECT_EQ(out(sh, "for ((i=1/0;;)); do :; done; echo $?"), "");
  EXPECT_EQ(out(sh, "repeat 1/0 echo; echo $?"), "");
  EXPECT_EQ(out(sh, "for i in ${x?u}; do :; done; echo $?"), "");
  r = sh.run("f() { local 1=x; echo $1 }; f a");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: local: not an identifier: 1\n");
  r = sh.run("f() { local a[1]=x; echo $? }; f");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: local: a[1]: can't create local array elements\n");
  EXPECT_EQ(out(sh, "f() { local a=(1 2); local a[1]=x; echo $? $a }; f"), "0 x 2\n");
  EXPECT_EQ(out(sh, "a=(1 2); typeset a[1]=x; echo $? $a"), "0 x 2\n");
  // typeset goes on with its other names first.
  EXPECT_EQ(out(sh, "{ typeset x=1 1y=2 z=3 } always { TRY_BLOCK_ERROR=0; echo $x $z }"),
            "1 3\n");
  r = sh.run("readonly r; read r <<< a; echo $?");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: read-only variable: r\n");
  EXPECT_EQ(out(sh, "for r in a b; do echo $r; done; echo $?"), "");
}

// for, select and read assign as `name=value` does.
TEST(CompoundM4Review, LoopsAndReadAssignAsAssignmentsDo) {
  Shell sh;
  EXPECT_EQ(out(sh, "typeset -i n; for n in 1+1 3; do echo $n; done"), "2\n3\n");
  EXPECT_EQ(out(sh, "typeset -i n; read n <<< 2+3; echo $n"), "5\n");
  EXPECT_EQ(out(sh, "typeset -i n; select n in 1+1 3; do echo $n; break; done <<< 1"), "2\n");
  EXPECT_EQ(out(sh, "for 1 in a b; do echo $1; done"), "a\nb\n");
  EXPECT_EQ(out(sh, "set -- p q; for 1 2 in a b c; do echo $1 $2; done; echo $#"),
            "a b\nc\n2\n");
  EXPECT_EQ(out(sh, "read 1 <<< a; echo $1"), "a\n");
  EXPECT_EQ(sh.run("read 1x <<< a; echo $?").err, "jfsh: not an identifier: 1x\n");
  // A reply is read as atoi does.
  EXPECT_EQ(out(sh, "select x in a b c; do echo \"[$x]\"; break; done <<< 1x"), "[a]\n");
}

// typeset -i N, -F N and -E N: a base or a number of digits.
TEST(CompoundM4Review, TypesetBaseAndDigits) {
  Shell sh;
  EXPECT_EQ(out(sh, "typeset -F 2 x=1.5; echo $x; typeset -p x"), "1.50\ntypeset -F x=1.50\n");
  EXPECT_EQ(out(sh, "f() { local -F2 a=3.14159; echo $a }; f"), "3.14\n");
  EXPECT_EQ(out(sh, "typeset -i 16 h=255; echo $h; typeset -p h; h=17; echo $h $((h+1))"),
            "16#FF\ntypeset -i16 h=255\n16#11 18\n");
  EXPECT_EQ(out(sh, "f() { local -E 3 e=1234; echo $e; typeset -p e }; f"),
            "1.23e+03\ntypeset -E e=1.23e+03\n");
  EXPECT_EQ(out(sh, "typeset -i2 b=5; echo $b"), "2#101\n");
  EXPECT_EQ(out(sh, "typeset -i 16 n=-255; echo $n"), "-16#FF\n");
  EXPECT_EQ(out(sh, "typeset -F 0 z=2.6; echo $z"), "2.6000000000\n");
  EXPECT_EQ(out(sh, "typeset -F 3 a b=2; typeset -p a b"),
            "typeset -F a=0.000\ntypeset -F b=2.000\n");
  EXPECT_EQ(out(sh, "typeset -F 2 y=1.5; typeset -F y; y=3; echo $y"), "3.00\n");
  EXPECT_EQ(out(sh, "typeset -F 2 w=1.5; typeset -i w; typeset -p w"), "typeset -i w=1\n");
  EXPECT_EQ(out(sh, "typeset -F 2 v=1.5; typeset -E v; echo $v"), "1.5e+00\n");
  EXPECT_EQ(out(sh, "float -F 3 fl=1; typeset -p fl"), "typeset -F fl=1.000\n");
  EXPECT_EQ(sh.run("integer 8 o=9").err, "jfsh: integer: not an identifier: 8\n");
  auto r = sh.run("typeset -i 1 q=3; echo $?");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: typeset: invalid base (must be 2 to 36 inclusive): 1\n");
}

// A sourced file is outside the caller's loops, and an error ends just the
// file, with status 126.
TEST(CompoundM4Review, SourcedFileErrors) {
  Shell sh;
  TempDir dir;
  auto brk = dir.file("brk", "echo in\nbreak\necho after-break\n", test::sf::perms::owner_all);
  auto err = dir.file("err", "echo $((1/0))\necho after\n", test::sf::perms::owner_all);
  auto r = sh.run("for i in 1 2; do . " + brk.string() + "; echo it$i; done");
  EXPECT_EQ(r.out, "in\nit1\nin\nit2\n");
  EXPECT_EQ(r.err, "jfsh: break: not in while, until, select, or repeat loop\n"
                   "jfsh: break: not in while, until, select, or repeat loop\n");
  EXPECT_EQ(out(sh, ". " + err.string() + "; echo st $?"), "st 126\n");
}

// readonly is `typeset -r`: local in a function, unless POSIX_BUILTINS.
TEST(CompoundM4Review, ReadonlyInAFunctionIsLocal) {
  Shell sh;
  EXPECT_EQ(out(sh, "z=0; f() { readonly z=1; echo $z }; f; echo $z; z=3; echo $z"),
            "1\n0\n3\n");
  EXPECT_EQ(sh.run("setopt posixbuiltins; g() { readonly w=1 }; g; w=2").err,
            "jfsh: read-only variable: w\n");
}

// A new local is not exported, whatever it hides.
TEST(CompoundM4Review, LocalsAreNotExported) {
  Shell sh;
  EXPECT_EQ(out(sh, "export y=1; g() { local y=5; printenv y; y=6 printenv y; "
                    "local -x y=7; printenv y }; g; printenv y"),
            "6\n7\n1\n");
  EXPECT_EQ(sh.run("f() { export x=1; local x=2; printenv x }; f").rc, 1);
}

// Function names are expanded as words.
TEST(CompoundM4Review, FunctionNamesAreExpanded) {
  Shell sh;
  EXPECT_EQ(out(sh, "'if'() { echo kw }; 'if'"), "kw\n");
  EXPECT_EQ(out(sh, "\"foo\"() { echo q }; foo"), "q\n");
  EXPECT_EQ(out(sh, "f\\oo() { echo bs }; foo"), "bs\n");
  EXPECT_EQ(out(sh, "n=foo; $n() { echo dyn }; foo"), "dyn\n");
  EXPECT_EQ(out(sh, "function \"foo\" { echo fn }; foo"), "fn\n");
  TempDir dir;
  EXPECT_EQ(sh.run("(cd " + dir.path.string() + "; zq*() { echo star })").err,
            "jfsh: no matches found: zq*\n");
}

// zsh 5.9 forgets the `!` of `! f` once f has run commands: ERR_EXIT and
// ZERR act on the negated status.
TEST(CompoundM4Review, NegatedFunctionCallAndErrExit) {
  {
    Shell sh;
    auto r = sh.run("setopt errexit; f() { false; echo in; }; ! f; echo ok");
    EXPECT_EQ(r.out, "in\n");
    EXPECT_EQ(r.rc, 1);
  }
  {
    Shell sh;
    EXPECT_EQ(out(sh, "setopt errexit; ! { true }; ! true; f() { }; ! f; echo ok"), "ok\n");
    EXPECT_EQ(out(sh, "f() { true }; ! f || true; if ! f; then :; fi; echo ok"), "ok\n");
  }
  Shell sh;
  EXPECT_EQ(out(sh, "f() { true; }; trap 'echo ZERR' ZERR; ! f; echo ok"), "ZERR\nok\n");
}

// for/select names, `()` and `( )`, `{}`, and the text of a parse error at
// the end of the input.
TEST(CompoundM4Review, ParsingCorners) {
  Shell sh;
  EXPECT_EQ(out(sh, "for in in a b; do echo $in; done"), "a\nb\n");
  EXPECT_EQ(sh.run("for x-y in a; do :; done").err, "jfsh:1: parse error near `x-y'\n");
  EXPECT_EQ(sh.run("select x y in a b; do :; done").err, "jfsh:1: parse error near `do'\n");
  EXPECT_EQ(out(sh, "false; ( ); echo $?"), "0\n");
  EXPECT_EQ(out(sh, "false; (); echo $?"), "1\n");
  EXPECT_EQ(out(sh, "f() ( ); f; echo $?"), "0\n");
  EXPECT_EQ(out(sh, "false; {}; echo $?"), "0\n");
  EXPECT_EQ(sh.run("{} a").err, "jfsh:1: parse error near `a'\n");
  EXPECT_EQ(out(sh, "f(); echo hi; echo x; f"), "x\nhi\n");
  EXPECT_EQ(sh.run("if true; then").err, "jfsh:1: parse error near `then'\n");
  EXPECT_EQ(sh.run("f() {").err, "jfsh:1: parse error near `{'\n");
  EXPECT_EQ(sh.run_script("echo x\n{ echo\n").err, "jfsh:3: parse error near `\\n'\n");
  EXPECT_EQ(out(sh, "[[ a ==\n\n  a ]] && echo y; [[ a\n== a ]] && echo z"), "y\nz\n");
}

// An unclosed `[` is a bad pattern in case, [[ ]] and ${…#…}.
TEST(CompoundM4Review, UnclosedBracketIsABadPattern) {
  Shell sh;
  auto r = sh.run("case x in [) echo br;; esac; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: bad pattern: [\n");
  r = sh.run("[[ x = a[ ]]; echo $?");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 2);
  EXPECT_EQ(sh.run("x='[a'; echo ${x#[}").err, "jfsh: bad pattern: [\n");
  EXPECT_EQ(out(sh, "echo ["), "[\n");
}

// The compiled case patterns are kept per option setting.
TEST(CompoundM4Review, CasePatternsFollowTheOptions) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; for i in 1 2; do case aB in (#i)ab) echo m$i;; "
                    "esac; unsetopt extendedglob; done"),
            "m1\n");
}

// Long `[[ ]]` chains, deep nesting and many `!`s: no recursion where zsh
// copes, and a clean error where the nesting is too deep to evaluate.
TEST(CompoundM4Review, LongAndDeepConditions) {
  test::on_stack(8 << 20, [] {
    Shell sh;
    str chain = "[[ a == b";
    for (int i = 0; i < 40000; ++i) chain += " || a == b";
    EXPECT_EQ(out(sh, chain + " || a == a ]] && echo yes"), "yes\n");
    str nest = "[[ ";
    for (int i = 0; i < 30000; ++i) nest += "( ";
    nest += "a == a";
    for (int i = 0; i < 30000; ++i) nest += " )";
    EXPECT_EQ(out(sh, nest + " ]] && echo yes"), "yes\n");
    str nots = "[[ ";
    for (int i = 0; i < 20001; ++i) nots += "! ";
    EXPECT_EQ(out(sh, nots + "a == a ]]; echo $?"), "1\n");
    str left = "[[ ";
    for (int i = 0; i < 300000; ++i) left += "( ";
    left += "a == b";
    for (int i = 0; i < 300000; ++i) left += " ) || a == b";
    auto r = sh.run(left + " ]]; echo after");
    EXPECT_EQ(r.out, "");
    EXPECT_EQ(r.err, "jfsh: condition nested too deeply\n");
  });
}

// The text `functions` prints reads back the same: [[ ]] grouping, here-
// document delimiters, `|&`, case alternatives.
TEST(CompoundM4Review, FunctionTextReadsBack) {
  Shell sh;
  EXPECT_EQ(out(sh, "h() { [[ -z x && ( a == a || b == b ) ]]; [[ ! ( a || b ) ]]; "
                    "[[ ( a && b ) || c ]]; [[ a && ( b && c ) ]]; [[ ( a || b ) || c ]]; "
                    "[[ ! ! a ]]; [[ ( ! a ) && b ]] }; functions h"),
            "h () {\n"
            "\t[[ -z x && ( a == a || b == b ) ]]\n"
            "\t[[ ! ( -n a || -n b ) ]]\n"
            "\t[[ ( -n a && -n b ) || -n c ]]\n"
            "\t[[ -n a && -n b && -n c ]]\n"
            "\t[[ -n a || -n b || -n c ]]\n"
            "\t[[ ! ( ! -n a ) ]]\n"
            "\t[[ ! -n a && -n b ]]\n"
            "}\n");
  EXPECT_EQ(out(sh, "f() {\n  cat <<'EOT'\na $x\nEOT\n  cat <<-X\n\td\n\tX\n"
                    "  cat <<E\"N\"D\ne\nEND\n  cat <<A <<B; echo after\n1\nA\n2\nB\n}\n"
                    "functions f"),
            "f () {\n\tcat <<'EOT'\na $x\nEOT\n\tcat <<X\nd\nX\n\tcat <<E\"N\"D\ne\nEND\n"
            "\tcat <<A <<B\n1\nA\n2\nB\n\techo after\n}\n");
  EXPECT_EQ(out(sh, "k() { case $1 in a|b) echo;; (c|d) ;; (e | f) ;; (g|h) | i) ;; *) ;; "
                    "esac; a |& b | c }; functions k"),
            "k () {\n\tcase $1 in\n\t\t(a | b) echo ;;\n\t\t(c|d)  ;;\n\t\t(e|f)  ;;\n"
            "\t\t((g|h) | i)  ;;\n\t\t(*)  ;;\n\tesac\n\ta 2>&1 | b | c\n}\n");
  EXPECT_EQ(out(sh, "g() { [[ -z $1 && ( a == a || b == c ) ]] && echo c1; cat <<'E'\n"
                    "lit $HOME\nE\n}; t=$(functions g); unfunction g; eval \"$t\"; g; "
                    "[[ $(functions g) == \"$t\" ]] && echo same"),
            "c1\nlit $HOME\nsame\n");
}

// Lines continued with a backslash, and an array assignment over several
// lines, are one command on stdin.
TEST(CompoundM4Review, StdinJoinsContinuedLines) {
  auto run = [](strv input) {
    str cmd = "printf '" + str(input) + "' | " JFSH_BINARY " --norouter -f 2>&1";
    s::FILE* p = ::popen(cmd.c_str(), "r");
    str o;
    char buf[256];
    while (p and s::fgets(buf, sizeof buf, p)) o += buf;
    if (p) ::pclose(p);
    return o;
  };
  EXPECT_EQ(run("echo a \\\\\\nb\\n"), "a b\n");
  EXPECT_EQ(run("echo ab\\\\\\ncd\\n"), "abcd\n");
  EXPECT_EQ(run("x=(\\na b\\n); echo $x\\n"), "a b\n");
}

// ---- M4 review, round 2 -------------------------------------------------------

// Runs `jfsh -f -c src` as a process of its own (for signals), stderr merged.
str run_binary(strv src) {
  str quoted;
  for (char c : src) quoted += c == '\'' ? str("'\\''") : str(1, c);
  str cmd = JFSH_BINARY " --norouter -f -c '" + quoted + "' 2>&1 </dev/null";
  s::FILE* p = ::popen(cmd.c_str(), "r");
  str o;
  char buf[256];
  while (p and s::fgets(buf, sizeof buf, p)) o += buf;
  if (p) ::pclose(p);
  return o;
}

// An error in a trap ends the trap, not the code it interrupted.
TEST(CompoundM4Review2, ErrorInATrapEndsJustTheTrap) {
  EXPECT_EQ(run_binary("trap 'break' USR1; kill -USR1 $$; echo after"),
            "jfsh: break: not in while, until, select, or repeat loop\nafter\n");
  EXPECT_EQ(run_binary("trap 'local 1=x; echo t2' USR1; kill -USR1 $$; echo after"),
            "jfsh: local: not an identifier: 1\nafter\n");
  EXPECT_EQ(run_binary("trap 'echo $((1/0)); echo t2' USR1; kill -USR1 $$; echo after $?"),
            "jfsh: division by zero\nafter 0\n");
  EXPECT_EQ(run_binary("TRAPUSR1() { echo $((1/0)) }; kill -USR1 $$; echo after $?"),
            "jfsh: division by zero\nafter 0\n");
  Shell sh;
  EXPECT_EQ(out(sh, "f() { trap 'echo $((1/0))' EXIT }; f; echo after"), "after\n");
  // A function's EXIT trap runs on an `exit` in it too.
  auto r = sh.run("f() { trap 'echo inT $?' EXIT; exit 2 }; f; echo no");
  EXPECT_EQ(r.out, "inT 0\n");
  EXPECT_EQ(r.rc, 2);
}

// An `exit` that unwinds out of the functions leaves at once.
TEST(CompoundM4Review2, ExitFromAFunctionSkipsOuterAlwaysBlocks) {
  Shell sh;
  auto r = sh.run("f() { { exit 3 } always { echo a } }; { f } always { echo b }; echo no");
  EXPECT_EQ(r.out, "a\n");
  EXPECT_EQ(r.rc, 3);
  r = sh.run("{ f() { exit 2 }; f } always { echo b }");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 2);
}

// A runtime error in a sourced file gives 126, unless ERR_EXIT exits with
// its status; a parse error gives 126 too.
TEST(CompoundM4Review2, SourcedFileStatus) {
  TempDir dir;
  auto err = dir.file("err", "echo $((1/0))\n", test::sf::perms::owner_all);
  auto bad = dir.file("bad", "echo s1\nif\n", test::sf::perms::owner_all);
  Shell sh;
  EXPECT_EQ(out(sh, ". " + bad.string() + "; echo after $?"), "s1\nafter 126\n");
  Shell errexit;
  auto r = errexit.run("setopt errexit; . " + err.string() + "; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 1);
}

// zsh's $LINENO and $HISTCMD are read-only, also as locals; a local
// special integer is set through its setter.
TEST(CompoundM4Review3, LocalSpecials) {
  Shell sh;
  auto r = sh.run("f() { local LINENO=50; echo $LINENO }; f");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: read-only variable: LINENO\n");
  EXPECT_EQ(sh.run("f() { local HISTCMD=3 }; f").err, "jfsh: read-only variable: HISTCMD\n");
  EXPECT_EQ(sh.run("LINENO=5").err, "jfsh: read-only variable: LINENO\n");
  EXPECT_EQ(out(sh, "f() { local status; echo x }; f"), "x\n");
  EXPECT_EQ(out(sh, "f() { local -i OPTIND=3; echo $OPTIND }; f; echo $OPTIND"), "3\n1\n");
}

TEST(CompoundM4Review2, TypesetCorners) {
  Shell sh;
  EXPECT_EQ(out(sh, "'a b'() { echo sp }; functions 'a b'"), "'a b' () {\n\techo sp\n}\n");
  EXPECT_EQ(out(sh, "typeset -i 16 -x h=255; printenv h"), "16#FF\n");
  auto r = sh.run("typeset -F 1.5 x=1; echo $? $x");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: typeset: bad precision value: 1.5\n");
  EXPECT_EQ(sh.run("typeset -i 1x h").err, "jfsh: typeset: bad base value: 1x\n");
  EXPECT_EQ(out(sh, "f() { local TRY_BLOCK_ERROR=3; echo $TRY_BLOCK_ERROR }; f; "
                    "echo $TRY_BLOCK_ERROR"),
            "3\n-1\n");
}

}  // namespace
}  // namespace jfsh
