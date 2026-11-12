// Word expansion (M3): parameters, command and arithmetic substitution,
// splitting, braces, tilde, globbing and quote handling, run end to end
// through Executor::exec_string.

#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <jfsh/exec/subst.h>

#include "shell_fixture.h"

namespace jfsh {
namespace {

using test::Shell;
using test::TempDir;
namespace fs = std::filesystem;

constexpr fs::perms kFile = fs::perms::owner_read | fs::perms::owner_write;

// Runs `cmd` and returns its stdout.  `show` prints each argument in
// brackets so word boundaries are visible: show a 'b c' → "[a][b c]".
std::string out(Shell& sh, std::string_view cmd) {
  auto r = sh.run(cmd);
  EXPECT_EQ(r.err, "") << cmd;
  return r.out;
}

std::string show(Shell& sh, std::string_view args) {
  return out(sh, "env printf '[%s]' " + std::string(args));
}

// ---- parameters --------------------------------------------------------------

TEST(Subst, ScalarsAndQuotes) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=hello; echo $x ${x} \"$x\" '$x' \\$x"),
            "hello hello hello $x $x\n");
  EXPECT_EQ(out(sh, "x=1; echo \"${x}x\" \"$x\"y $x'z' a$x$x"), "1x 1y 1z a11\n");
  EXPECT_EQ(out(sh, "echo \"a\\$b\" \"a\\\\b\" 'a\\b' a\\ b"), "a$b a\\b a\\b a b\n");
}

TEST(Subst, ScalarsAreNotSplitOrGlobbed) {
  Shell sh;
  EXPECT_EQ(show(sh, "\"$x\" $x"), "[]");   // unset: "$x" is a word, $x none
  sh.run("x='a  b'; y='*'");
  EXPECT_EQ(show(sh, "$x $y"), "[a  b][*]");
}

TEST(Subst, UnsetAndEmptyExpandToNothingUnquoted) {
  Shell sh;
  EXPECT_EQ(show(sh, "a $nope \"$nope\" x$nope ''"), "[a][][x][]");
}

TEST(Subst, DefaultOperators) {
  Shell sh;
  sh.run("x=val; e=");
  EXPECT_EQ(out(sh, "echo ${x:-d} ${u:-d} ${e:-d} ${e-d}. ${u-d}"), "val d d . d\n");
  EXPECT_EQ(out(sh, "echo ${x:+alt} ${u:+alt}. ${e+set}"), "alt . set\n");
  EXPECT_EQ(out(sh, "echo ${u:=assigned} $u"), "assigned assigned\n");
  EXPECT_EQ(sh.get("u"), "assigned");
  EXPECT_EQ(out(sh, "echo ${+x} ${+nope}"), "1 0\n");
}

TEST(Subst, ErrorOperatorFailsTheCommand) {
  Shell sh;
  auto r = sh.run("echo ${nope:?custom message}");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: nope: custom message\n");
  EXPECT_EQ(sh.run("echo ${nope?}").err, "jfsh: nope: parameter not set\n");
}

TEST(Subst, NounsetRejectsUnsetParameters) {
  Shell sh;
  sh.opts.unset(OPT_UNSET);
  auto r = sh.run("echo $nope");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: nope: parameter not set\n");
  EXPECT_EQ(sh.run("echo ${nope:-ok}").out, "ok\n");
}

TEST(Subst, LengthSubstringAndSubscripts) {
  Shell sh;
  sh.run("x=hello");
  EXPECT_EQ(out(sh, "echo ${#x} ${x:1} ${x:1:2} ${x: -2} ${x:0:-1}"), "5 ello el lo hell\n");
  EXPECT_EQ(out(sh, "echo ${x[2]} ${x[2,3]} ${x[-1]} $x[1]"), "e el o h\n");
}

