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
  // On a 1 MB stack moderate nesting works and very deep nesting fails
  // cleanly with an error, wherever the stack runs out.
  test::on_stack(1 << 20, [&] {
    Shell sh;
    auto clean = [](const test::Run& r) {
      return r.err == "jfsh: substitutions nested too deeply\n"
          or r.err == "jfsh: stack overflow\n";
    };
    EXPECT_EQ(out(sh, "x=a; echo " + nest("${", "}", 200, "x")), "a\n");
    EXPECT_EQ(out(sh, "x=a; echo " + nest("\"${", "}\"", 200, "x")), "a\n");
    EXPECT_EQ(out(sh, "echo " + nest("$((", "))", 200, "1")), "1\n");
    constexpr int deep = 100000;
    auto r = sh.run("x=a; echo " + nest("${", "}", deep, "x"));
    EXPECT_TRUE(clean(r)) << r.err;
    r = sh.run("x=a; echo " + nest("\"${", "}\"", deep, "x"));
    EXPECT_TRUE(clean(r)) << r.err;
    str ao(deep, '('), ac(deep, ')');
    r = sh.run("x='" + ao + "1" + ac + "'; echo $((x))");
    EXPECT_TRUE(clean(r)) << r.err;
    r = sh.run("echo " + nest("$((", "))", deep, "1"));
    EXPECT_TRUE(clean(r)) << r.err;
    // A pattern nested past the stack is literal text as a whole.
    str pat = str(deep, '(') + "a" + str(deep, ')');
    EXPECT_EQ(out(sh, "p='" + pat + "'; [[ a == $~p ]] || echo no; [[ $p == $~p ]] && echo self"),
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

// ---- M15c: shell features compsys needs ------------------------------------------

// $+x and $^x without braces.
TEST(Subst15c, UnbracedPlusAndCaret) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=1; a=(p q); echo $+x $+nope ${+a[5]}; echo x$^a"),
            "1 0 0\nxp xq\n");
}

// $x:h, $x:t:r and $x:s/a/b/ on an unbraced name.
TEST(Subst15c, UnbracedModifiers) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=/a/b.c; echo $x:h $x:t:r $x:e ${x}:h $x:s/a/Z/ $x:gs/b/Q/; "
                "a=(/x/y /z/w); echo $a:t"),
            "/a b c /a/b.c:h /Z/b.c /a/Q.c\ny w\n");
}

// (k) with (r)/(R) gives the keys of the values found.
TEST(Subst15c, KeysOfSearchedValues) {
  Shell sh;
  EXPECT_EQ(out(sh, "typeset -A h; h=(a 1 b 2 c 2); "
                "echo ${(k)h[(R)2]} \"|\" ${(k)h[(r)1]} \"|\" ${(kv)h[(R)2]}"),
            "b c | a | b 2 c 2\n");
}

// (#b) sets $match, (#m) sets $MATCH.
TEST(Subst15c, MatchFlagsAndBackrefs) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; x=foobar; "
                "[[ $x = (#b)(f*)(b*) ]] && echo $match[2] $mbegin[2] $mend[1]; "
                "echo ${x//(#m)o/<$MATCH>}"),
            "bar 4 3\nf<o><o>bar\n");
}

// (#i) and (#a1) in patterns.
TEST(Subst15c, CaseInsensitiveAndApproximate) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; [[ FOO = (#i)foo ]] && echo i; "
                "[[ fxo = (#a1)foo ]] && echo a1; [[ fxx = (#a1)foo ]] || echo no"),
            "i\na1\nno\n");
}

// (S) shortest match, (B)/(E) positions, (M) matched part.
TEST(Subst15c, ParamFlagsBSAndMI) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=abcabc; echo ${(S)x#b*c} ${(SB)x#b} ${(SE)x#b} ${(M)x%c*}"),
            "aabc 2 3 c\n");
}

// ${x::=v} assigns even when set.
TEST(Subst15c, ForcedAssignment) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=1; echo ${x::=2} $x"),
            "2 2\n");
}

// (n) and (On) sort numerically.
TEST(Subst15c, NumericSort) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(10 9 100 1); echo ${(n)a} ${(On)a}"),
            "1 9 10 100 100 10 9 1\n");
}

// (z) splits like the parser.
TEST(Subst15c, ZFlagSplitsWords) {
  Shell sh;
  EXPECT_EQ(out(sh, "x='a \"b c\" d;e'; print -l ${(z)x}"),
            "a\n\"b c\"\nd\n;\ne\n");
}

// (P) applies the subscript before indirecting.
TEST(Subst15c, IndirectSubscript) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(x y); y=Y; echo ${(P)a[2]}"),
            "Y\n");
}

// ${h[key]:=v} assigns the element.
TEST(Subst15c, ElementAssignWithDefault) {
  Shell sh;
  EXPECT_EQ(out(sh, "typeset -A h; : ${h[k]:=v}; echo $h[k]"),
            "v\n");
}

// a[i,j]=() removes a range.
TEST(Subst15c, ArrayRangeAssign) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(1 2 3 4 5); a[2,3]=(); echo $a; a[2,2]=(x y); echo $a"),
            "1 4 5\n1 x y 5\n");
}

// An unquoted | from $~p separates alternatives.
TEST(Subst15c, TopLevelBarAlternatives) {
  Shell sh;
  EXPECT_EQ(out(sh, "p=\"a|b\"; [[ b = $~p ]] && echo y; words=(a x b); "
                "echo ${words[(I)a|b]}"),
            "y\n3\n");
}

