// Word expansion (M3): parameters, command and arithmetic substitution,
// splitting, braces, tilde, globbing and quote handling, run end to end
// through Executor::exec_string.

#include <pwd.h>
#include <unistd.h>

#include <clocale>

#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/subst.h>
#include <shell_fixture.h>

namespace jfsh {
namespace {

using test::Shell;
using test::TempDir;
namespace sf = s::filesystem;

constexpr sf::perms kFile = sf::perms::owner_read | sf::perms::owner_write;

// Runs `cmd` and returns its stdout.  `show` prints each argument in
// brackets so word boundaries are visible: show a 'b c' → "[a][b c]".
str out(Shell& sh, strv cmd) {
  auto r = sh.run(cmd);
  EXPECT_EQ(r.err, "") << cmd;
  return r.out;
}

str show(Shell& sh, strv args) {
  return out(sh, "env printf '[%s]' " + str(args));
}

// ---- parameters --------------------------------------------------------------

TEST(Subst, ScalarsAndQuotes) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=hello; echo $x ${x} \"$x\" '$x' \\$x"),
            "hello hello hello $x $x\n");
  EXPECT_EQ(out(sh, "x=1; echo \"${x}x\" \"$x\"y $x'z' a$x$x"), "1x 1y 1z a11\n");
  // print -r: echo would take `\b` for a backspace, as zsh's does.
  EXPECT_EQ(out(sh, "print -r -- \"a\\$b\" \"a\\\\b\" 'a\\b' a\\ b"), "a$b a\\b a\\b a b\n");
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
  sh.params.assign("h", umap<str, str>{{"k1", "v1"}, {"k2", "v2"}},
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
            "[a][b][c][a][b][c][a][b][][c]");
  EXPECT_EQ(out(sh, "n=(10 9 100); echo ${(on)n} ${(On)n}"), "9 10 100 100 10 9\n");
  EXPECT_EQ(out(sh, "q='a b'; echo ${(q)q} ${(qq)q} ${(Q)${(qq)q}}"), "a\\ b 'a b' a b\n");
  EXPECT_EQ(out(sh, "ref=x; echo ${(P)ref} ${(t)x} ${(t)a}"), "abc scalar array\n");
}

TEST(Subst, NestedSubstitutions) {
  Shell sh;
  sh.run("x=abc");
  EXPECT_EQ(out(sh, "echo ${${x#a}%c} ${#${x}}"), "b 3\n");
  EXPECT_EQ(show(sh, "${(f)\"$(env printf 'l 1\\nl2')\"}"), "[l 1][l2]");
  EXPECT_EQ(show(sh, "\"${(f)$(env printf 'l1\\nl2')}\""), "[l1][l2]");
}

TEST(Subst, PositionalParameters) {
  Shell sh;
  sh.ctx.arg0       = "script";
  sh.ctx.positional = {"a b", "c", "d", "e", "f", "g", "h", "i", "j", "tenth"};
  EXPECT_EQ(show(sh, "$0 $# $1 ${10} $10"), "[script][10][a b][tenth][tenth]");
  sh.opts.set(OPT_KSH_ARRAYS);
  EXPECT_EQ(show(sh, "$10 ${10}"), "[a b0][tenth]");
  sh.opts.unset(OPT_KSH_ARRAYS);
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
  EXPECT_EQ(out(sh, "echo $$"), s::to_string(getpid()) + "\n");
  EXPECT_EQ(out(sh, "echo $!"), "0\n");
  EXPECT_EQ(out(sh, "echo $(echo $$)"), s::to_string(getpid()) + "\n");
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
  EXPECT_EQ(show(sh, "${=x}"), "[a][b][][c]");
  EXPECT_EQ(ifs_split("a:b::c:", ":"), (vec<str>{"a", "b", "", "c", ""}));
  EXPECT_EQ(ifs_split("  a  b ", " \t\n"), (vec<str>{"a", "b"}));
  EXPECT_EQ(ifs_split("a : b", " :"), (vec<str>{"a", "b"}));
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
  EXPECT_NE(sh.run("echo $((1+))").err.find("bad math expression"), str::npos);
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
  sh.params.assign("HOME", str("/home/t"), PM_SCALAR);
  sh.params.assign("PWD", str("/w"), PM_SCALAR);
  EXPECT_EQ(out(sh, "echo ~ ~/x ~+ a~ '~' \\~ \"~\""), "/home/t /home/t/x /w a~ ~ ~ ~\n");
  sh.run("p=~/a:~/b; q=x~");
  EXPECT_EQ(sh.get("p"), "/home/t/a:/home/t/b");
  EXPECT_EQ(sh.get("q"), "x~");
  struct passwd* root = getpwnam("root");
  ASSERT_NE(root, nullptr);
  EXPECT_EQ(out(sh, "echo ~root"), str(root->pw_dir) + "\n");  // /var/root on macOS
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
  TempDir dir;
  str     d;
  GlobTree() : d{dir.path.string()} {
    for (auto n : {"a1.txt", "a2.txt", "a10.txt", "b.txt", "c.log", ".hidden"})
      dir.file(n, "", kFile);
    sf::create_directories(dir.path / "sub" / "deep");
    dir.file("sub/x.txt", "", kFile);
    dir.file("sub/deep/y.txt", "", kFile);
  }
};