TEST(Subst, PatternRemovalAndReplacement) {
  Shell sh;
  sh.run("f=/a/b/c.tar.gz; s=aXbXc");
  EXPECT_EQ(out(sh, "echo ${f#*/} ${f##*/} ${f%.*} ${f%%.*}"),
            "a/b/c.tar.gz c.tar.gz /a/b/c.tar /a/b/c\n");
  EXPECT_EQ(out(sh, "echo ${s/X/-} ${s//X/-} ${s/#a/A} ${s/%c/C} ${s:/aXbXc/all}"),
            "a-bXc a-b-c AXbXc aXbXC all\n");
  EXPECT_EQ(out(sh, "echo ${s//[ab]/_} ${s/(X|c)/_} ${f//\\//:}"),
            "_X_Xc a_bXc :a:b:c.tar.gz\n");
  // A quoted pattern character is literal.
  EXPECT_EQ(out(sh, "p='a*'; echo ${p#\"a*\"}. ${p#a\\*}."), ". .\n");
}

TEST(Subst, Modifiers) {
  Shell sh;
  sh.run("f=/a/b/c.tar.gz; p=AbC");
  EXPECT_EQ(out(sh, "echo ${f:t} ${f:h} ${f:r} ${f:e} ${f:t:r} ${p:l} ${p:u}"),
            "c.tar.gz /a/b /a/b/c.tar gz c.tar abc ABC\n");
  EXPECT_EQ(out(sh, "x=/a/./b/../c; echo ${x:a}"), "/a/c\n");
}

TEST(Subst, Arrays) {
  Shell sh;
  sh.run("a=(one 'two three' '' four)");
  EXPECT_EQ(show(sh, "$a"), "[one][two three][four]");             // empties dropped
  EXPECT_EQ(show(sh, "\"${a[@]}\""), "[one][two three][][four]");
  EXPECT_EQ(show(sh, "\"$a\""), "[one two three  four]");
  EXPECT_EQ(show(sh, "${#a} ${a[2]} ${a[-1]} $a[1] ${a[2,3]}"),
            "[4][two three][four][one][two three]");
  EXPECT_EQ(show(sh, "x${a[1,2]}y"), "[xone][two threey]");
  sh.run("a+=(five)");
  EXPECT_EQ(out(sh, "echo ${#a} ${a[-1]}"), "5 five\n");
}

TEST(Subst, ArrayAssignmentExpandsItsElements) {
  Shell sh;
  sh.run("n=x; b=(\n  $n   # comment\n  {1,2}\n  \"$n y\"\n)");
  EXPECT_EQ(show(sh, "\"${b[@]}\""), "[x][1][2][x y]");
}

TEST(Subst, AssociativeArrays) {
  Shell sh;
  sh.params.assign("h", std::unordered_map<std::string, std::string>{{"k1", "v1"}, {"k2", "v2"}},
                   PM_HASH);
  EXPECT_EQ(out(sh, "k=k2; echo ${h[k1]} ${h[$k]} ${(k)h} $h ${#h}"), "v1 v2 k1 k2 v1 v2 2\n");
  sh.run("h=(a 1 b 2)");
  EXPECT_EQ(out(sh, "echo ${(kv)h}"), "a 1 b 2\n");
}

TEST(Subst, Flags) {
  Shell sh;
  sh.run("a=(b a c a); x=abc; c='a,b,,c'");
  EXPECT_EQ(out(sh, "echo ${(U)x} ${(C)x} ${(L)${(U)x}} ${(o)a} ${(O)a} ${(u)a}"),
            "ABC Abc abc a a b c c b a a b a c\n");
  EXPECT_EQ(out(sh, "echo ${(j:-:)a} \"${(j:/:)a}\""), "b-a-c-a b/a/c/a\n");
  EXPECT_EQ(show(sh, "${(s:,:)c} \"${(s:,:)c}\" \"${(@s:,:)c}\""),
            "[a][b][c][a b c][a][b][c]");
  EXPECT_EQ(out(sh, "n=(10 9 100); echo ${(on)n} ${(On)n}"), "9 10 100 100 10 9\n");
  EXPECT_EQ(out(sh, "q='a b'; echo ${(q)q} ${(qq)q} ${(Q)${(qq)q}}"), "a\\ b 'a b' a b\n");
  EXPECT_EQ(out(sh, "ref=x; echo ${(P)ref} ${(t)x} ${(t)a}"), "abc scalar array\n");
}