// words[(I)…] in (( … )) reads through the expander.
TEST(Subst15c, FlaggedSubscriptInArithmetic) {
  Shell sh;
  EXPECT_EQ(out(sh, "words=(git commit --amend); "
                "(( words[(I)-c|--amend] )) && echo yes; echo $(( words[(i)c*] ))"),
            "yes\n2\n");
}

// Glob qualifiers, flags and approximate matching (M15c).
TEST(Subst15c, GlobQualifiersAndFlags) {
  TempDir dir;
  Shell sh;
  EXPECT_EQ(out(sh, "(setopt extendedglob; cd " + dir.path.string() + "; print -n abc > big; "
                "touch a bb; mkdir d; ln -s a l; "
                "echo *(.) '|' *(/) '|' *(@) '|' *(.OL[1]) '|' *(^/) '|' *(#q.On) '|' "
                "*(.:r:u) '|' *(/,@) '|' *(.e:'[[ $REPLY = b* ]]':) '|' x*(N) '|' "
                "(#i)A* '|' (#a1)bx '|' *(-.))"),
            "a bb big | d | l | big | a bb big l | big bb a | A BB BIG | d l | bb big | "
            "| a | bb | a bb big l\n");
}

// * # ## are greedy, so captures are zsh's.
TEST(Subst15c, GreedyStarAndRepeats) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; [[ aXbXc = (#b)(*)X(*) ]] && echo $match; "
                "[[ foo.tar.gz = (#b)(*).(*) ]] && echo $match; "
                "[[ aaab = (#b)(a##)(*) ]] && echo $match; "
                "[[ aaab = (#b)(a#)(*) ]] && echo \"[$match[1]] [$match[2]]\"; "
                "x=foo/bar/baz; echo ${x/(#b)(*)\\/(*)/$match[1]+$match[2]}"),
            "aXb c\nfoo.tar gz\naaa b\n[aaa] [b]\nfoo/bar+baz\n");
}

// In double quotes an empty value split is one empty word.
TEST(Subst15c, QuotedSplitOfEmptyValue) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=; a=(\"${(s: :)x}\"); b=(\"${(f)x}\"); c=(\"${(z)x}\"); "
                "d=(${(s: :)x}); echo ${#a} ${#b} ${#c} ${#d}; "
                "for i in \"${(s: :)x}\"; do echo \"<$i>\"; done"),
            "1 1 1 0\n<>\n");
}

// A run of separators is one; an empty end field stays.
TEST(Subst15c, QuotedSplitKeepsEndFields) {
  Shell sh;
  EXPECT_EQ(out(sh, "for x in a,,b ,a a, , ,,,a,,,b,,,; do a=(\"${(s:,:)x}\"); "
                "b=(\"${(@s:,:)x}\"); echo \"${#a} [${(j:|:)a}] ${#b}\"; done"),
            "2 [a|b] 3\n2 [|a] 2\n2 [a|] 2\n2 [|] 2\n4 [|a|b|] 10\n");
}

// In zsh ~ binds tighter than |: b~b|a is (b~b)|a.
TEST(Subst15c, ExclusionBindsTighterThanAlternation) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; p=\"b~b|a\"; [[ a = $~p ]] && echo 1; "
                "[[ b = $~p ]] || echo 2; p=\"a*~ab|x\"; [[ ab = $~p ]] || echo 3; "
                "[[ ac = $~p ]] && echo 4"),
            "1\n2\n3\n4\n");
}

// (a~b) and (^a) work inside a group; a trailing ~ is plain.
TEST(Subst15c, ExclusionAndNegationInsideGroups) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; [[ b1 = (*~a*) ]] && echo 1; "
                "[[ abc = (^a)* ]] && echo 2; [[ abc = (^x) ]] && echo 3; "
                "[[ a~ = (*~|x) ]] && echo 4; [[ a = (*~) ]] || echo 5"),
            "1\n2\n3\n4\n5\n");
}

// ${x//pat/r} tries no empty match at the end of the string.
TEST(Subst15c, GlobalReplaceStopsAtTheEnd) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; x=aaab; echo ${x//a#/q} ${x/a#/q}; y=ab; "
                "echo ${y//b#/Q}; z=; echo \"[${z//b#/Q}]\""),
            "qqb qb\nQaQ\n[Q]\n");
}

// :s's left side is text unless HIST_SUBST_PATTERN.
TEST(Subst15c, ModifierSubstitutionIsLiteral) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=abc; echo ${x:gs/[ac]/Y/} ${x:s/b/X/}; z=\"a*b\"; "
                "echo ${z:s/*/S/}; setopt histsubstpattern; echo ${x:gs/[ac]/Y/}"),
            "abc aXc\naSb\nYbY\n");
}

// ${(tP)${…}} gives the named parameter's type.
TEST(Subst15c, IndirectTypeOfNestedSubject) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=(a b); y=x; echo ${(tP)${:-x}} \"${(P)${:-x}}\" ${(P)${y}}"),
            "array a b a b\n");
}