TEST(Subst, Globbing) {
  GlobTree t;
  Shell sh;
  sh.params.assign("d", t.d, PM_SCALAR);
  auto rel = [&](str s) {
    for (s::size_t p; (p = s.find(t.d + "/")) != str::npos;) s.erase(p, t.d.size() + 1);
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
  EXPECT_EQ(r.err, "+jfsh:1> x='a b' \n+jfsh:1> echo 'a b' 1 2\n");
}

TEST(Subst, BadSubstitutionIsAnError) {
  Shell sh;
  EXPECT_EQ(sh.run("echo ${").rc, 1);
  auto r = sh.run("echo ${x!y}");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: bad substitution\n");
}

// ---- M3 review fixes (expected values from zsh 5.9) ---------------------------

// Runs `cmd` and returns its stderr, for the expected errors.
str err(Shell& sh, strv cmd) { return sh.run(cmd).err; }

TEST(SubstReview, SplitFlagsInDoubleQuotesKeepWordsApart) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=a,b; printf '[%s]' \"${(s:,:)x}\" \"${(@s:,:)x}\""), "[a][b][a][b]");
  EXPECT_EQ(out(sh, "x='b a'; printf '[%s]' \"${${(s: :)x}[2]}\" \"${(j:,:)${(s: :)x}}\" "
                    "\"${${(s: :)x}}\" \"${#${(s: :)x}}\""),
            "[a][b,a][b a][2]");
  // Empty pieces: kept with (@), kept at the ends without it, dropped
  // unquoted.
  EXPECT_EQ(out(sh, "x='a,,b,'; a=(\"${(@s:,:)x}\"); b=(\"${(s:,:)x}\"); c=(${(s:,:)x}); "
                    "echo ${#a} ${#b} ${#c}"),
            "4 3 2\n");
  EXPECT_EQ(out(sh, "a=(\"${(@f)$(printf 'a\\n\\nb\\n\\n')}\"); echo ${#a}"), "3\n");
  EXPECT_EQ(out(sh, "x=' a  b '; printf '[%s]' \"${=x}\" \"p${=x}q\" ${=x}"),
            "[][a][b][][p][a][b][q][a][b]");
  // An array is joined before it is split, unless (@) is given.
  EXPECT_EQ(out(sh, "a=('x,y' z); printf '[%s]' ${(s:,:)a} \"${(@s:,:)a}\""),
            "[x][y z][x][y][z]");
}

TEST(SubstReview, NonWhitespaceIfsKeepsEmptyFields) {
  Shell sh;
  EXPECT_EQ(out(sh, "IFS=:; x=a::b; printf '[%s]' $=x $(echo :a:) ${=:-a:}"),
            "[a][][b][][a][][a]");
  EXPECT_EQ(out(sh, "IFS=:; x=a::; y=($=x); echo ${#y}"), "3\n");
  EXPECT_EQ(out(sh, "IFS=:; setopt shwordsplit; x=a::b; for i in $x; do echo \"<$i>\"; done"),
            "<a>\n<>\n<b>\n");
  // Empty array elements stay when glued to text.
  Shell fresh;
  EXPECT_EQ(out(fresh, "a=(x '' y); printf '[%s]' $a -${a}- \"${a[@]}\""),
            "[x][y][-x][y-][x][][y]");
}

TEST(SubstReview, OffsetAndLengthDoNotOverflow) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(a b c d); echo ${a:1:9223372036854775807} "
                    "${a: -9223372036854775807:1} ${a[-9,2]}x"),
            "b c d a x\n");
  EXPECT_EQ(err(sh, "x=abcdef; echo ${x:2:-10}"), "jfsh: substring expression: -4 < 2\n");
  EXPECT_EQ(err(sh, "x=abc; echo ${x:y}"), "jfsh: unrecognized modifier `y'\n");
}

TEST(SubstReview, BraceSequences) {
  Shell sh;
  // A stepped character range is not expanded, whatever the step.
  EXPECT_EQ(out(sh, "printf '[%s]' {a..c..9223372036854775807} {1..10..-3} {1..2..0} "
                    "{a..e..2}"),
            "[{a..c..9223372036854775807}][10][7][4][1][1..2..0][{a..e..2}]");
  EXPECT_EQ(out(sh, "printf '[%s]' {a,} {,}"), "[a][][][]");
  sh.opts.set(OPT_BRACECCL);
  EXPECT_EQ(out(sh, "printf '[%s]' {a-d} {z-a}"), "[a][b][c][d][-][a][z]");
}

TEST(SubstReview, OnlyBracesInTheScriptAreActive) {
  Shell sh;
  EXPECT_EQ(out(sh, "n=3; x='{1..3}'; printf '[%s]' {1..$n} ${~x} {%..+}"),
            "[1][2][3][{1..3}][%][&]['][(][)][*][+]");
  sh.opts.set(OPT_GLOBSUBST);
  EXPECT_EQ(out(sh, "printf '[%s]' $(echo '{a,b}')"), "[{a,b}]");
  // A value cannot make the shell build a huge list.
  EXPECT_EQ(out(sh, "x='{1..100000000}'; echo $x"), "{1..100000000}\n");
}

TEST(SubstReview, ManyBracesStayLinear) {
  Shell sh;
  str open(20000, '{');
  EXPECT_EQ(out(sh, "echo " + open), open + "\n");
  str nested;
  for (int k = 0; k < 2000; ++k) nested += "{a,";
  nested += str(2000, '}');
  EXPECT_EQ(out(sh, "a=(" + nested + "); echo ${#a}"), "2001\n");
}

TEST(SubstReview, DollarBracketStaysOneWord) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $[1 + 2] $[3 * 4]; x=$[3 * 4]; echo $x"), "3 12\n12\n");
}

TEST(SubstReview, SubstitutionWithoutAName) {
  Shell sh;
  EXPECT_EQ(out(sh, "printf '[%s]' ${:-foo} ${(U):-foo} ${#:-abc} ${${:-a/b}:t}"),
            "[foo][FOO][3][b]");
  EXPECT_EQ(err(sh, "echo ${:=foo}"), "jfsh: not an identifier: \n");
}