TEST(Subst, NestedSubstitutions) {
  Shell sh;
  sh.run("x=abc");
  EXPECT_EQ(out(sh, "echo ${${x#a}%c} ${#${x}}"), "b 3\n");
  EXPECT_EQ(show(sh, "${(f)\"$(env printf 'l 1\\nl2')\"}"), "[l 1][l2]");
  EXPECT_EQ(show(sh, "\"${(f)$(env printf 'l1\\nl2')}\""), "[l1 l2]");
}

TEST(Subst, PositionalParameters) {
  Shell sh;
  sh.ctx.arg0       = "script";
  sh.ctx.positional = {"a b", "c", "d", "e", "f", "g", "h", "i", "j", "tenth"};
  EXPECT_EQ(show(sh, "$0 $# $1 ${10} $10"), "[script][10][a b][tenth][a b0]");
  sh.ctx.positional = {"a b", "", "c"};
  EXPECT_EQ(show(sh, "\"$@\""), "[a b][][c]");
  EXPECT_EQ(show(sh, "\"x$@y\""), "[xa b][][cy]");
  EXPECT_EQ(show(sh, "$@"), "[a b][c]");
  EXPECT_EQ(show(sh, "\"$*\" ${argv[3]} ${@[1]}"), "[a b  c][c][a b]");
  sh.ctx.positional.clear();
  EXPECT_EQ(show(sh, "a \"$@\" b"), "[a][b]");
}

TEST(Subst, SpecialParameters) {
  Shell sh;
  EXPECT_EQ(out(sh, "false; echo $?; echo $?"), "1\n0\n");
  EXPECT_EQ(out(sh, "echo $$"), std::to_string(getpid()) + "\n");
  EXPECT_EQ(out(sh, "echo $!"), "0\n");
  EXPECT_EQ(out(sh, "echo $(echo $$)"), std::to_string(getpid()) + "\n");
}

// ---- word splitting ------------------------------------------------------------

TEST(Subst, WordSplitting) {
  Shell sh;
  sh.run("x=' a  b '");
  EXPECT_EQ(show(sh, "$x"), "[ a  b ]");
  EXPECT_EQ(show(sh, "${=x}"), "[a][b]");
  sh.opts.set(OPT_SHWORDSPLIT);
  EXPECT_EQ(show(sh, "$x \"$x\""), "[a][b][ a  b ]");
  EXPECT_EQ(show(sh, "${==x}"), "[ a  b ]");
}

TEST(Subst, IfsWithNonWhitespaceSeparators) {
  Shell sh;
  sh.run("IFS=:; x=a:b::c");
  EXPECT_EQ(show(sh, "${=x}"), "[a][b][c]");
  EXPECT_EQ(ifs_split("a:b::c:", ":"), (std::vector<std::string>{"a", "b", "", "c"}));
  EXPECT_EQ(ifs_split("  a  b ", " \t\n"), (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(ifs_split("a : b", " :"), (std::vector<std::string>{"a", "b"}));
}

// ---- command substitution ------------------------------------------------------

TEST(Subst, CommandSubstitution) {
  Shell sh;
  EXPECT_EQ(show(sh, "$(env printf 'a b\\nc\\n\\n') \"$(env printf 'a b\\nc\\n\\n')\""),
            "[a][b][c][a b\nc]");
  EXPECT_EQ(show(sh, "x$(echo ' 1 2 ')y"), "[x][1][2][y]");
  EXPECT_EQ(out(sh, "echo `echo back` \"`echo q`\" $(echo $(echo nested))"), "back q nested\n");
  EXPECT_EQ(out(sh, "echo \"$(echo \")\")\""), ")\n");
}

TEST(Subst, CommandSubstitutionSetsStatus) {
  Shell sh;
  EXPECT_EQ(sh.run("x=$(exit 3)").rc, 3);
  EXPECT_EQ(out(sh, "x=$(exit 3); echo $?"), "3\n");
  EXPECT_EQ(out(sh, "x=$(true); echo $?"), "0\n");
  EXPECT_EQ(out(sh, "echo $(false) $?"), "1\n");
}

TEST(Subst, CommandSubstitutionOutputIsNotGlobbed) {
  TempDir dir;
  dir.file("f1", "", kFile);
  Shell sh;
  EXPECT_EQ(out(sh, "cd_dir=" + dir.path.string() + "; echo $(echo \"$cd_dir/*\")"),
            dir.path.string() + "/*\n");
}

// ---- arithmetic -----------------------------------------------------------------

TEST(Subst, Arithmetic) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $((1+2*3)) $((2**10)) $((7/2)) $((-7%3)) $(( (1+2)*3 ))"),
            "7 1024 3 -1 9\n");
  EXPECT_EQ(out(sh, "echo $((1<2)) $((3==4)) $((!0)) $((~0)) $((1?10:20)) $((5&3|8))"),
            "1 0 1 -1 10 9\n");
  EXPECT_EQ(out(sh, "echo $((0x10)) $((0b101)) $((2#101)) $((16#ff)) $[3+4]"),
            "16 5 5 255 7\n");
  EXPECT_EQ(out(sh, "i=5; echo $((i+=2)) $i $((i++)) $i $((--i)) $((i*=2,i+1))"),
            "7 7 7 8 7 15\n");
  EXPECT_EQ(out(sh, "n=3 e='n*2'; echo $((e+1)) $(( $n * (n+1) )) $((unset_var+1))"),
            "7 12 1\n");
  EXPECT_EQ(out(sh, "x=0; echo $((1 || (x=5))) $x $((0 && (x=6))) $x"), "1 0 0 0\n");
}