// (P) on a nested subject names the parameter once: the value of foo_v,
// not of the parameter that value names.  Array slices assign as zsh does,
// and a failing subscript leaves the array alone.
TEST(SubstMerge, IndirectionOnceAndSliceAssignment) {
  Shell sh;
  EXPECT_EQ(out(sh, "n=foo_v; foo_v=1; print -r -- ${(P)${n}} ${(P)${:-n}}"), "1 foo_v\n");
  EXPECT_EQ(out(sh, "a=(1 2 3 4); a[2,3]=X; print -r -- $a; i=2,3; a=(1 2 3 4); a[$i]=Y; "
                    "print -r -- $a"),
            "1 X 4\n1 Y 3 4\n");
  (void)sh.run("a=(1 2 3)");
  auto r = sh.run("a[1/0]=x");
  EXPECT_EQ(r.err, "jfsh: division by zero\n");
  EXPECT_EQ(out(sh, "print -r -- $a"), "1 2 3\n");
}

// (S) with (B) (E) counts characters in a multibyte string, as zsh.
TEST(SubstMerge, MatchPositionsCountCharacters) {
  Utf8Locale loc{LC_CTYPE};
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  EXPECT_EQ(out(sh, "x=\xc3\xa9\xc3\xa9" "b; print -r -- ${(SB)x#b} ${(SE)x#b}"), "3 4\n");
}

// {1..$n} is a range, as zsh does brace expansion last.
TEST(Subst15c, BraceRangeAfterExpansion) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(); for i in {1..$#a}; do echo -n \"[$i]\"; done; b=(x y); "
                "echo {1..$#b} {\"1\"..\"3\"} \"{1..3}\"; x=1..3; echo {$x}; "
                "z=a,b; echo {$z}"),
            "[1][0]1 2 1 2 3 {1..3}\n1 2 3\n{a,b}\n");
}

// typeset +m and +i list names only.
TEST(Subst15c, TypesetPlusListsNames) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=1; typeset -i y=2; typeset +m '[xy]'; typeset +i | grep '^y$'"),
            "x\ninteger y\ny\n");
}

// *(T) marks a plain file with a blank; a word that is all one group is
// qualifiers (on nothing); `~` inside a group excludes (M15c round 1).
TEST(Subst15c, GlobTypeMarksGroupsAndExclusions) {
  TempDir dir;
  Shell sh;
  str cd = "(setopt extendedglob; cd " + dir.path.string() + "; ";
  EXPECT_EQ(out(sh, cd + "touch a1 b1 bar; mkdir d1; print -r -- *(T); "
                         "echo (b1)(N) x (*~a*) ((b*)~bar) (b*~bar))"),
            "a1  b1  bar  d1/\nb1 x b1 bar d1 b1 b1\n");
  EXPECT_EQ(sh.run(cd + "echo (a1))").err, "jfsh: no matches found: (a1)\n");
}

// (#s) and (#e) anchor to the whole value, not the part tried.
TEST(Subst15c, AnchorsAreTheWholeStringsEnds) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; x=aab; echo ${x//(#s)a/q}; x=abc; "
                "echo ${x//(#e)/Z} ${x//(#s)/Z}; x=aab; "
                "echo ${x/a(#e)/q} ${x#a(#e)} ${x%(#s)b}; x=foo.bar; "
                "echo ${x/o(#e)/Z}; x=abab; echo ${(S)x/(#s)b/Z} ${(S)x//(#s)a/Z}; "
                "x=foo/bar; echo ${x//(#s)?/Z}"),
            "qab\nabcZ Zabc\naab aab aab\nfoo.bar\nabab Zbab\nZoo/bar\n");
  // A pattern subscript matches each tail on its own, as zsh's getarg:
  // there (#s) is where the tail starts.
  EXPECT_EQ(out(sh, "setopt extendedglob; x=abcab; "
                "echo $x[(r)(#s)b] $x[(i)(#s)b] $x[(I)(#s)a] $x[(I)(#s)b]"),
            "b 2 4 5\n");
}

// The empty match at the end is tried unless the step that got there
// was a match.
TEST(Subst15c, SubstitutionAtTheEnd) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; x=abcd; echo ${x//(b|(#e))/Z} ${x//(bcd|(#e))/Z}; "
                "x=abc; echo ${x//((#s)|(#e))/Z} ${x//(a|(#e))/Z} ${x//c#/Z} ${x//x#/Z} "
                "${x//(#e)/Z} ${x/(#e)/Z} ${x/c#/Z}"),
            "aZcdZ aZ\nZabcZ ZbcZ ZaZbZ ZaZbZc abcZ abcZ Zabc\n");
  // `#` and `%` anchor after `//` too.
  EXPECT_EQ(out(sh, "y=abc; echo \"<${y//#/Z}>\" \"<${y//%/Z}>\" ${y//#a/Z} ${y//%c/Z}"),
            "<Zabc> <abcZ> Zbc abZ\n");
}

TEST(Subst15c, AnchorsWithBackreferences) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; x=aXbXc; "
                "echo ${x/(#b)(*)X(*)(#e)/$match[2]-$match[1]} ${(M)x%%X*(#e)} "
                "${(S)x/(#b)X(?)/<$match[1]>}; "
                "g='a(p)'; echo ${g/(#b)\\(((|^)[p=\\*])\\)(#e)/}; x=abab; "
                "echo ${x//b(#e)/Z} ${x//*(#e)/W}"),
            "c-aXb XbXc a<b>Xc\na\nabaZ W\n");
}