TEST(SubstReview, QuotesInsideNestedCommandSubstitution) {
  Shell sh;
  EXPECT_EQ(out(sh, "printf '[%s]' ${(f)\"$(printf 'a\\nb')\"} ${\"$(echo \"a\")\"} "
                    "${#${:-\"$(echo \"ab\")\"}}"),
            "[a][b][a][2]");
}

TEST(SubstReview, IndirectionOnNestedValuesAndSubscripts) {
  Shell sh;
  EXPECT_EQ(out(sh, "n=foo_v; foo_v=1; x='a[2]'; a=(p q); y='h[k]'; typeset -A h; "
                    "h=(k v); printf '[%s]' ${(P)${n}} ${(P)x} ${(P)y}"),
            "[1][q][v]");
  // Unlike zsh, a subscript in the value never runs a command.
  auto r = sh.run("x='a[$(echo ran >&2)1]'; a=(p q); echo \"<${(P)x}>\"");
  EXPECT_EQ(r.out, "<>\n");
  EXPECT_EQ(r.err, "");
}

TEST(SubstReview, NoUnsetCoversPositionalElementsAndArithmetic) {
  Shell sh;
  sh.opts.unset(OPT_UNSET);
  auto r = sh.run("echo $1; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: 1: parameter not set\n");
  EXPECT_EQ(err(sh, "a=(1); echo ${a[5]}"), "jfsh: a[5]: parameter not set\n");
  EXPECT_EQ(err(sh, "typeset -A h; echo ${h[z]}"), "jfsh: h[z]: parameter not set\n");
  EXPECT_EQ(err(sh, "echo $((zz+1))"), "jfsh: zz: parameter not set\n");
  EXPECT_EQ(out(sh, "a=(1); echo ${a[5]-d} $((a[2]))"), "d 0\n");
  sh.opts.set(OPT_UNSET);
  EXPECT_EQ(err(sh, "y=; echo ${y:?}"), "jfsh: y: parameter not set\n");
}

TEST(SubstReview, QuotingSurvivesEval) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=$'a\\nb'; eval \"y=${(q)x}\"; [[ $y == $x ]] && echo same"), "same\n");
  EXPECT_EQ(out(sh, "x=; eval \"set -- ${(q)x}\"; echo $#"), "1\n");
  EXPECT_EQ(out(sh, "x=$'a\\tb\\x01'; print -r -- ${(q)x} ${(qqq)x} ${(qqqq)x} ${(q+)x} ${(q-)x}"),
            "a$'\\t'b$'\\001' \"a\tb\x01\" $'a\\tb\\001' $'a\\tb\\C-A' 'a\tb\x01'\n");
  EXPECT_EQ(out(sh, "x=\"a b'c\"; print -r -- ${(q)x} ${(qqq)x} ${(qqqq)x} ${(q-)x} ${(q+)x}"),
            "a\\ b\\'c \"a b'c\" $'a b\\'c' 'a b'\\'c 'a b'\\''c'\n");
  EXPECT_EQ(out(sh, "x=$'a\\tb\\x01'; for f in q qq qqq qqqq q- q+; do "
                    "eval \"y=\\${(${f})x}\"; eval \"z=$y\"; [[ $z == $x ]] || echo $f; done"),
            "");
}

TEST(SubstReview, ControlAndMetaEscapes) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=$'\\C-a\\M-a\\M-\\C-a\\c['; echo ${#x} $(( #x ))"), "5 1\n");
  EXPECT_EQ(out(sh, "x=$'\\M-\\C-a'; [[ $x == $'\\x81' ]] && echo ok"), "ok\n");
}

TEST(SubstReview, ArithmeticTakesDoubleQuotes) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $((\"1\"+2)) $(( \"1 + 2\" * 2 ))"), "3 5\n");
  EXPECT_EQ(err(sh, "echo $(( '1' + 2 ))"), "jfsh: bad math expression: illegal character: '\n");
}

TEST(SubstReview, HeadAndTailIgnoreTrailingSlashes) {
  Shell sh;
  EXPECT_EQ(out(sh, "for x in a/ a/b/ / a/b/c; do printf '[%s]' ${x:h} ${x:t} ${x:h2} ${x:t2}; done"),
            "[.][a][a/][a][a][b][a/b/][a/b][/][/][a/b][c][a/b][b/c]");
}

TEST(SubstReview, ScalarContextsJoinWithIfs) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(1 2); IFS=:; x=$a; y=$a[@]; echo $x $y; cat <<< $a"), "1:2 1:2\n1:2\n");
}

TEST(SubstReview, HeredocExpansionErrorFailsOnlyTheCommand) {
  Shell sh;
  auto r = sh.run_script("cat <<E\n${x?m}\nE\necho rc=$?\n");
  EXPECT_EQ(r.out, "rc=1\n");
  EXPECT_EQ(r.err, "jfsh: x: m\n");
}

TEST(SubstReview, SortFlags) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(3 1 2); b=(x10 x2 x1); c=(b B a A); print ${(n)a} / ${(on)b} / ${(i)c}"),
            "1 2 3 / x1 x2 x10 / a A b B\n");
}

TEST(SubstReview, EmptyMatchesInReplacement) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=abc; printf '[%s]' ${x///X} ${x//b#/-}"), "[XaXbXc][abc]");
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "x=abc; printf '[%s]' ${x//c#/-}"), "[-a-b-]");
}