// Overflow wraps around, as in zsh, rather than being undefined behaviour.
TEST(Subst, ArithmeticOverflowWraps) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $((2**63)) $((9223372036854775807+1)) $(( (-9223372036854775807-1) / -1 ))"),
            "-9223372036854775808 -9223372036854775808 -9223372036854775808\n");
}

TEST(Subst, ArithmeticErrors) {
  Shell sh;
  auto r = sh.run("echo $((10/0))");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: division by zero\n");
  EXPECT_NE(sh.run("echo $((1+))").err.find("bad math expression"), std::string::npos);
}

TEST(Subst, LetBuiltin) {
  Shell sh;
  EXPECT_EQ(sh.run("let 'y = 6 * 7' 'z = y - 2'").rc, 0);
  EXPECT_EQ(sh.get("y"), "42");
  EXPECT_EQ(sh.get("z"), "40");
  EXPECT_EQ(sh.run("let 0").rc, 1);
  EXPECT_EQ(sh.run("let '1/0'").rc, 2);
}

// ---- brace, tilde, equals, $'…' --------------------------------------------------

TEST(Subst, BraceExpansion) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo {a,b,c} x{1..3}y {05..10..2} {c..a} {3..1} a{b,{c,d}}e {1,2}{x,y}"),
            "a b c x1y x2y x3y 05 07 09 c b a 3 2 1 abe ace ade 1x 1y 2x 2y\n");
  EXPECT_EQ(out(sh, "echo {a} {} '{a,b}' \\{a,b\\} \"{a,b}\" {a,'b,c'}"),
            "{a} {} {a,b} {a,b} {a,b} a b,c\n");
  EXPECT_EQ(out(sh, "x=1; echo {$x,2} a{-1..1}"), "1 2 a-1 a0 a1\n");
  EXPECT_EQ(out(sh, "x='{a,b}'; echo $x"), "{a,b}\n");
  sh.opts.set(OPT_IGNOREBRACES);
  EXPECT_EQ(out(sh, "echo {a,b}"), "{a,b}\n");
  sh.opts.unset(OPT_IGNOREBRACES);
  sh.opts.set(OPT_BRACECCL);
  EXPECT_EQ(out(sh, "echo x{cab}"), "xa xb xc\n");
}

TEST(Subst, TildeExpansion) {
  Shell sh;
  sh.params.assign("HOME", std::string("/home/t"), PM_SCALAR);
  sh.params.assign("PWD", std::string("/w"), PM_SCALAR);
  EXPECT_EQ(out(sh, "echo ~ ~/x ~+ a~ '~' \\~ \"~\""), "/home/t /home/t/x /w a~ ~ ~ ~\n");
  sh.run("p=~/a:~/b; q=x~");
  EXPECT_EQ(sh.get("p"), "/home/t/a:/home/t/b");
  EXPECT_EQ(sh.get("q"), "x~");
  EXPECT_EQ(out(sh, "echo ~root"), "/root\n");
  EXPECT_EQ(sh.run("echo ~no_such_user_xyz").err,
            "jfsh: no such user or named directory: no_such_user_xyz\n");
  EXPECT_EQ(out(sh, "echo --a=~/x"), "--a=~/x\n");
  sh.opts.set(OPT_MAGICEQUALSUBST);
  EXPECT_EQ(out(sh, "echo --a=~/x"), "--a=/home/t/x\n");
}