// A word ending in ~ is not a pattern (backup files).
TEST(Subst15c, TrailingTildeIsLiteral) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt extendedglob; echo nofile~ b~; [[ x~ = x~ ]] && echo y; "
                "[[ \"\" = (~a) ]] && echo y3; x=abc; echo ${x:&}"),
            "nofile~ b~\ny\ny3\nabc\n");
  // `~` alone makes no pattern; with a wildcard it excludes.
  TempDir dir;
  str cd = "(setopt extendedglob; cd " + dir.path.string() + "; ";
  EXPECT_EQ(out(sh, cd + "touch foo.c bar.c; echo x~y x~y~ x~y~z foo.c~y *.c~foo*)"),
            "x~y x~y~ x~y~z foo.c~y bar.c\n");
  EXPECT_EQ(sh.run(cd + "echo foo.c~f*)").err, "jfsh: no matches found: foo.c~f*\n");
}

// M13 Phase 13c.1: the pattern matcher follows zsh 5.9; expected values
// are zsh's.

// ##, / and // take the end zsh's greedy search reaches first, with the
// end left open, not the longest.
TEST(Pattern13c, FirstFoundEnd) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "x=aaa; print -r -- ${x##(a|aa)} ${x/(a|aa)/Z} ${x//(a|aa)/Z}"),
            "aa Zaa ZZZ\n");
  EXPECT_EQ(out(sh, "x=abc; print -r -- ${x##(a|ab)} ${x/(#m)(a|ab)/<$MATCH>}"), "bc <a>bc\n");
  EXPECT_EQ(out(sh, "x=abab; print -r -- \"${x##*(a|ab)}|${(M)x##*(a|ab)}\""), "b|aba\n");
  EXPECT_EQ(out(sh, "x=baaa; print -r -- ${x//(|a##)/Z} ${x/(|a##)/Z}"), "ZbZaZaZa Zbaaa\n");
  EXPECT_EQ(out(sh, "x=aaa; print -r -- ${(S)x##(a|aa)} ${(SM)x##(a|aa)} ${(S)x%%(a|aa)} "
                    "${(SM)x%%(a|aa)} ${(SM)x%(a|aa)} ${(M)x%%(a|aa)}"),
            "aa a aa a a aa\n");
  EXPECT_EQ(out(sh, "x=abcabc; print -r -- ${x##(#b)(*)(b|bc)} $match ${x/(b|bc)/Z} "
                    "${x//(b|bc)/Z} ${(S)x/(b|bc)/Z}"),
            "c abca b aZcabc aZcaZc aZcabc\n");
  EXPECT_EQ(out(sh, "x=aaab; print -r -- ${x##(a|aa)#} ${x/(a|aa)#/Z}; "
                    "x=1234x; print -r -- ${x##<1-200>} ${x//<1-20>/Z}"),
            "b Zb\n4x ZZZx\n");
  EXPECT_EQ(out(sh, "x=abab; print -r -- ${x##(#b)(a|ab)(b|)} $match / $mbegin / $mend"),
            "ab a b / 1 2 / 1 2\n");
}

// Under EXTENDED_GLOB `^` negates the rest of its branch wherever it is.
TEST(Pattern13c, CaretAnywhere) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "[[ ab = (#b)^x ]] && [[ ab = (#i)^x ]] && [[ ab = a^x ]] && "
                    "[[ ab = *^b ]] && print yes"),
            "yes\n");
  EXPECT_EQ(out(sh, "x=ab; print -r -- ${x#a^b}"), "b\n");
  EXPECT_EQ(out(sh, "for p in '^a|b' 'a|^b' 'a~^b'; do for s in a b c; do "
                    "[[ $s = $~p ]] && print -rn -- $s; done; print -r -- \" $p\"; done"),
            "bc ^a|b\nac a|^b\n a~^b\n");
  EXPECT_EQ(out(sh, "[[ x = ^ ]] && print 1; [[ '' = ^ ]] || print 2; [[ ab = a^ ]] && print 3; "
                    "[[ a = a^ ]] || print 4; [[ a = ^^a ]] && print 5; [[ b = ^^a ]] || print 6"),
            "1\n2\n3\n4\n5\n6\n");
  EXPECT_EQ(out(sh, "[[ b = (^a|b) ]] && print 1; [[ c = (x|^a) ]] && print 2; "
                    "[[ a = (x|^a) ]] || print 3; [[ xy = x(^a|b) ]] && print 4; "
                    "[[ ab = ^a~b ]] || print 5; [[ c = ^a~b ]] && print 6"),
            "1\n2\n3\n4\n6\n");
  EXPECT_EQ(out(sh, "x=abc; print -r -- \"${x#a^c}|${x##a^c}|${x%^c}|${x%%^c}|${x/^b/Z}\""),
            "bc||abc||Z\n");
  EXPECT_EQ(out(sh, "[[ AB = (#i)a^b ]] || print 1; [[ AC = (#i)a^b ]] && print 2; "
                    "[[ aB = a(#i)^b ]] || print 3"),
            "1\n2\n3\n");
}

TEST(Pattern13c, CaretAndCaseInGlobs) {
  Shell sh;
  TempDir dir;
  str cd = "(cd " + dir.path.string() + "; ";
  EXPECT_EQ(out(sh, cd + "touch a.txt b.TXT c.c; setopt extendedglob; print (#i)^*.txt)"),
            "c.c\n");
  // NO_CASE_GLOB folds literals but, as in zsh, not brackets.
  TempDir mixed;
  str cd2 = "(cd " + mixed.path.string() + "; touch abc ABD Abe xyz; setopt nocaseglob; ";
  EXPECT_EQ(out(sh, cd2 + "print -- [a]*; print -- *bd a?e; print -- [^a]*; print -- *[d-e])"),
            "abc\nABD Abe\nABD Abe xyz\nAbe\n");
}