TEST(SubstReview, KeysTypesAndShellWords) {
  Shell sh;
  EXPECT_EQ(out(sh, "typeset -A h; h=(a 1 b 2); a=(x y z); printf '[%s]' ${(k)h[a]} ${(k)a[2]}"),
            "[a][2]");
  EXPECT_EQ(out(sh, "x='y=(1 2) z'; printf '[%s]' ${(z)x}; x='y+=(a (b) c)'; printf '[%s]' ${(z)x}"),
            "[y=(][1][2][)][z][y+=(][a][(b)][c][)]");
  EXPECT_EQ(out(sh, "echo ${(t)@} ${(t)?}"), "array-readonly-special integer-readonly-special\n");
}

TEST(SubstReview, ArrayNullTestInDoubleQuotes) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(''); b=('' ''); printf '[%s]' \"${a:-e}\" \"${b:-e}\""), "[e][ ]");
}

TEST(SubstReview, KshArraysNameIsFirstElement) {
  Shell sh;
  sh.opts.set(OPT_KSH_ARRAYS);
  EXPECT_EQ(out(sh, "a=(xyz b c); echo $a ${#a} ${a[1]}"), "xyz 3 b\n");
}

TEST(SubstReview, EqualsAndTildeTakeQuotedNames) {
  Shell sh;
  str ls = out(sh, "whence -p ls");
  EXPECT_EQ(out(sh, "x=ls; echo =$x; echo =\"ls\""), ls + ls);
  EXPECT_EQ(err(sh, "echo =ls/x"), "jfsh: ls/x not found\n");
  if (auto* pw = ::getpwnam("root"))
    EXPECT_EQ(out(sh, "echo ~\"root\" ~'ro'ot"), str(pw->pw_dir) + " " + pw->pw_dir + "\n");
  EXPECT_EQ(out(sh, "(unset HOME; echo \"[\" ~ \"]\")"), "[  ]\n");
}

TEST(SubstReview, ElementsAreReadInPlace) {
  Shell sh;
  // Quadratic before: each element access copied the whole array.
  EXPECT_EQ(out(sh, "typeset -A h; for ((i=0;i<20000;i++)); do h[k$i]=$i; done; "
                    "a=({1..20000}); s=0; for ((i=0;i<20000;i++)); do "
                    "(( s += a[i+1] + ${h[k$i]} )); done; echo $s"),
            "400000000\n");
  // A failed element assignment leaves the array as it was.
  auto r = sh.run("a=(1 2); (( a[0] = 3 )); echo $a");
  EXPECT_EQ(r.out, "1 2\n");
  EXPECT_EQ(r.err, "jfsh: a: assignment to invalid subscript range\n");
}

TEST(SubstReview, LongSubjectsDoNotRecursePerCharacter) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "x=${(l:200000::a:)e}; [[ $x == a## ]] && echo y; "
                    "p=${(l:100000::?:)e}; [[ ${x[1,100000]} == $~p ]] && echo y; "
                    "[[ $x == $~p ]] || echo n"),
            "y\ny\nn\n");
  EXPECT_EQ(out(sh, "x=${(l:30000::ab:)e}; y=${x//a/b}; echo ${#y} ${#${x#*b}} ${#${x##*b}}"),
            "30000 29998 0\n");
}

TEST(SubstReview, DeepNestingFailsCleanly) {
  auto nest = [](strv open, strv close, int n, strv mid) {
    str o, c;
    for (int k = 0; k < n; ++k) { o += open; c += close; }
    return o + str(mid) + c;
  };
  {
    // The depths that worked before the limits keep working on an 8 MB
    // stack (zsh's limits are higher still).
    Shell sh;
    EXPECT_EQ(out(sh, "x=a; echo " + nest("${", "}", 1100, "x")), "a\n");
    EXPECT_EQ(out(sh, "x=a; echo " + nest("\"${", "}\"", 600, "x")), "a\n");
    EXPECT_EQ(out(sh, "x=1; for i in {1..1000}; do x=\"($x)\"; done; echo $((x))"), "1\n");
    str some = str(1100, '(') + "a" + str(1100, ')');
    EXPECT_EQ(out(sh, "p='" + some + "'; [[ a == $~p ]] && echo yes"), "yes\n");
  }
  // On a 1 MB stack the same nesting, deeper, fails with an error.
  test::on_stack(1 << 20, [&] {
    Shell sh;
    auto r = sh.run("x=a; echo " + nest("${", "}", 5000, "x"));
    EXPECT_EQ(r.err, "jfsh: substitutions nested too deeply\n");
    r = sh.run("x=a; echo " + nest("\"${", "}\"", 5000, "x"));
    EXPECT_EQ(r.err, "jfsh: substitutions nested too deeply\n");
    str ao(5000, '('), ac(5000, ')');
    r = sh.run("x='" + ao + "1" + ac + "'; echo $((x))");
    EXPECT_EQ(r.err, "jfsh: stack overflow\n");
    r = sh.run("echo " + nest("$((", "))", 5000, "1"));
    EXPECT_EQ(r.err, "jfsh: substitutions nested too deeply\n");
    // A pattern nested past the stack is literal text as a whole.
    str deep = str(5000, '(') + "a" + str(5000, ')');
    EXPECT_EQ(out(sh, "p='" + deep + "'; [[ a == $~p ]] || echo no; [[ $p == $~p ]] && echo self"),
              "no\nself\n");
    str braces = nest("{a,", "}", 5000, "b");
    r = sh.run("echo " + braces + " | wc -c");
    EXPECT_EQ(r.rc, 0);
  });
}

TEST(SubstReview, SmallStackStillExpands) {
  // With a 256 KB stack (`ulimit -s 256`) ordinary nesting still works.
  test::on_stack(256 << 10, [] {
    Shell sh;
    EXPECT_EQ(out(sh, "print hi; x=$(echo a); print ${x:-y} {a,b} $((1+(2*3))) "
                      "\"${${x}:-z}\" ${(U)${x}}"),
              "hi\na a b 7 a A\n");
  });
}