TEST(Subst, EqualsExpansion) {
  Shell sh;
  auto env_path = sh.exec.find_command("env");
  ASSERT_TRUE(env_path);
  EXPECT_EQ(out(sh, "echo =env"), *env_path + "\n");
  EXPECT_EQ(sh.run("echo =no_such_cmd_xyz").err, "jfsh: no_such_cmd_xyz not found\n");
  EXPECT_EQ(out(sh, "echo '=env' a=b ="), "=env a=b =\n");
}

TEST(Subst, DollarSingleQuotes) {
  Shell sh;
  EXPECT_EQ(show(sh, "$'a\\tb' $'\\x41\\101' $'it\\'s'"), "[a\tb][AA][it's]");
  EXPECT_EQ(show(sh, "\"$'x'\""), "[$'x']");
}

// ---- globbing ---------------------------------------------------------------------

struct GlobTree {
  TempDir     dir;
  std::string d;
  GlobTree() : d{dir.path.string()} {
    for (auto n : {"a1.txt", "a2.txt", "a10.txt", "b.txt", "c.log", ".hidden"})
      dir.file(n, "", kFile);
    fs::create_directories(dir.path / "sub" / "deep");
    dir.file("sub/x.txt", "", kFile);
    dir.file("sub/deep/y.txt", "", kFile);
  }
};

TEST(Subst, Globbing) {
  GlobTree t;
  Shell sh;
  sh.params.assign("d", t.d, PM_SCALAR);
  auto rel = [&](std::string s) {
    for (std::size_t p; (p = s.find(t.d + "/")) != std::string::npos;) s.erase(p, t.d.size() + 1);
    return s;
  };
  EXPECT_EQ(rel(out(sh, "echo $d/*.txt")), "a1.txt a10.txt a2.txt b.txt\n");
  EXPECT_EQ(rel(out(sh, "echo $d/a?.txt $d/[bc].*")), "a1.txt a2.txt b.txt c.log\n");
  EXPECT_EQ(rel(out(sh, "echo $d/[!a]*.txt $d/*.(log|nope)")), "b.txt c.log\n");
  EXPECT_EQ(rel(out(sh, "echo $d/a<2-10>.txt")), "a10.txt a2.txt\n");
  EXPECT_EQ(rel(out(sh, "echo $d/*/*.txt $d/**/*.txt")),
            "sub/x.txt a1.txt a10.txt a2.txt b.txt sub/deep/y.txt sub/x.txt\n");
  EXPECT_EQ(rel(out(sh, "echo $d/*(/) $d/s*/")), "sub sub/\n");
  EXPECT_EQ(rel(out(sh, "echo $d/*(.oL[1,2])")), "a1.txt a10.txt\n");
  // Quoted or expanded pattern characters are literal.
  EXPECT_EQ(rel(out(sh, "p='*'; echo \"$d/*\" $d/'*' $d/$p")), "* * *\n");
}