// Under MULTIBYTE $mbegin, $mend, $MBEGIN and $MEND count characters.
TEST(Pattern13c, CharacterPositions) {
  Utf8Locale loc{LC_CTYPE};
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "[[ éXé = (#b)(*)X(*) ]] && print -r -- $mbegin / $mend"), "1 3 / 1 3\n");
  EXPECT_EQ(out(sh, "x=aéb; print -r -- ${x/(#m)é/<$MATCH $MBEGIN $MEND>}"), "a<é 2 2>b\n");
  EXPECT_EQ(out(sh, "x=éaé; print -r -- ${x//(#m)?/<$MBEGIN-$MEND>}"), "<1-1><2-2><3-3>\n");
  EXPECT_EQ(out(sh, "x=ééa; print -r -- ${x#(#b)(é)} $mbegin $mend ${x%(#b)(a)} $mbegin $mend "
                    "${(S)x%(#b)(é)} $mbegin $mend"),
            "éa 1 1 éé 3 3 éa 2 2\n");
  EXPECT_EQ(out(sh, "setopt ksharrays; [[ éXé = (#b)(*)X(*) ]] && print -r -- ${mbegin[@]} / "
                    "${mend[@]}; x=aéb; print -r -- ${x/(#m)b/<$MBEGIN $MEND>}"),
            "0 2 / 0 2\naé<2 2>\n");
}

// (#i) and (#l) fold non-ASCII literals; brackets are not folded.
TEST(Pattern13c, CaseFlags) {
  Utf8Locale loc{LC_CTYPE};
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "[[ É = (#i)é ]] && [[ é = (#i)É ]] && [[ É = (#l)é ]] && "
                    "[[ ÉÉ = (#i)é# ]] && print yes"),
            "yes\n");
  EXPECT_EQ(out(sh, "[[ A = (#i)[a] ]] || [[ A = (#i)[a-z] ]] || [[ a = (#l)[A] ]] || "
                    "[[ É = (#i)[é] ]] || print no"),
            "no\n");
  EXPECT_EQ(out(sh, "[[ A = (#i)[^a] ]] && print yes"), "yes\n");
  EXPECT_EQ(out(sh, "[[ Ä = (#i)ä ]] && print 1; [[ ä = (#l)Ä ]] || print 2; "
                    "[[ ÄÖ = (#i)äö ]] && print 3; [[ É = (#i)[[:lower:]] ]] || print 4; "
                    "[[ x = (#i)[X] ]] || print 5"),
            "1\n2\n3\n4\n5\n");
  EXPECT_EQ(out(sh, "[[ AB = (#l)ab ]] && print 1; [[ Ab = (#l)aB ]] || print 2"), "1\n2\n");
}

// A repeat that matches nothing ends the repeat, its groups kept.
TEST(Pattern13c, EmptyRepeats) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "[[ b = (#b)(|a)##b ]] && print -r -- $mbegin $mend"), "1 0\n");
  EXPECT_EQ(out(sh, "[[ a = (#b)a(x|)# ]] && print -r -- $mbegin $mend"), "2 1\n");
  EXPECT_EQ(out(sh, "[[ ab = (#b)(a|b|)# ]] && print -r -- \"<$match>\" $mbegin $mend"),
            "<> 3 2\n");
  EXPECT_EQ(out(sh, "[[ b = (#a1)(|a)##b ]] && print yes"), "yes\n");
  EXPECT_EQ(out(sh, "[[ ab = (#b)(a|)#b ]] && print -r -- \"<$match>\" $mbegin $mend; "
                    "[[ aab = (#b)(a|)##b ]] && print -r -- \"<$match>\" $mbegin $mend"),
            "<> 2 1\n<> 3 2\n");
  EXPECT_EQ(out(sh, "[[ ab = (#b)(|a)(#c1,3)b ]] && print -r -- \"<$match>\" $mbegin $mend; "
                    "[[ ab = (#b)(|a)(#c2,3)b ]] && print -r -- \"<$match>\" $mbegin $mend"),
            "<> 2 1\n<> 2 1\n");
  EXPECT_EQ(out(sh, "[[ aaa = (#b)(a#)# ]] && print -r -- \"<$match>\" $mbegin $mend; "
                    "[[ '' = (#b)(a|)# ]] && print -r -- \"<$match>\" $mbegin $mend; "
                    "[[ '' = (#b)(a|)## ]] && print -r -- \"<$match>\" $mbegin $mend"),
            "<> 4 3\n<> 1 0\n<> 1 0\n");
  EXPECT_EQ(out(sh, "x=aaa; print -r -- ${x//(|a)/Z} ${x//(a|)/Z} ${x//a#/Z}"),
            "ZaZaZa ZZZ Z\n");
}

TEST(Pattern13c, NineGroups) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "[[ abcdefghijk = (#b)(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k) ]] && "
                    "print -r -- $#match $match"),
            "9 a b c d e f g h i\n");
  EXPECT_EQ(out(sh, "[[ abcdefghij = (#b)(a)(b)(c)(d)(e)(f)(g)(h)(i)(j) ]] && "
                    "print -r -- $#match $match[9] $mbegin[9] $#mbegin"),
            "9 i 9 9\n");
}