// Sets LC_CTYPE (or LC_COLLATE) to a UTF-8 locale for one test.
struct Utf8Locale {
  explicit Utf8Locale(int cat) : category{cat}, saved{s::setlocale(cat, nullptr)} {
    for (const char* name : {"en_US.UTF-8", "C.UTF-8", "en_US.utf8"})
      if (s::setlocale(cat, name)) { ok = true; break; }
  }
  ~Utf8Locale() { s::setlocale(category, saved.c_str()); }

  int  category;
  str  saved;
  bool ok = false;
};

TEST(SubstReview, MultibyteCountsCharacters) {
  Utf8Locale loc{LC_CTYPE};
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  EXPECT_EQ(out(sh, "x=héllo; print ${#x} ${x:1:1} ${x[2]} ${x#h?} ${x[2,3]} ${(U)x} ${x:u}"),
            "5 é é llo él HÉLLO HÉLLO\n");
  EXPECT_EQ(out(sh, "[[ é == ? ]] && [[ é == [é] ]] && echo y"), "y\n");
  EXPECT_EQ(out(sh, "x=héllo; print \"[${(l:7:)x}]\" ${#${(s::)x}} ${(c)#x}"), "[  héllo] 5 5\n");
  EXPECT_EQ(out(sh, "w=éa; x=héllo; print ${w//[^é]/X} ${w%[^a]} ${x[(i)l]}"),
            "éX éa 3\n");
  sh.opts.unset(OPT_MULTIBYTE);
  EXPECT_EQ(out(sh, "x=héllo; print ${#x}"), "6\n");
}

TEST(SubstReview, SortingFollowsTheLocale) {
  Utf8Locale loc{LC_COLLATE};
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  EXPECT_EQ(out(sh, "a=(B a C b); print ${(o)a}"), "a b B C\n");
}

TEST(SubstReview, GlobRecursiveDirectoriesAndBrackets) {
  GlobTree t;
  Shell sh;
  sh.params.assign("d", t.d, PM_SCALAR);
  EXPECT_EQ(out(sh, "(cd $d; echo **/ ***/; echo sub/**/; echo [!]*.log)"),
            "sub/ sub/deep/ sub/ sub/deep/\nsub/ sub/deep/\nc.log\n");
  auto r = sh.run("(cd $d; echo [])");
  EXPECT_EQ(r.err, "jfsh: no matches found: []\n");
  r = sh.run("echo [a; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: bad pattern: [a\n");
  EXPECT_EQ(out(sh, "[ x = x ] && echo test"), "test\n");
}

TEST(SubstReview, NoMultiosTargetIsOneLiteralWord) {
  GlobTree t;
  Shell sh;
  sh.params.assign("d", t.d, PM_SCALAR);
  sh.opts.unset(OPT_MULTIOS);
  EXPECT_EQ(out(sh, "(cd $d; echo hi > a*; cat 'a*'; a=(p q); echo ho > $a; cat 'p q')"),
            "hi\nho\n");
}

TEST(SubstReview, PatternOperatorsInDoubleQuotes) {
  Shell sh;
  // The pattern of # % / is not double-quoted text, though the whole
  // substitution is; its own quotes still quote.
  EXPECT_EQ(out(sh, "x=a/b.c; print -r -- \"${x#*/}\" \"${x%.*}\" \"${x##*/}\" \"${x%%/*}\" "
                    "\"${x/b/X}\" \"${x//[bc]/Y}\" \"${x/#a/Z}\" \"${x/%c/W}\""),
            "b.c a/b b.c a a/X.c a/Y.Y Z/b.c a/b.W\n");
  EXPECT_EQ(out(sh, "z='a*b'; print -r -- \"${z#\"a*\"}\" \"${z/\"*\"/S}\" \"${z#a\\*}\" \"${z#'a*'}\""),
            "b aSb b b\n");
  EXPECT_EQ(out(sh, "x=a/b.c; y=\"${x#*/}\"; print -r -- $y; echo $(( ${#${x#*/}} )); "
                    "[[ \"${x#*/}\" == b.c ]]; echo $?"),
            "b.c\n3\n0\n");
  EXPECT_EQ(out(sh, "x=a/b.c; cat <<E\n${x#*/} ${x%.*}\nE"), "b.c a/b\n");
  // The replacement is double-quoted text there.
  EXPECT_EQ(out(sh, "x=abc; z='a*b'; print -r -- \"${x/a/\"q\"}\" \"${x/a/\\\"}\" \"${x/a/'q'}\" "
                    "\"${x/a/$z}\" \"${x/a/*}\""),
            "qbc \"bc 'q'bc a*bbc *bc\n");
}

TEST(SubstReview, AnchorsAfterDoubleSlashAndDqFilters) {
  Shell sh;
  EXPECT_EQ(out(sh, "y=aaaa; echo ${y//#/Q} ${y//%/Q} ${y//#a/Q} ${y//%a/Q}"),
            "Qaaaa aaaaQ Qaaa aaaQ\n");
  EXPECT_EQ(out(sh, "a=(x.c y.h); printf '[%s]' \"${a:#*.c}\" \"${(M)a:#*.c}\" \"${(@)a:#*.c}\""),
            "[x.c y.h][][y.h]");
  sh.opts.set(OPT_KSH_ARRAYS);
  EXPECT_EQ(out(sh, "a=(x y z); b=a; echo ${(P)b}"), "x\n");
}