TEST(Subst, GlobOptions) {
  GlobTree t;
  Shell sh;
  sh.params.assign("d", t.d, PM_SCALAR);
  auto r = sh.run("echo $d/*.none");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: no matches found: " + t.d + "/*.none\n");
  EXPECT_EQ(out(sh, "echo x $d/*.none(N) y"), "x y\n");
  sh.opts.unset(OPT_NOMATCH);
  EXPECT_EQ(out(sh, "echo $d/*.none"), t.d + "/*.none\n");
  sh.opts.set(OPT_NULLGLOB);
  EXPECT_EQ(out(sh, "echo x $d/*.none"), "x\n");
  EXPECT_EQ(out(sh, "echo $d/.h* $d/*(D[1])"), t.d + "/.hidden " + t.d + "/.hidden\n");
  sh.opts.set(OPT_GLOBSUBST);
  EXPECT_EQ(out(sh, "p='b.*'; echo $d/$p"), t.d + "/b.txt\n");
  sh.opts.unset(OPT_GLOBSUBST);
  EXPECT_EQ(out(sh, "p='b.*'; echo $d/${~p}"), t.d + "/b.txt\n");
  sh.opts.set(OPT_NUMERICGLOBSORT);
  sh.opts.set(OPT_MARKDIRS);
  EXPECT_EQ(out(sh, "echo $d/a*.txt $d/s*"),
            t.d + "/a1.txt " + t.d + "/a2.txt " + t.d + "/a10.txt " + t.d + "/sub/\n");
  sh.opts.set(OPT_EXTENDEDGLOB);
  sh.opts.unset(OPT_MARKDIRS);
  EXPECT_EQ(out(sh, "echo $d/^*.txt"), t.d + "/c.log " + t.d + "/sub\n");
  EXPECT_EQ(out(sh, "echo $d/*.txt~*/a*"), t.d + "/b.txt\n");
}

TEST(Subst, LoneBracketIsNotAPattern) {
  Shell sh;
  EXPECT_EQ(show(sh, "[ a ]"), "[[][a][]]");
}

// ---- here-documents, here-strings and redirections ------------------------------

TEST(Subst, HeredocBodyIsExpanded) {
  Shell sh;
  sh.run("name=world");
  EXPECT_EQ(out(sh, "cat <<EOF\nhi $name $((1+1)) $(echo c) \\$name 'q' \"dq\"\nEOF\n"),
            "hi world 2 c $name 'q' \"dq\"\n");
  EXPECT_EQ(out(sh, "cat <<'EOF'\nhi $name\nEOF\n"), "hi $name\n");
  EXPECT_EQ(out(sh, "cat <<\\EOF\nhi $name\nEOF\n"), "hi $name\n");
  EXPECT_EQ(out(sh, "cat <<< \"here $name\""), "here world\n");
}

TEST(Subst, HeredocDoesNotSwallowTheNextLine) {
  Shell sh;
  EXPECT_EQ(out(sh, "cat <<A | tr a-z A-Z\none\nA\necho after\n"), "ONE\nafter\n");
  EXPECT_EQ(out(sh, "cat <<A; cat <<B\none\nA\ntwo\nB\n"), "one\ntwo\n");
}

TEST(Subst, RedirectionTargetsAreExpanded) {
  TempDir dir;
  Shell sh;
  sh.params.assign("d", dir.path.string(), PM_SCALAR);
  EXPECT_EQ(sh.run("n=out; echo hi > $d/$n.txt").rc, 0);
  EXPECT_EQ(out(sh, "cat $d/out.txt"), "hi\n");
  EXPECT_EQ(sh.run("echo two > $d/{p,q}").rc, 0);   // MULTIOS writes both
  EXPECT_EQ(out(sh, "cat $d/p $d/q"), "two\ntwo\n");
  EXPECT_EQ(out(sh, "cat < $d/o*.txt"), "hi\n");
  EXPECT_EQ(out(sh, "fd=1; echo dup >&$fd"), "dup\n");
}

TEST(Subst, ProcessSubstitutionIsLeftToTheExecutor) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=hi; cat <(echo $x)"), "hi\n");
}

TEST(Subst, XtraceShowsExpandedWords) {
  Shell sh;
  sh.opts.set(OPT_XTRACE);
  auto r = sh.run("x='a b'; echo $x {1,2}");
  EXPECT_EQ(r.err, "+ x='a b'\n+ echo 'a b' 1 2\n");
}

TEST(Subst, BadSubstitutionIsAnError) {
  Shell sh;
  EXPECT_EQ(sh.run("echo ${").rc, 1);
  auto r = sh.run("echo ${x!y}");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: bad substitution\n");
}

TEST(Subst, RemoveQuotes) {
  EXPECT_EQ(remove_quotes("a'b c'\"d\\\"e\"\\ f$'\\t'"), "ab cd\"e f\t");
}

}  // namespace
}  // namespace jfsh