// A pattern zsh cannot compile is "bad pattern": [[ ]] fails with status
// 2, case with 0, an expansion with 1, and the command line ends.
TEST(Pattern13c, BadPatterns) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  auto bad = [&](strv cmd, strv err, int rc) {
    auto r = sh.run(cmd);
    EXPECT_EQ(r.out, "") << cmd;
    EXPECT_EQ(r.err, err) << cmd;
    EXPECT_EQ(r.rc, rc) << cmd;
  };
  bad("[[ ab = (#X)ab ]]; print after", "jfsh: bad pattern: (#X)ab\n", 2);
  bad("[[ a = (#c2)a ]]; print after", "jfsh: bad pattern: (#c2)a\n", 2);
  bad("[[ ac = (#a)ac ]] || print no; print after", "jfsh: bad pattern: (#a)ac\n", 2);
  bad("x=ab; y=${x#(#X)a}; print after", "jfsh: bad pattern: (#X)a\n", 1);
  bad("x=ab; print ${x/(#X)\\*/b}; print after", "jfsh: bad pattern: (#X)*\n", 1);
  bad("case ab in (#X)ab) print y;; esac; print after", "jfsh: bad pattern: (#X)ab\n", 0);
  bad("print (#X)*", "jfsh: bad pattern: (#X)*\n", 1);
  bad("setopt histsubstpattern; x=abc; print ${x:s/(#X)b/Z/}", "jfsh: bad pattern: (#X)b\n", 1);
  for (strv p : {"(#a255)a", "#a", "a###", "*#", "*(#c2)", "a(#cx)", "a#(#c2)", "a(#i)(#c2)",
                 "(#se)a", "(#i", "^#", "a|(#X)", "a~(#X)"})
    bad("p='" + str(p) + "'; [[ a = $~p ]]", "jfsh: bad pattern: " + str(p) + "\n", 2);
  EXPECT_EQ(out(sh, "for p in '(#a254)a' '(#)a' '(#q.)a' 'a(#c,2)' '(#a 1)a' '(#ia1)a'; do "
                    "[[ a = $~p ]]; print -n $?; done; print"),
            "000000\n");
  EXPECT_EQ(out(sh, "setopt nobadpattern; print (#X)*"), "(#X)*\n");
  sh.opts.unset(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "[[ ab = (#X)ab ]]; print $?"), "1\n");
}

// In ${x#…} ${x/…} and :s, a `|` written outside parentheses is text;
// one from $~var separates alternatives.
TEST(Pattern13c, BarInOperands) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=ab; print -r -- ${x/a|b/Z} ${x#a|b} ${x//(a|b)/Z} \"${x/a|b/Z}\""),
            "ab ab ZZ ab\n");
  EXPECT_EQ(out(sh, "x='a|b'; print -r -- ${x/a|b/Z} ${x#a|} ${x:#a|b}; p='b|a'; x=ab; "
                    "print -r -- ${x//$~p/Z} ${x//$p/Z}"),
            "Z b\nZZ ab\n");
  EXPECT_EQ(out(sh, "x=ab; print -r -- ${x/a\\|b/Z}; x='a|b'; "
                    "print -r -- ${x/a\\|b/Z} ${x/\"a|b\"/Z} ${x/'|'/Z}"),
            "ab\nZ Z aZb\n");
  EXPECT_EQ(out(sh, "setopt histsubstpattern; x=abcb; print -r -- ${x:s/b|c/Z/} ${x:s/(b|c)/Z/}"),
            "abcb aZcb\n");
}

// A byte that is no character matches only such a byte, and literals
// compare characters; zsh's `*` before final literal text compares bytes.
TEST(Pattern13c, Bytes) {
  Utf8Locale loc{LC_CTYPE};
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "a=$'\\xc3' b=$'\\xa9'; [[ é = (#b)$a(*)$b ]] || print 1; "
                    "[[ é = (|$a)?#$b ]] && print 2; [[ é = (|$a)$b ]] || print 3; "
                    "[[ é = $a$b ]] && print 4; [[ é = *$b ]] && print 5; "
                    "[[ é = (*$b) ]] || print 6"),
            "1\n2\n3\n4\n5\n6\n");
  EXPECT_EQ(out(sh, "b=$'\\xa9'; x=é; print -r -- \"${x%*$b}|${x#*$b}|${x%%$b}\""), "|é|é\n");
}

// zsh's getmatch, quirks included: (S) finds an empty match at the end
// first and reports it at the start, its search for a shortest match
// leaves (#e) (and in (S)% (#s)) failing, and (B) (E) (N) (M) (R) join
// with spaces.  # % and :# set $match and $MATCH too.
TEST(Pattern13c, GetmatchQuirks) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "x=abc; print -r -- ${(SB)x#(#e)} ${(S)x/(#e)/Z} ${x/(#e)/Z} "
                    "${(SM)x#c#(#e)}. ${(SB)x#c#(#e)}"),
            "1 Zabc abcZ . 1\n");
  EXPECT_EQ(out(sh, "x=abaa; print -r -- ${(S)x//(b|(#e))/Z} ${x//(b|(#e))/Z}"), "aZaa aZaaZ\n");
  EXPECT_EQ(out(sh, "x=bb; print -r -- ${(S)x%(#s)**}. ${(S)x%(#s)b}. ${(SM)x%(#s)b*}"),
            ". b. bb\n");
  EXPECT_EQ(out(sh, "x=abc; print -r -- ${x%c#}. ${(I:2:)x%c#}. ${(MB)x%%x#}; "
                    "print -r -- \"${(MR)x#a}|${(MBEN)x#b}|${(BE)x#z}|${(MR)x#z}|\""),
            "abc. ab.  4\na bc| 1 1 0|1 1| abc|\n");
  EXPECT_EQ(out(sh, "x=abab; : ${x#(#b)(a)}; print -r -- $match $mbegin; : ${x%(#b)(?)}; "
                    "print -r -- $match $mbegin; : ${x##(#m)a*}; print -r -- $MATCH $MBEGIN $MEND; "
                    ": ${x:#(#b)(a)*}; print -r -- $match"),
            "a 1\nb 4\nabab 1 4\na\n");
}