TEST(SubstReview, ConsecutiveBracesStayLinear) {
  Shell sh;
  str w = "{x,}";
  for (int k = 0; k < 20000; ++k) w += "{1..1}";
  str ones(20000, '1');
  EXPECT_EQ(out(sh, "echo " + w), "x" + ones + " " + ones + "\n");
}

TEST(SubstReview, NoBadPatternKeepsTheWord) {
  Shell sh;
  sh.opts.unset(OPT_BADPATTERN);
  EXPECT_EQ(out(sh, "echo foo[bar; echo b"), "foo[bar\nb\n");
}

TEST(SubstReview, BackslashesInDoubleQuotedOperands) {
  Shell sh;
  // In "${…}" the operand's own level also takes `\}`; a "…" inside it is
  // an ordinary double-quoted region, where `\}` stays.
  sh.run("x=abc");
  EXPECT_EQ(out(sh, "print -r -- \"1 ${y:-\\}} ${y:-\\\\} ${y:-\\$} ${y:-\\`} ${y:-\\\"} "
                    "${y:-\\a} ${y:-'q'} ${y:-\\'}\""),
            "1 } \\ $ ` \" \\a 'q' \\'\n");
  EXPECT_EQ(out(sh, "print -r -- \"2 ${y:-\"\\}\"} ${y:-\"\\\\\"} ${y:-\"\\$\"} ${y:-\"\\a\"} "
                    "${y:-\"\\\"\"} ${y:-\"'\"}\""),
            "2 \\} \\ $ \\a \" '\n");
  EXPECT_EQ(out(sh, "print -r -- \"3 ${x/b/\\}} ${x/b/\\\\} ${x/b/\\$} ${x/b/\\a} ${x/b/'q'}\" "
                    "\"${x/b/\"\\}\"}\" \"${x/b/x\\}y}\" \"${x/b/\\\\\\}}\""),
            "3 a}c a\\c a$c a\\ac a'q'c a\\}c ax}yc a\\}c\n");
  EXPECT_EQ(out(sh, "cat <<E\n${y:-\\}} ${x/b/\\}} ${x/b/\"\\}\"} ${x/b/'q'}\nE"),
            "} a}c a\\}c a'q'c\n");
  EXPECT_EQ(out(sh, "HOME=/hh; print -r -- ${x/b/~} \"${x/b/~}\""), "a/hhc a~c\n");
}

TEST(SubstReview, SingleQuotesInDoubleQuotedOperands) {
  Shell sh;
  // There a single quote quotes only when closed, and the closing `}` of
  // the substitution is not looked for inside one.
  EXPECT_EQ(out(sh, "x=a/b; print -r -- \"${x/'/'/X}\"; x=\"a'b'c\"; "
                    "print -r -- \"${x#*'b}\" \"${x//\\'/Q}\"; z='a*b'; "
                    "print -r -- \"${z#'a*'}\" \"${z/'*'/S}\""),
            "a/b\n'c aQbQc\nb aSb\n");
  EXPECT_EQ(out(sh, "print -r -- \"${y:-'}'}\" \"${y:-'a}\"; print -r -- \"${y:-$(echo 'p}q')}\""),
            "''} 'a\np}q\n");
}