// (#cN,M) runs as zsh's counter, which keeps where the last repeat began
// as attempts fail; the backtracker decides such matches, so the set
// matcher only filters.
TEST(Pattern13c, CountedRepeats) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "[[ babaa = (#b)(*b(a|ba)#)(#c2) ]] && print -r -- y $match $mbegin"),
            "y abaa a 2 5\n");
  EXPECT_EQ(out(sh, "[[ cba = (?|cb)(#c1,2) ]] || print no; [[ cba = (#b)(?|cb)(#c1,2) ]] || "
                    "print no; x=cba; print -r -- ${x%%(?|cb)(#c1,2)} ${x##(?|cb)(#c1,2)}."),
            "no\nno\nc a.\n");
  EXPECT_EQ(out(sh, "[[ babaa = (b(a|ba)#)(#c2) ]] || print no; "
                    "[[ babaa = (*b(a|x)#)(#c2) ]] && print yes"),
            "no\nyes\n");
  EXPECT_EQ(out(sh, "[[ bab = (#b)(*b(a|ba)#)(#c2) ]] && print -r -- $mbegin; "
                    "[[ ab = (#b)(|a)(#c2,3)b ]] && print -r -- $mbegin $mend"),
            "3 2\n2 1\n");
}

// zsh's flags leak past (#cN,M): when a further repeat of a one-branch
// group fails after the branch changed the case flags, nothing puts them
// back, so they stay on for what follows.
TEST(Pattern13c, FlagsLeakPastCounts) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "for p in '((#i)A)(#c1,2)b' '((#i)A)(#c0,2)b' '((#l)a)(#c1,2)b' "
                    "'((#i)A)(#c1,2)bc' 'a((#i)A)(#c1,2)b' '((#i)A)(#c1)b' '(y|(#i)A)(#c1,2)b' "
                    "'((#i)A)#b' '((#i)A(#I))(#c1,2)b' '((#i)A)(#c1,2)[b]'; do "
                    "[[ aB = $~p ]] && print -n y || print -n n; done; print"),
            "yyynnnnnyn\n");
  EXPECT_EQ(out(sh, "[[ aBC = ((#i)A)(#c1,2)bc ]] && print y; x=bAbaaB; "
                    "print -r -- ${x//((#i)A)(#c0,2)b/Z}; "
                    "[[ xaB = (?(#i)A)(#c1,2)b ]] && print y2; "
                    "[[ xaB = (x(#i)A)(#c1,2)b ]] || print n3"),
            "y\nZZaZ\ny2\nn3\n");
  // Through enclosing groups that change no flags themselves, and from
  // a one-branch group inside the counted one.
  EXPECT_EQ(out(sh, "for t in 'aB (((#i)A)(#c1,2))b' 'aB (((#i)A)(#c1,2)|x)b' "
                    "'aB (x|((#i)A)(#c1,2))b' 'aBc (((#i)A)(#c1,2)b)C' 'aBB ((((#i)A)(#c1,2))b)b' "
                    "'aB (((#i)A)(#c1,2))#b' 'aB (((#i)A))(#c1,2)b'; do s=${t% *} p=${t#* }; "
                    "[[ $s = $~p ]]; print -n $?; done; print"),
            "0000000\n");
  // A branch with an exclusion puts its flags back.
  EXPECT_EQ(out(sh, "for t in 'aB ((#i)A~x)(#c1,2)b' 'AB ((#i)A~x)(#c1,2)b' "
                    "'aB (((#i)A~x))(#c1,2)b' 'aB (((#i)A)~x)(#c1,2)b'; do s=${t% *} p=${t#* }; "
                    "[[ $s = $~p ]]; print -n $?; done; print"),
            "1111\n");
  EXPECT_EQ(out(sh, "x=BBbbB; print -r -- ${x##(((#i)A)(#c0,3))b}; "
                    "x=BabBbb; print -r -- ${x//(b|(?b(#i)A)(#c0,3))(b)#/Z}"),
            "BbbB\nZBZaZZ\n");
}