TEST(SubstReview, BraceWordsZshLeavesAlone) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo {{{a,b} {a{b{c,d} {{{1..3} x1..2{{b,{,} {{a,b} {+1..3} {1..+3} "
                    "{1..3..} {..3} {1..} {01..100..33}"),
            "{{{a,b} {a{b{c,d} {{{1..3} x1..2{{b,{,} {a {b {+1..3} {1..+3} 1..3.. ..3 1.. "
            "01 34 67 100\n");
}

TEST(SubstReview, FlagArgumentsAreText) {
  Shell sh;
  sh.run("x=\"a'b\"; a=(1 2)");
  EXPECT_EQ(out(sh, "print -r -- \"${(s:':)x}\" \"${(j:':)a}\" \"${(l:5::':)x}\" \"${(s.'.)x}\""),
            "a b 1'2 ''a'b a b\n");
  EXPECT_EQ(out(sh, "x='a)b'; print -r -- ${(s:):)x}; x='a(b'; print -r -- ${(s:(:)x}"),
            "a b\na b\n");
  // A word that stays one word is not split at its top level.
  EXPECT_EQ(out(sh, "x=a,b; y=${(s:,:)x}; z=\"${(s:,:)x}\"; print -r -- $y $z "
                    "${${(s:,:)x}[2]}; [[ ${(s:,:)x} == a,b ]] && echo m"),
            "a,b a,b b\nm\n");
}

TEST(SubstReview, QuestionMessageIsTheWordAsWritten) {
  Shell sh;
  EXPECT_EQ(sh.run("echo \"${x:?'boom'}\"").err, "jfsh: x: 'boom'\n");
  EXPECT_EQ(sh.run("y=Q; echo ${x:?$y $(echo hi)}").err, "jfsh: x: $y $(echo hi)\n");
}

TEST(SubstReview, UnmatchedClosingBracesArePlain) {
  Shell sh;
  EXPECT_EQ(out(sh, "print -r -- \\{2{}..'}'}0a a{}..'}'}-; setopt braceccl; print -r -- ,{3..}}}a{"),
            "{2{}..}}0a a{}..}}-\n,.}}a{ ,3}}a{\n");
}

TEST(SubstReview, OneWordContextsSplitWhereZshDoes) {
  Shell sh;
  sh.run("x=a,b; w='a  b'; z1=1");
  // Operands of :- :+ = are words (zsh's multsub), joined afterwards.
  EXPECT_EQ(out(sh, "y=${z:-${(s:,:)x}}; y2=\"${z:-${(s:,:)x}}\"; y3=${z1:+${(s:,:)x}}; "
                    "y4=${z:-x${(s:,:)x}y}; print -r -- \"[$y][$y2][$y3][$y4]\""),
            "[a b][a b][a b][xa by]\n");
  EXPECT_EQ(out(sh, "cat <<< ${z:-${(s:,:)x}}; [[ ${z:-${(s:,:)x}} == 'a b' ]] && echo m"),
            "a b\nm\n");
  // A replacement or pattern operand is not split.
  EXPECT_EQ(out(sh, "q=${x/b/${(s:,:)x}}; print -r -- \"[$q]\""), "[a,a,b]\n");
  // (z) splits even there, the words joined with IFS's first character.
  EXPECT_EQ(out(sh, "y=${(z)w}; y2=\"${(z)w}\"; print -r -- \"[$y][$y2]\"; cat <<< ${(z)w}; "
                    "IFS=:; y=${(z)w}; print -r -- \"[$y]\""),
            "[a b][a b]\na b\n[a:b]\n");
  // An unquoted operand drops empty elements.
  EXPECT_EQ(out(sh, "unset IFS; a=(1 '' 2); y=${z:-$a}; print -r -- \"[$y]\""), "[1 2]\n");
}

TEST(SubstReview, NestedEqualsSplits) {
  Shell sh;
  sh.run("x='a b'");
  EXPECT_EQ(out(sh, "printf '[%s]' ${${=x}}; echo; print -r -- ${#${=x}} ${${=x}[2]} "
                    "${(j:-:)${=x}}; a=(${${=x}}); print $#a"),
            "[a][b]\n2 b a-b\n2\n");
  sh.opts.set(OPT_SHWORDSPLIT);
  EXPECT_EQ(out(sh, "print -r -- ${${x}[2]} ${#${x}}"), "b 2\n");
}

TEST(SubstReview, ArrayFlagsAndNulSplitting) {
  Shell sh;
  EXPECT_EQ(out(sh, "print -r -- ${(A)y::=a b c}; print -r -- $#y ${(t)y}; s='1,2,3'; "
                    "print -r -- ${(A)r::=${(s:,:)s}}; print -r -- $#r ${r[2]}"),
            "a b c\n1 array\n1 2 3\n3 2\n");
  EXPECT_EQ(out(sh, "kv=(k1 v1 k2 v2); print -r -- ${(AA)h::=$kv}; print -r -- ${(t)h} ${h[k2]}"),
            "v1 v2\nassociation v2\n");
  EXPECT_EQ(out(sh, "x=$'a\\0b\\0c'; c=(${(0)x}); print -r -- $c $#c"), "a b c 3\n");
  EXPECT_EQ(out(sh, "x=$'a b # d\\nc'; printf '[%s]' ${(Z:c:)x}; x='a # b c'; printf '[%s]' ${(Z:c:)x}"),
            "[a][b][# d][;][c][a][# b c]");
  EXPECT_EQ(out(sh, "x=$'a\\nb;c\\n'; printf '[%s]' ${(Z:n:)x}; x=$'a\\n;b'; printf '[%s]' ${(Z:n:)x}"),
            "[a][b][;][c][a][;][b]");
  EXPECT_EQ(out(sh, "x=$'a # b; c\\nd'; printf '[%s]' ${(Z:C:)x}; x=$'a # b \"c\\nd'; "
                    "printf '[%s]' ${(Z:C:)x}; x='a b'; printf '[%s]' ${(Z::)x}"),
            "[a][;][d][a][;][d][a b]");
  EXPECT_EQ(sh.run("x='a b'; echo ${(Z:q:)x}").err, "jfsh: error in flags\n");
  EXPECT_EQ(out(sh, "y=1; print ${y::=a} $y; x=q; print ${x:=r} ${x::=s} $x"), "a a\nq s s\n");
}

TEST(SubstReview, IndexesAndMessages) {
  Shell sh;
  // An index stops at its first top-level comma, as zsh's getindex.
  EXPECT_EQ(out(sh, "a=(p q r); i=2,3; print ${a[$i]} ${a[${(s:,:)i}]} ${a[2,3]}"), "q q q r\n");
  EXPECT_EQ(sh.run("echo ${x:?a$'\\t'b}").err, "jfsh: x: a$'\\t'b\n");
  EXPECT_EQ(sh.run("t=\"$(printf '\\t')\"; eval \"echo \\${x:?a${t}b}\"").err, "jfsh: x: a\\tb\n");
}

TEST(SubstReview, BraceSequencesSaturate) {
  Shell sh;
  // zsh overflows on these; jfsh stops at the int64 range.
  EXPECT_EQ(out(sh, "print -r -- {-9223372036854775807..-9223372036854775808} / "
                    "{-9223372036854775808..-9223372036854775806..-1} / "
                    "{-3..-9223372036854775808..9223372036854775807}"),
            "-9223372036854775807 -9223372036854775808 / -9223372036854775806 "
            "-9223372036854775807 -9223372036854775808 / -3\n");
}

TEST(SubstReview, SplittingOperandWords) {
  TempDir d;
  for (auto n : {"a", "a1", "a2", "b"}) d.file(n, "", kFile);
  Shell sh;
  sh.params.assign("d", d.path.string(), PM_SCALAR);
  sh.run("cd $d; t=T; v='a  b'");
  // ${=x:-word} and SH_WORD_SPLIT split only unquoted IFS characters, and
  // the pieces still glob and brace-expand.
  EXPECT_EQ(out(sh, "printf '[%s]' ${=t:+a\\ b} ${=t+a*} ${=t:+$v} ${=t:+a b}"),
            "[a b][a][a1][a2][a][b][a][b]");
  sh.opts.set(OPT_SHWORDSPLIT);
  EXPECT_EQ(out(sh, "printf '[%s]' ${t:+a\\ b} ${t:+\\ } ${t:+${t:+\"a b\"}} ${t+a*} ${t+\"a\"*}"),
            "[a b][ ][a b][a][a1][a2][a][a1][a2]");
  EXPECT_EQ(out(sh, "printf '[%s]' ${z-$(echo m){1,2}'p q'}"), "[m1p q][m2p q]");
  EXPECT_EQ(out(sh, "y=x${t:+${t:+$v}}; print -r -- \"<$y>\"; y=${v+${z:-$v}$(echo m)}; "
                    "print -r -- \"<$y>\"; : ${e:=$v}; : ${f::=$v}; print -r -- \"<$e><$f>\""),
            "<xa  b>\n<a  bm>\n<a  b><a  b>\n");
}

TEST(SubstReview, AssignedValueIsOneString) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(p '' 'q r'); printf '[%s]' ${z:=$a}; echo; c='1,,2'; "
                    "y=(x${w=\"${(s:,:)c}\"a  b}y); printf '[%s]' $y"),
            "[p q r]\n[x1 2a  by]");
  EXPECT_EQ(out(sh, ": ${(AA)h=}; print ${#h} ${(t)h}; e=; : ${(AA)h2=$e}; print ${#h2} ${(t)h2}"),
            "0 association\n0 association\n");
  EXPECT_EQ(out(sh, "a=(p q r s); i=2,3; print ${a[1,$i]}; a[$i]=X; print $a"), "p q\np X r s\n");
}

TEST(SubstReview, SplitResultsOfAssigningOperators) {
  Shell sh;
  sh.opts.set(OPT_SHWORDSPLIT);
  // ${^z:=…} keeps its product when the scalar result splits.
  EXPECT_EQ(out(sh, "a=(p q); unset z; printf '[%s]' x${^z:=${a[@]}}y; unset z; "
                    "printf '[%s]' x${^z=$a}y; v='a b'; printf '[%s]' x${^v}y"),
            "[xpy][xqy][xpy][xqy][xay][xby]");
  sh.opts.unset(OPT_SHWORDSPLIT);
  EXPECT_EQ(out(sh, "set -- 'x y' z; unset z; printf '[%s]' x${^=z:=\"$@\"}y"),
            "[xxy][xyy][xzy]");
  // The operand is assigned whole when the result is to be split.
  EXPECT_EQ(out(sh, "v='a  b'; unset z; y=x${=z:=${=v}}; print -r -- \"<$y><$z>\"; unset z; "
                    "[[ x${=z:=${=v}} == 'xa  b' ]] && echo T"),
            "<xa  b><a  b>\nT\n");
  EXPECT_EQ(out(sh, "a=(p 'q r'); unset z; : ${(A)=z:=${a[@]}}; printf '[%s]' $z"), "[p][q][r]");
  // The product keeps the empty pieces of leading and trailing blanks.
  EXPECT_EQ(out(sh, "v=' a '; printf '[%s]' x${^=v}y; v=' a  b '; printf '[%s]' x${^=v}; "
                    "v=$'\\ta\\n'; printf '[%s]' ${^=v}y ${^=v}"),
            "[xy][xay][xy][x][xa][xb][x][y][ay][y][a]");
  EXPECT_EQ(out(sh, "v=''; printf '[%s]' x${^=v}y; v=' '; printf '[%s]' x${^=v}y; "
                    "IFS=:; v='a::b'; printf '[%s]' x${^=v}y; unset IFS"),
            "[xy][xy][xy][xay][xy][xby]");
  // (s) is skipped at the top of the `=` operand of a split result, and
  // applies one level deeper (zsh's PREFORK_SINGLE).
  EXPECT_EQ(out(sh, "v='a,b c'; unset z; printf '[%s]' ${=z:=${(s:,:)v}}; echo \"<$z>\"; "
                    "unset z; printf '[%s]' ${=z:=${${(s:,:)v}}}; echo \"<$z>\""),
            "[a,b][c]<a,b c>\n[a][b][c]<a b c>\n");
  // In double quotes ${=…} splits a plain value, not an operand's.
  EXPECT_EQ(out(sh, "unset z; v='a b'; t=1; printf '[%s]' \"${=z:-$v}\" \"${=t:+$v}\" \"${=v}\""),
            "[a b][a b][a][b]");
  // A quoted empty string in a split operand is a word.
  EXPECT_EQ(out(sh, "t=T; printf '[%s]' ${=t:+a \"\" b} ${=t:+\"\" b} ${=t:+a \"\"\"\" b}"),
            "[a][][b][][b][a][][b]");
}

TEST(SubstReview, CommentsStartOnlyAtTopLevelWords) {
  Shell sh;
  EXPECT_EQ(out(sh, "x='a ${b # c} d'; printf '[%s]' ${(Z:c:)x}; x='a $((1 #2)) d'; "
                    "printf '[%s]' ${(Z:c:)x}; x='a `b # c` d'; printf '[%s]' ${(Z:c:)x}; "
                    "x='a\\ #b c'; printf '[%s]' ${(Z:c:)x}"),
            "[a][${b # c}][d][a][$((1 #2))][d][a][`b # c`][d][a\\ #b][c]");
  auto r = sh.run("a=(1 2 3 4); a[2,]=X");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err, "");
}

TEST(Subst, RemoveQuotes) {
  EXPECT_EQ(remove_quotes("a'b c'\"d\\\"e\"\\ f$'\\t'"), "ab cd\"e f\t");
}

}  // namespace
}  // namespace jfsh