// (#cN,M) reads its numbers as zsh's zstrtol: blanks and a sign may come
// first, and a negative one is no limit.
TEST(Pattern13c, CountSyntax) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "for p in 'a(#c 1)' 'a(#c-1)' 'a(#c+1)' 'a(#c1,-1)' 'a(#c-1,2)' 'a(#c 1, 2)' "
                    "'a(#c,-1)'; do for s in '' a aa aaa; do [[ $s = $~p ]] && print -n y || "
                    "print -n n; done; print -n ' '; done; print"),
            "nynn yyyy nynn nyyy yyyn nyyn yyyy \n");
  // A lone sign is 0; a sign before a blank or another sign is bad.
  EXPECT_EQ(out(sh, "[[ B = (a)(#c-)B ]]; print -n $?; [[ B = (a)(#c+)B ]]; print -n $?; "
                    "[[ aaB = (a)(#c-,2)B ]]; print -n $?; [[ aB = (a)(#c1,-)B ]]; print -n $?; "
                    "[[ aaB = (a)(#c1,+)B ]]; print $?"),
            "00011\n");
  for (strv p : {"(a)(#c- 1,2)", "(a)(#c--1,2)"}) {
    auto r = sh.run("p='" + str(p) + "'; [[ a = $~p ]]");
    EXPECT_EQ(r.err, "jfsh: bad pattern: " + str(p) + "\n");
  }
}

// Giving up on one element of an array does not change the others.
TEST(Pattern13c, SearchStateIsPerCall) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "h=${(l:300::a:)}c${(l:300::a:)}; a=(aaaca $h aaaca); "
                    "r=(${a##*a*a*a(c|ca)}); print -r -- \"<$r[1]> <$r[3]>\""),
            "<a> <a>\n");
  // A stopgap until 13c.2: the backtracker gives up on the middle one and
  // takes the longest end; zsh prints 300.
  EXPECT_EQ(out(sh, "x=${(l:300::a:)}c${(l:300::a:)}; print ${#${x##*a*a*a(c|ca)}}"), "299\n");
}

// Inside a group, `~` exclusions and the text after `^` start with the
// group's flags; at the top level with those their branch ends with.
TEST(Pattern13c, FlagsOfExclusions) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  EXPECT_EQ(out(sh, "for p in '((#i)^A)' '((#i)a~A|b)' 'x((#i)a~A)' '(x(#i)^A)' '(#i)^A' "
                    "'x(#i)a~A' '((#i)a(#I)~A)'; do for s in a A b B xa xA x; do "
                    "[[ $s = $~p ]] && print -rn -- \"$s \"; done; print -r -- \"| $p\"; done"),
            "a b B xa xA x | ((#i)^A)\na b | ((#i)a~A|b)\nxa | x((#i)a~A)\nxa x | (x(#i)^A)\n"
            "b B xa xA x | (#i)^A\nxa xA | x(#i)a~A\na | ((#i)a(#I)~A)\n");
  // A `~` after leading flags excludes.
  EXPECT_EQ(out(sh, "p='(#i)~a'; [[ '' = $~p ]] && print 1; p='(#)~a'; [[ '' = $~p ]] && print 2; "
                    "[[ a = $~p ]] || print 3"),
            "1\n2\n3\n");
}

TEST(Pattern13c, MultibyteQuirks) {
  Utf8Locale loc{LC_CTYPE};
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  // x#'s flags are its operand's: (#i)x#k compares `k` with the last byte
  // of the Kelvin sign.
  EXPECT_EQ(out(sh, "k=$'\\u212a'; [[ x$k = (#i)x#k ]] || print 1; [[ $k = (#i)k ]] && print 2"),
            "1\n2\n");
  // zsh's (S)% gives its shortest match the character index of the end,
  // and % tries the empty match at the end of one character with (#s) on.
  EXPECT_EQ(out(sh, "x=abcabc; : ${(S)x%(#m)b*}; print -r -- $MBEGIN $MEND; x=aébcabc; "
                    ": ${(S)x%(#m)b*}; print -r -- $MBEGIN; x=abcabc; : ${(S)x%(#b)(b)*}; "
                    "print -r -- $mbegin"),
            "7 7\n8\n7\n");
  EXPECT_EQ(out(sh, "x=b; print -r -- ${(B)x%(#s)}; x=é; print -r -- ${(B)x%(#s)}; "
                    ": ${x%(#m)(#s)}; print -r -- \"<$MATCH>\" $MBEGIN; "
                    "x=ab; print -r -- ${(B)x%(#s)}"),
            "2\n2\n<> 2\n1\n");
}

// Policy: `~` and `^` follow the documented set semantics, an exclusion
// tested where its branch ends.  zsh tests it after the rest of the
// pattern matched, where earlier attempts may already have marked the
// positions, so its results differ; those are noted with each.
TEST(Pattern13c, ExclusionsFollowSetSemantics) {
  Shell sh;
  sh.opts.set(OPT_EXTENDEDGLOB);
  // zsh prints only yes2.
  EXPECT_EQ(out(sh, "[[ ba = (b|ba)~b ]] && print yes; [[ ba = (ba|b)~b ]] && print yes2"),
            "yes\nyes2\n");
  // zsh: b / 1 1.
  EXPECT_EQ(out(sh, "[[ b = (#b)(*~b|^ba)# ]] && print -r -- $match / $mbegin $mend"),
            "/ 2 1\n");
  // zsh: aba.
  EXPECT_EQ(out(sh, "x=aba; print -r -- ${x%(b|b#|ba*)~b#()(#c0,1)}"), "a\n");
  // zsh matches nothing: an attempt that an inner exclusion (the `^`)
  // rejected has already marked the outer exclusion's positions.
  EXPECT_EQ(out(sh, "for s in x '' a; do [[ $s = ((^)#~b) ]] && print -rn -- \"<$s>\"; done; "
                    "print"),
            "<x><><a>\n");
}

}  // namespace
}  // namespace jfsh
