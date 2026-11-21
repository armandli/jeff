// zsh/complete (M15 phase 15a): completion widgets (zle -C), their special
// parameters and $compstate, and the compadd, compset and compquote
// builtins.  The editor runs in process on a pty (editor_rig.h); keys are
// typed as a user would.  Expected values are zsh 5.9's (`zsh -f`), taken
// by driving an interactive zsh the same way.

#include <string>
#include <string_view>

#include <editor_rig.h>
#include <gtest/gtest.h>
#include <shell_fixture.h>

#include <jfsh/core/aliases.h>

namespace jfsh {
namespace {

using test::Rig;
using test::Shell;
using test::TempDir;

const str CX = "\x18";   // ^X: the widgets below are bound to ^X and a letter

struct WRig : Rig {
  WRig() : Rig(120) {}
  // Completion widget w<key> on ^X<key>, acting like `style`, running `fn`.
  void widget(strv key, strv style, strv fn) {
    str name = "w" + str(key);
    sh.run("zle -C " + name + " " + str(style) + " " + str(fn) + "; bindkey '^X" + str(key) + "' "
           + name);
  }
  // A global the completion function set.
  str var(strv name) { return sh.run("print -rn -- \"$" + str(name) + "\"").out; }
  // Types `keys`, which end the line with an error: whether they did.
  bool aborts(const str& keys) {
    sh.run("got=");
    start();
    send(keys);
    finish();
    return ed->interrupted();
  }
  // Types `keys` then Enter, and returns the line; `got` is cleared first.
  opt<str> run(const str& keys) {
    sh.run("got=");
    return line(keys + "\r");
  }
};

// ---- zle -C ------------------------------------------------------------------------

TEST(CompletionWidget, DefinedWithZleC) {
  Shell sh;
  EXPECT_EQ(sh.run("zle -C a b c").err, "jfsh: zle: invalid widget `b'\n");
  EXPECT_EQ(sh.run("zle -C a complete-word").err, "jfsh: zle: not enough arguments for -C\n");
  sh.run("zle -C foo complete-word bar");
  EXPECT_EQ(sh.run("zle -l -L").out, "zle -C foo complete-word bar\n");
  EXPECT_EQ(sh.run("zle -l").out, "foo -C complete-word bar\n");
  sh.run("zle -C g .menu-complete h");
  EXPECT_EQ(sh.run("zle -l -L").out, "zle -C foo complete-word bar\nzle -C g .menu-complete h\n");
  sh.run("zle -N foo");
  EXPECT_EQ(sh.run("zle -l").out, "foo\ng -C .menu-complete h\n");
}

TEST(CompletionWidget, ActsLikeItsBuiltinWidget) {
  WRig t;
  t.sh.run("f() { compadd foo foobar }; g() { compadd -S = foo bar }");
  t.widget("a", "complete-word", "f");
  t.widget("b", "menu-complete", "g");
  t.widget("c", "reverse-menu-complete", "g");
  t.widget("e", "list-choices", "f");
  t.widget("g", "delete-char-or-list", "f");
  EXPECT_EQ(t.run("x f" + CX + "a"), "x foo");
  EXPECT_EQ(t.run("x " + CX + "b"), "x bar=");
  EXPECT_EQ(t.run("x " + CX + "b" + CX + "b"), "x foo=");
  EXPECT_EQ(t.run("x " + CX + "b" + CX + "b" + CX + "b"), "x bar=");
  EXPECT_EQ(t.run("x " + CX + "c"), "x foo=");
  EXPECT_EQ(t.run("xy\x02" + CX + "g"), "x");   // not at the end: deletes
  t.type("x f" + CX + "e");
  EXPECT_EQ(t.screen.row(0), "> x f");
  EXPECT_EQ(t.screen.row(1), "foo     foobar");
  t.send("\r");
  EXPECT_EQ(t.finish(), "x f");
}

// ---- The special parameters ----------------------------------------------------------

TEST(CompletionWidget, SpecialParameters) {
  WRig t;
  t.sh.run("f() { got=\"${(j:,:)words}|$CURRENT|$PREFIX|$SUFFIX|$IPREFIX|$ISUFFIX|$QIPREFIX|"
           "$QISUFFIX|$WIDGETSTYLE\"; compadd foo foobar }");
  t.widget("a", "complete-word", "f");
  t.run("echo f" + CX + "a");
  EXPECT_EQ(t.var("got"), "echo,f|2|f||||||complete-word");
  t.run("echo \"ab cd\x02\x02" + CX + "a");
  EXPECT_EQ(t.var("got"), "echo,\"ab cd|2|ab cd||||\"||complete-word");
  t.run("echo $fo" + CX + "a");
  EXPECT_EQ(t.var("got"), "echo,$fo|2|fo||$||||complete-word");
  t.run("echo 'f" + CX + "a");
  EXPECT_EQ(t.var("got"), "echo,'f|2|f||||'||complete-word");
  t.run("x a\\ b" + CX + "a");
  EXPECT_EQ(t.var("got"), "x,a\\ b|2|a\\ b||||||complete-word");
  t.run("x ab\"cd" + CX + "a");
  EXPECT_EQ(t.var("got"), "x,ab\"cd|2|abcd||||||complete-word");
  t.sh.run("setopt completeinword");
  t.run("cmd ab=cd\x02\x02\x02" + CX + "a");
  EXPECT_EQ(t.var("got"), "cmd,ab=cd|2|ab|=cd|||||complete-word");
  // $words has the whole command, after the cursor too.
  t.run("a -e b -z c\x02\x02\x02\x02\x02" + CX + "a");
  EXPECT_EQ(t.var("got"), "a,-e,b,-z,c|3|b||||||complete-word");
}

TEST(CompletionWidget, ParametersAreLiveAndEditable) {
  WRig t;
  t.widget("a", "complete-word", "f");
  // What compadd matches against and what goes in the line follow them.
  t.sh.run("f() { PREFIX=b; compadd bar baz }");
  EXPECT_EQ(t.run("x f" + CX + "a"), "x ba");
  t.sh.run("f() { IPREFIX=x; PREFIX=${PREFIX#x}; compadd foo }");
  EXPECT_EQ(t.run("x xf" + CX + "a"), "x xfoo");
  t.sh.run("f() { words=(a b); CURRENT=5; got=\"${#words} $CURRENT\" }");
  t.run("x " + CX + "a");
  EXPECT_EQ(t.var("got"), "2 5");
  t.sh.run("f() { unset PREFIX; got=\"[${PREFIX-unset}]\" }");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "[]");
  t.sh.run("f() { { QIPREFIX=x } 2>/dev/null; got=reached }");
  EXPECT_EQ(t.run("x f" + CX + "a"), "x f");
  EXPECT_EQ(t.var("got"), "");
  // A function's changes are undone when it returns, unless it set
  // compstate[restore] to something else.
  t.sh.run("inner() { PREFIX=zz; IPREFIX=ii; words=(q); CURRENT=1 }"
           "; f() { inner; got=\"${(j:,:)words}|$CURRENT|$PREFIX|$IPREFIX\"; compadd zzfoo }");
  EXPECT_EQ(t.run("x f" + CX + "a"), "x f");
  EXPECT_EQ(t.var("got"), "x,f|2|f|");
  t.sh.run("inner() { compstate[restore]=no; PREFIX=zz }"
           "; f() { inner; got=\"$PREFIX|$compstate[restore]\" }");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "zz|auto");
  // Outside completion functions they are ordinary names again.
  EXPECT_EQ(t.sh.run("print -r -- ${+PREFIX} ${+compstate}").out, "0 0\n");
}

// ---- $compstate ----------------------------------------------------------------------

TEST(CompletionWidget, CompstateContext) {
  WRig t;
  t.sh.run("f() { got=\"$compstate[context]|$compstate[parameter]|$compstate[redirect]|"
           "$compstate[quote]|$compstate[quoting]|$compstate[all_quotes]|${(j:,:)words}|"
           "$CURRENT|$PREFIX|$IPREFIX\" }");
  t.widget("a", "complete-word", "f");
  auto ctx = [&](const str& line) {
    t.run(line + CX + "a");
    return t.var("got");
  };
  EXPECT_EQ(ctx("echo f"), "command|||||\\|echo,f|2|f|");
  EXPECT_EQ(ctx("f"), "command|||||\\|f|1|f|");
  EXPECT_EQ(ctx("foo=bar cmd f"), "command|||||\\|cmd,f|2|f|");
  EXPECT_EQ(ctx("echo $(cmd a f"), "command|||||\\|cmd,a,f|3|f|");
  EXPECT_EQ(ctx("ls > f"), "redirect||>|||\\|ls,f|2|f|");
  EXPECT_EQ(ctx("ls 2>f"), "redirect||2>|||\\|ls,f|2|f|");
  EXPECT_EQ(ctx("cat <f"), "redirect||<|||\\|cat,f|2|f|");
  EXPECT_EQ(ctx("x=f"), "value|x||||\\|f|1|f|");
  EXPECT_EQ(ctx("a[x]=f"), "value|a||||\\|f|1|f|");
  EXPECT_EQ(ctx("a=(b c f"), "array_value|a||||\\|c,f|2|f|");
  EXPECT_EQ(ctx("a[f"), "subscript|a||||\\||0|f|");
  EXPECT_EQ(ctx("[[ -f f"), "condition|||||\\|[[,-f,f|3|f|");
  EXPECT_EQ(ctx("(( a + f"), "math|||||\\||0| a + f|");
  EXPECT_EQ(ctx("echo $((f"), "math|||||\\||0|f|");
  EXPECT_EQ(ctx("echo $fo"), "parameter|||||\\|echo,$fo|2|fo|$");
  EXPECT_EQ(ctx("echo ${fo"), "brace_parameter|||||\\|echo,${fo|2|fo|${");
  EXPECT_EQ(ctx("echo ${(f"), "brace_parameter|||||\\|echo,${(f|2|${(f|");
  EXPECT_EQ(ctx("echo \"$fo"), "parameter|||\"|double|\"|echo,\"$fo|2|fo|\"$");
  EXPECT_EQ(ctx("echo 'f"), "command|||'|single|'|echo,'f|2|f|");
  EXPECT_EQ(ctx("echo `f"), "command|||`|backtick|\\|f|1|f|");
}

TEST(CompletionWidget, CompstateOnEntry) {
  WRig t;
  t.sh.run("f() { got=\"${compstate[insert]-UNSET}|$compstate[list]|${compstate[old_list]-UNSET}|"
           "$compstate[restore]|$compstate[to_end]|$compstate[pattern_insert]|"
           "$compstate[last_prompt]|$compstate[list_max]|${compstate[vared]-UNSET}|"
           "$compstate[nmatches]|$compstate[unambiguous_cursor]|$compstate[ignored]\""
           "; compadd foo1 foo2 }");
  t.widget("a", "complete-word", "f");
  t.widget("b", "menu-complete", "f");
  t.widget("e", "list-choices", "f");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "automenu-unambiguous|ambiguous|UNSET|auto|match|menu|yes|100||0|1|0");
  t.run("x f" + CX + "b");
  EXPECT_EQ(t.var("got"), "menu|ambiguous|UNSET|auto|match|menu|yes|100||0|1|0");
  t.run("x f" + CX + "e");
  EXPECT_EQ(t.var("got"), "UNSET|list|UNSET|auto|match|menu|yes|100||0|1|0");
  t.run("echo \"ab cd\x02\x02" + CX + "a");
  EXPECT_EQ(t.var("got"), "automenu-unambiguous|ambiguous|UNSET|auto|single|menu|yes|100||0|1|0");
  // The third press: the list is shown, and automenu starts the menu
  // (the fourth goes round it without calling the function).
  EXPECT_EQ(t.run("x f" + CX + "a" + CX + "a" + CX + "a"), "x foo1");
  EXPECT_EQ(t.var("got"), "automenu|ambiguous|shown|auto|match|menu|yes|100||0|1|0");
  EXPECT_EQ(t.run("x f" + CX + "a" + CX + "a" + CX + "a" + CX + "a"), "x foo2");
  t.sh.run("unsetopt autolist automenu");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "unambiguous||UNSET|auto|match|menu|yes|100||0|1|0");
  t.sh.run("setopt autolist nolistambiguous listpacked listrowsfirst menucomplete");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "menu|autolist packed rows|UNSET|auto|match|menu|yes|100||0|1|0");
}

TEST(CompletionWidget, CompstateAfterCompadd) {
  WRig t;
  t.widget("a", "complete-word", "f");
  t.sh.run("f() { compadd foo foobar; got=\"$? $compstate[nmatches] [$compstate[unambiguous]] "
           "$compstate[unambiguous_cursor] [$compstate[exact_string]] $compstate[ignored] "
           "$compstate[list_lines]\" }");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 2 [foo] 4 [] 0 1");
  t.run("x foo" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 2 [foo] 4 [foo] 0 1");
  t.sh.run("f() { compadd zz; got=\"$? $compstate[nmatches]\" }");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "1 0");
  // Some keys can be set, some not; an error throws the matches away.
  t.sh.run("f() { compstate[context]=xx; compstate[foo]=1; unset 'compstate[list]'"
           "; got=\"$compstate[context] $compstate[foo] $compstate[list]\" }");
  t.run("x " + CX + "a");
  EXPECT_EQ(t.var("got"), "xx 1 ambiguous");
  t.sh.run("f() { compadd foo; { compstate[nmatches]=7 } 2>/dev/null; got=reached }");
  EXPECT_EQ(t.run("x f" + CX + "a"), "x f");
  EXPECT_EQ(t.var("got"), "");
}

TEST(CompletionWidget, InsertAndListOnReturn) {
  WRig t;
  t.widget("a", "complete-word", "f");
  auto with = [&](const str& insert) {
    t.sh.run("f() { compadd foo bar baz; compstate[insert]='" + insert + "' }");
  };
  with("menu");
  EXPECT_EQ(t.run("x " + CX + "a"), "x bar");
  with("2");
  EXPECT_EQ(t.run("x " + CX + "a"), "x baz");
  with("2 ");
  EXPECT_EQ(t.run("x " + CX + "a"), "x baz");
  with("0");
  EXPECT_EQ(t.run("x " + CX + "a"), "x foo");
  with("4");
  EXPECT_EQ(t.run("x " + CX + "a"), "x bar");
  with("-1");   // zsh 5.9 inserts nothing for a negative number
  EXPECT_EQ(t.run("x b" + CX + "a"), "x b");
  with("all");
  EXPECT_EQ(t.run("x b" + CX + "a"), "x bar baz ");
  with("");
  EXPECT_EQ(t.run("x b" + CX + "a"), "x b");
  with("tab");   // just the key
  EXPECT_EQ(t.run("x b" + CX + "a"), "x ba");
  t.sh.run("f() { compadd foo; compstate[insert]='menu:2' }");
  EXPECT_EQ(t.run("x " + CX + "a"), "x foo");
  t.sh.run("f() { compadd foo bar; compstate[list]='' }");
  t.type("x " + CX + "a");
  EXPECT_EQ(t.screen.row(1), "");
  t.send("\r");
  EXPECT_EQ(t.finish(), "x ");
  // exact (REC_EXACT): the match that is the word is taken.
  t.sh.run("f() { compadd foo foobar; got=${compstate[exact]-unset} }");
  EXPECT_EQ(t.run("x foo" + CX + "a"), "x foo");
  EXPECT_EQ(t.var("got"), "unset");
  t.sh.run("setopt recexact");
  EXPECT_EQ(t.run("x foo" + CX + "a"), "x foo");
  EXPECT_EQ(t.var("got"), "accept");
  t.sh.run("unsetopt recexact; f() { compadd foo foobar; compstate[exact]=accept }");
  EXPECT_EQ(t.run("x foo" + CX + "a"), "x foo");
  // old_list=keep: the matches of the last call are used again.
  t.sh.run("f() { if [[ -n $compstate[old_list] ]]; then compadd zzz; compstate[old_list]=keep"
           "; else compadd foo1 foo2; fi }");
  EXPECT_EQ(t.run("x f" + CX + "a" + CX + "a" + CX + "a"), "x foo1");
  t.sh.run("f() { compadd foo; compstate[list]='list force' }");
  t.type("x " + CX + "a");
  EXPECT_EQ(t.screen.row(0), "> x foo");
  EXPECT_EQ(t.screen.row(1), "foo");
  t.send("\r");
  EXPECT_EQ(t.finish(), "x foo");
}

// ---- compadd -------------------------------------------------------------------------

TEST(Compadd, PrefixesAndSuffixes) {
  WRig t;
  t.widget("2", "complete-word", "f2");
  t.widget("3", "complete-word", "f3");
  t.widget("4", "complete-word", "f4");
  t.sh.run("f2() { compadd -P pre- -S -suf foo }; f3() { compadd -p hp -s hs foo foobar }"
           "; f4() { compadd -i IPRE -I ISUF foo }");
  EXPECT_EQ(t.run("x f" + CX + "2"), "x pre-foo-suf");
  EXPECT_EQ(t.run("x pre-f" + CX + "2"), "x pre-foo-suf");
  EXPECT_EQ(t.run("x f" + CX + "3"), "x f");   // the hidden prefix must match
  // The common hidden suffix goes in, the cursor before it.
  EXPECT_EQ(t.run("x hpf" + CX + "3" + "Z"), "x hpfooZhs");
  EXPECT_EQ(t.run("x f" + CX + "4"), "x IPREfooISUF");
  // -p and -s are quoted, -P -S -i -I go in as they are.
  t.widget("5", "complete-word", "f5");
  t.sh.run("f5() { compadd -p 'd r/' -P 'P x' -s 'h s' -S 'S y' -i 'i p' -I 'I s' 'a b' }");
  EXPECT_EQ(t.run("x " + CX + "5"), "x i pP xd\\ r/a\\ bh\\ sS yI s");
  t.widget("6", "complete-word", "f6");
  t.sh.run("f6() { compadd -p 'd r/' 'a b' }");
  EXPECT_EQ(t.run("x d" + CX + "6"), "x d\\ r/a\\ b");
  EXPECT_EQ(t.run("x d\\ r/a" + CX + "6"), "x d\\ r/a\\ b");
  EXPECT_EQ(t.run("x \"d r/a" + CX + "6"), "x \"d r/a b\"");
}

TEST(Compadd, MatchingAndQuoting) {
  WRig t;
  t.widget("1", "complete-word", "f1");
  t.sh.run("f1() { compadd 'a bc' }");
  EXPECT_EQ(t.run("x a\\ b" + CX + "1"), "x a\\ bc");
  EXPECT_EQ(t.run("x \"a b" + CX + "1"), "x \"a bc\"");
  EXPECT_EQ(t.run("x 'a\\ b" + CX + "1"), "x 'a\\ b");
  t.widget("2", "complete-word", "f2");
  t.sh.run("f2() { compadd -U zzz yyy }");
  EXPECT_EQ(t.run("x f" + CX + "2"), "x ");
  t.widget("3", "complete-word", "f3");
  t.sh.run("f3() { compadd -Q 'a b' 'a$c' }");
  EXPECT_EQ(t.run("x a" + CX + "3"), "x a");
  t.sh.run("f3() { compadd 'a b' 'a$c' }");
  EXPECT_EQ(t.run("x a" + CX + "3"), "x a\\");
  // Without -O / -A / -D's arrays nothing is added.
  t.widget("4", "complete-word", "f4");
  t.sh.run("f4() { compadd -O arr foo foobar bar; local rc=$?; local -a a2=(1 2 3)"
           "; compadd -D a2 foo bar foobar; compadd -A arr3 foo bar"
           "; got=\"$rc ${(j:,:)arr} ${(j:,:)a2} ${(j:,:)arr3} $compstate[nmatches]\" }");
  EXPECT_EQ(t.run("x f" + CX + "4"), "x f");
  EXPECT_EQ(t.var("got"), "1 foo,foobar 1,3 foo 0");
  // -F: patterns of words that are not matches.
  t.widget("5", "complete-word", "f5");
  t.sh.run("f5() { local -a ig=('*bar'); compadd -F ig foo foobar fox"
           "; got=\"$compstate[nmatches] $compstate[ignored]\" }");
  EXPECT_EQ(t.run("x f" + CX + "5"), "x fo");
  EXPECT_EQ(t.var("got"), "2 1");
  t.sh.run("f5() { compadd -F '(*bar fox)' foo foobar fox }");
  EXPECT_EQ(t.run("x f" + CX + "5"), "x foo");
  // Several -M add up.
  t.widget("6", "complete-word", "f6");
  t.sh.run("f6() { compadd -M 'm:{a-z}={A-Z}' -M 'r:|=*' foo }");
  EXPECT_EQ(t.run("x f" + CX + "6"), "x foo");
}

TEST(Compadd, SpecialKindsOfMatches) {
  WRig t;
  t.widget("1", "complete-word", "f1");
  t.sh.run("f1() { compadd -E 1 foo; local n=$compstate[nmatches]; compadd -E 3 bar"
           "; got=\"$n $compstate[nmatches]\" }");
  EXPECT_EQ(t.run("x " + CX + "1"), "x ");
  EXPECT_EQ(t.var("got"), "2 3");
  t.sh.run("f1() { compadd -E 2 foo }");
  EXPECT_EQ(t.run("x f" + CX + "1"), "x foo");   // not alone: no space
  // -C: a match that inserts all the others; the menu starts with it.
  t.widget("2", "complete-word", "f2");
  t.sh.run("f2() { compadd -C foo bar; got=$compstate[nmatches] }");
  EXPECT_EQ(t.run("x " + CX + "2" + CX + "2"), "x bar foo");
  EXPECT_EQ(t.var("got"), "3");
  t.widget("3", "menu-complete", "f3");
  t.sh.run("typeset -A assoc; assoc=(kone 1 ktwo 2); f3() { compadd -k assoc }");
  EXPECT_EQ(t.run("x " + CX + "3"), "x kone");
  t.sh.run("f3() { local -a arr=(aone atwo athree); compadd -a 'arr[2,-1]' }");
  EXPECT_EQ(t.run("x " + CX + "3" + CX + "3"), "x atwo");
  TempDir dir;
  dir.file("plain", "", s::filesystem::perms::owner_read | s::filesystem::perms::owner_write);
  s::filesystem::create_directory(dir.path / "sub");
  dir.file("w", "", s::filesystem::perms::owner_read | s::filesystem::perms::owner_write);
  t.sh.run("f3() { compadd -f -W " + dir.path.string() + "/ sub plain }");
  EXPECT_EQ(t.run("x " + CX + "3"), "x plain");
  EXPECT_EQ(t.run("x " + CX + "3" + CX + "3"), "x sub");
  // -W is joined to the word as it is; a file that isn't there gets no space.
  t.sh.run("f3() { compadd -f -W " + dir.path.string() + " /sub nofile }");
  EXPECT_EQ(t.run("x /" + CX + "3"), "x /sub");
  t.sh.run("f3() { compadd -f nonexistent }");
  t.type("x " + CX + "3");
  EXPECT_EQ(t.screen.row(0), "> x nonexistent");
  t.send("\x03");
  t.finish();
  t.widget("4", "complete-word", "f4");
  t.sh.run("JFSHDIR=" + dir.path.string() + "; f4() { compadd -e JFSHDIR }");
  EXPECT_EQ(t.run("x $JFSHD" + CX + "4"), "x $JFSHDIR");
}

TEST(Compadd, GroupsInListings) {
  WRig t;
  t.widget("1", "list-choices", "l1");
  t.sh.run("l1() { compadd -J g1 -X 'Explain One' zeta alpha"
           "; compadd -V g2 -X 'Explain Two' zz aa mm; compadd -x 'a message' }");
  t.type("x " + CX + "1");
  EXPECT_EQ(t.screen.row(1), "a message");
  EXPECT_EQ(t.screen.row(2), "Explain One");
  EXPECT_EQ(t.screen.row(3), "alpha  zeta");
  EXPECT_EQ(t.screen.row(4), "Explain Two");
  EXPECT_EQ(t.screen.row(5), "zz  aa  mm");
  t.send("\r");
  t.finish();
  t.sh.run("l1() { local -a d=('first -- one' 'second -- two'); compadd -l -d d f1 f2 }");
  t.type("x " + CX + "1");
  EXPECT_EQ(t.screen.row(1), "first -- one");
  EXPECT_EQ(t.screen.row(2), "second -- two");
  t.send("\r");
  t.finish();
  t.sh.run("l1() { compadd -n hidden; compadd shown other }");
  t.type("x " + CX + "1");
  EXPECT_EQ(t.screen.row(1), "other  shown");
  t.send("\r");
  t.finish();
  // -1: consecutive duplicates go; -2: all are kept.
  t.sh.run("l1() { compadd -V v -1 a a b a; compadd -V v2 -2 c c }");
  t.type("x " + CX + "1");
  EXPECT_EQ(t.screen.row(1), "a  b  a");
  // The groups share zsh's column grid: c is as wide as a and b.
  EXPECT_EQ(t.screen.row(2), "c   c");
  t.send("\r");
  t.finish();
  t.sh.run("l1() { compadd -X '%Bbold%b 100%%' x y; compadd -X e2 z }");
  t.type("x " + CX + "1");
  EXPECT_EQ(t.screen.row(1), "bold 100%");
  EXPECT_EQ(t.screen.row(2), "e2");
  EXPECT_EQ(t.screen.row(3), "x  y  z");
  t.send("\r");
  t.finish();
  // Sorted by the display strings; -o nosort keeps the order.
  t.sh.run("l1() { local -a d=(Zed Alpha); compadd -d d b a; compadd -J n -o nosort q p }");
  t.type("x " + CX + "1");
  EXPECT_EQ(t.screen.row(1), "Alpha  Zed");
  EXPECT_EQ(t.screen.row(2), "q      p");
  t.send("\r");
  t.finish();
}

TEST(Compadd, AutoRemovableSuffix) {
  WRig t;
  t.widget("2", "complete-word", "g2");
  t.widget("3", "complete-word", "g3");
  t.widget("4", "complete-word", "g4");
  t.sh.run("g2() { compadd -q -S = foo }; g3() { compadd -r 'a-z' -S = foo }"
           "; rf() { got=\"$1 $BUFFER\" }; g4() { compadd -R rf -S XY foo }");
  EXPECT_EQ(t.run("x f" + CX + "2" + " "), "x foo ");
  EXPECT_EQ(t.run("x f" + CX + "2" + "="), "x foo=");
  EXPECT_EQ(t.run("x f" + CX + "2" + "x"), "x foo=x");
  EXPECT_EQ(t.run("x f" + CX + "2"), "x foo");            // Enter inserts nothing
  EXPECT_EQ(t.run("x f" + CX + "2" + "\x02Z"), "x foZo");  // nor does ^B
  EXPECT_EQ(t.run("x f" + CX + "2" + "\x7f"), "x foo");   // backspace takes the suffix
  EXPECT_EQ(t.run("x f" + CX + "3" + "q"), "x fooq");
  EXPECT_EQ(t.run("x f" + CX + "3" + "0"), "x foo=0");
  EXPECT_EQ(t.run("x f" + CX + "4" + "z"), "x fooXYz");
  EXPECT_EQ(t.var("got"), "2 x fooXY");
  // A user widget leaves it alone.
  t.sh.run("nop() { : }; zle -N nop; bindkey '^Xn' nop");
  t.type("x f" + CX + "2" + CX + "n");
  EXPECT_EQ(t.screen.row(0), "> x foo=");
  t.send("\x03");
  t.finish();
}

// ---- compset -------------------------------------------------------------------------

TEST(Compset, PrefixAndSuffix) {
  WRig t;
  t.widget("a", "complete-word", "f");
  auto check = [&](const str& body, const str& line) {
    t.sh.run("f() { local o; " + body + " }");
    t.run(line + CX + "a");
    return t.var("got");
  };
  str dump = "o+=\"$? [$PREFIX|$SUFFIX|$IPREFIX|$ISUFFIX];\"; got=$o";
  EXPECT_EQ(check("compset -p 2; " + dump + "; compset -p 10; " + dump + "; compset -P '*='; "
                  + dump + "; compset -P 1 x; " + dump, "cmd xxa=b=c"),
            "0 [a=b=c||xx|];1 [a=b=c||xx|];0 [c||xxa=b=|];1 [c||xxa=b=|];");
  EXPECT_EQ(check("compset -P 1 '*='; " + dump, "cmd a=b=c"), "0 [b=c||a=|];");
  EXPECT_EQ(check("compset -P -1 '*='; " + dump, "cmd a=b=c"), "0 [c||a=b=|];");
  EXPECT_EQ(check("compset -P 2 '*='; " + dump, "cmd a=b=c"), "0 [c||a=b=|];");
  EXPECT_EQ(check("compset -P 3 '*='; " + dump, "cmd a=b=c"), "1 [a=b=c|||];");
  EXPECT_EQ(check("compset -P 1 '*'; " + dump + "; compset -P '*'; " + dump, "x a=b=c"),
            "0 [=b=c||a|];0 [||a=b=c|];");
  t.sh.run("setopt completeinword");
  EXPECT_EQ(check("compset -s 1; " + dump + "; compset -S '=*'; " + dump, "cmd ab=cd\x02\x02\x02"),
            "0 [ab|=c||d];0 [ab|||=cd];");
  EXPECT_EQ(check("compset -S 1 '=*'; " + dump, "x ab=c=d\x02\x02\x02\x02"), "0 [ab|=c||=d];");
  t.sh.run("unsetopt completeinword");
  EXPECT_EQ(check("compset -p x; " + dump, "x ab"), "0 [ab|||];");
  EXPECT_EQ(check("{ compset; compset -Z; compset -p } 2>/dev/null; " + dump, "x ab"),
            "1 [ab|||];");
}

TEST(Compset, WordRanges) {
  WRig t;
  t.widget("a", "complete-word", "f");
  auto check = [&](const str& body, const str& line) {
    t.sh.run("f() { " + body + "; got=\"$? ${(j:,:)words} $CURRENT\" }");
    t.run(line + CX + "a");
    return t.var("got");
  };
  EXPECT_EQ(check("compset -n 2", "a b c d e"), "0 b,c,d,e 4");
  EXPECT_EQ(check("compset -n 2; compset -n 5", "a b c d e"), "1 b,c,d,e 4");
  EXPECT_EQ(check("compset -n 2 3", "a b c d e"), "1 a,b,c,d,e 5");
  EXPECT_EQ(check("compset -n -2", "a b c d e"), "0 d,e 2");
  EXPECT_EQ(check("compset -N '-*'", "a -x b -y c d"), "0 c,d 2");
  t.sh.run("setopt completeinword");
  EXPECT_EQ(check("compset -n 2 4", "a b c d e\x02\x02\x02\x02"), "0 b,c,d 2");
  EXPECT_EQ(check("compset -N -e", "a -e b -z c\x02\x02\x02\x02\x02"), "0 b,-z,c 1");
  EXPECT_EQ(check("compset -N -e -z", "a -e b -z c\x02\x02\x02\x02\x02"), "0 b 1");
  EXPECT_EQ(check("compset -N -e -z", "a -e b c\x02\x02"), "0 b,c 1");
}

TEST(Compset, SplitsAQuotedWord) {
  WRig t;
  t.widget("a", "complete-word", "f");
  t.sh.run("dump() { o+=\"$1 [${(j:|:)words}] $CURRENT [$PREFIX|$SUFFIX|$QIPREFIX|$QISUFFIX] "
           "$compstate[all_quotes];\" }");
  t.sh.run("f() { local o; compset -q; dump $?; got=$o }");
  t.run("a b" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 [b] 1 [b|||] \\\\;");
  t.run("a \"b c d" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 [b|c|d] 3 [d||\"b c |] \\\";");
  t.sh.run("f() { local o; compset -q; dump $?; compset -q; dump $?; got=$o }");
  t.run("a \"b 'c d" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 [b|'c d] 2 [c d||\"b '|] '\";0 [c|d] 2 [d||\"b 'c |] \\'\";");
}

// ---- compquote and the builtins outside completion -----------------------------------

TEST(Compquote, QuotesAsTheWordIs) {
  WRig t;
  t.widget("a", "complete-word", "f");
  t.sh.run("f() { local a='a b$c' b=\"it's\"; local -a arr=('x y' 'z*'); compquote a b arr"
           "; local p='=a b'; compquote -p p; local q='=a'; compquote q; local r='~x#'"
           "; compquote r; got=\"[$a] [$b] [${(j:,:)arr}] [$p] [$q] [$r]\" }");
  t.run("x " + CX + "a");
  EXPECT_EQ(t.var("got"), "[a\\ b\\$c] [it\\'s] [x\\ y,z\\*] [=a\\ b] [\\=a] [\\~x\\#]");
  t.run("x 'a" + CX + "a");
  EXPECT_EQ(t.var("got"), "[a b$c] [it'\\''s] [x y,z*] [=a b] [=a] [~x#]");
  t.run("x \"a" + CX + "a");
  EXPECT_EQ(t.var("got"), "[a b\\$c] [it's] [x y,z*] [=a b] [=a] [~x#]");
}

TEST(CompletionBuiltins, OnlyInCompletionFunctions) {
  Shell sh;
  for (strv b : {"compadd x", "compset -p 1", "compquote x"}) {
    auto r = sh.run(str(b));
    EXPECT_EQ(r.rc, 1);
    EXPECT_EQ(r.err, "jfsh: " + str(b.substr(0, b.find(' ')))
                         + ": can only be called from completion function\n");
  }
}

TEST(CompletionBuiltins, CompdefFunctionsSeeTheSameParameters) {
  WRig t;
  t.sh.run("_m() { got=\"$compstate[context] $PREFIX $IPREFIX ${(j:,:)words}\""
           "; compset -P '*='; compadd -S '' foo }; compdef _m mycmd");
  EXPECT_EQ(t.run("mycmd a=f\t"), "mycmd a=foo");
  EXPECT_EQ(t.var("got"), "command a=f  mycmd,a=f");
  // Tab after Tab starts the menu, as for the builtin completers.
  t.sh.run("_m() { compadd foo1 foo2 }");
  EXPECT_EQ(t.run("mycmd f\t\t\t"), "mycmd foo1");
}

// ---- Review round 1 ------------------------------------------------------------------

TEST(CompletionWidget, ListsASingleMatch) {
  WRig t;
  t.sh.run("f() { compadd foo }");
  t.widget("e", "list-choices", "f");
  t.type("x " + CX + "e");
  EXPECT_EQ(t.screen.row(1), "foo");
  t.send("\x03");
  t.finish();
  // The builtin ^D (delete-char-or-list) too.
  TempDir dir;
  dir.file("foo.c", "", s::filesystem::perms::owner_read | s::filesystem::perms::owner_write);
  t.type("ls " + dir.path.string() + "/foo.\x04");
  EXPECT_EQ(t.screen.row(1), "foo.c");
  t.send("\x03");
  t.finish();
}

TEST(CompletionWidget, ErrorThrowsTheLineAway) {
  WRig t;
  t.sh.run("f() { compadd foo; print ${xx?oops} 2>/dev/null }");
  t.widget("a", "complete-word", "f");
  EXPECT_TRUE(t.aborts("x f" + CX + "a"));
  t.sh.run("f() { compadd foo; [[ -n ${xx?oops} ]] 2>/dev/null; compadd fob }");
  EXPECT_TRUE(t.aborts("x f" + CX + "a"));
  // So does one in an ordinary widget, a read-only assignment there too,
  // and an error in a widget run with `zle` stops the one that ran it.
  t.sh.run("g() { print ${xx?oops} 2>/dev/null }; zle -N g; bindkey '^Xg' g");
  EXPECT_TRUE(t.aborts("x f" + CX + "g"));
  t.sh.run("typeset -r ro=1; g() { BUFFER=before; { ro=2 } 2>/dev/null; BUFFER=after }");
  EXPECT_TRUE(t.aborts("x f" + CX + "g"));
  t.sh.run("g2() { print ${xx?oops} 2>/dev/null }; zle -N g2; g() { zle g2; got=after }");
  EXPECT_TRUE(t.aborts("x f" + CX + "g"));
  EXPECT_EQ(t.var("got"), "");
  // In a completion widget a read-only assignment only ends the function,
  // also when a widget ran it with `zle`.
  t.sh.run("f() { compadd foo; { ro=2 } 2>/dev/null; compadd fob }");
  EXPECT_EQ(t.run("x f" + CX + "a"), "x f");
  t.sh.run("g() { zle wa; BUFFER=\"$BUFFER after-$?\" }");
  EXPECT_EQ(t.run("x f" + CX + "g"), "x f after-1");
  EXPECT_EQ(t.run("y"), "y");
  // A user widget that leaves the line alone doesn't end the menu.
  t.sh.run("f() { compadd foo fob fab }; nop() { : }; zle -N nop; bindkey '^Xn' nop");
  t.widget("m", "menu-complete", "f");
  EXPECT_EQ(t.run("x f" + CX + "m" + CX + "n" + CX + "m"), "x fob");
  // Nor does one that runs a completion widget with `zle`.
  t.sh.run("mc() { zle wm }; zle -N mc; bindkey '^Xc' mc");
  EXPECT_EQ(t.run("x f" + CX + "c" + CX + "c"), "x fob");
}

TEST(CompletionWidget, MoreParameterDetails) {
  WRig t;
  t.widget("a", "complete-word", "f");
  t.sh.run("f() { unset CURRENT; got=\"$CURRENT $WIDGETSTYLE\" }");
  t.run("x f" + CX + "a");
  EXPECT_EQ(t.var("got"), "2 complete-word");
  t.sh.run("zle -C wd .complete-word f; bindkey '^Xd' wd");
  t.run("x f" + CX + "d");
  EXPECT_EQ(t.var("got"), "2 .complete-word");
  // A redirection's fd number after the cursor is not a word.
  t.sh.run("f() { got=${(j:,:)words} }");
  t.run("A=1 cmd >out x 'y z'  b c 2>err d" + str(9, '\x02') + CX + "a");
  EXPECT_EQ(t.var("got"), "cmd,x,'y z',b,c,d");
  // compstate[unambiguous] is quoted as it goes into the line, after the
  // open quote.
  t.sh.run("f() { compadd 'ab c' 'ab d'"
           "; got=\"[$compstate[unambiguous]] $compstate[unambiguous_cursor]\" }");
  t.run("x a" + CX + "a");
  EXPECT_EQ(t.var("got"), "[ab\\ ] 5");
  t.run("x 'a" + CX + "a");
  EXPECT_EQ(t.var("got"), "['ab ] 5");
  // compstate[exact] counts when a match is added, not later.
  t.sh.run("f() { compadd foo foobar; compstate[exact]=accept }");
  EXPECT_EQ(t.run("x foo" + CX + "a"), "x foo");
  t.sh.run("f() { compstate[exact]=accept; compadd foo foobar }");
  t.type("x foo" + CX + "a");
  EXPECT_EQ(t.screen.row(0), "> x foo");
  EXPECT_EQ(t.screen.cursor().second, 8);
  t.send("\x03");
  t.finish();
}

TEST(Compadd, DuplicatesOrderAndMessages) {
  WRig t;
  t.widget("a", "complete-word", "f");
  // A word that matched is a success even when its group had it already.
  t.sh.run("f() { compadd foo; compadd foo; got=\"$? $compstate[nmatches]\" }");
  t.run("x " + CX + "a");
  EXPECT_EQ(t.var("got"), "0 1");
  t.sh.run("f() { compadd foo foo fob; compadd -J g foo; local n=$compstate[nmatches]"
           "; compadd -1 -V v x x y x; compadd -2 -V w x x; got=\"$n $compstate[nmatches]\" }");
  t.run("x " + CX + "a");
  EXPECT_EQ(t.var("got"), "3 8");
  // A -S suffix already after the cursor still has to match.
  t.sh.run("setopt completeinword; f() { compadd -S = foo; got=\"$? $compstate[nmatches]\" }");
  EXPECT_EQ(t.run("x f=\x02" + CX + "a"), "x f=");
  EXPECT_EQ(t.var("got"), "1 0");
  t.sh.run("unsetopt completeinword");
  t.widget("e", "list-choices", "l");
  auto listing = [&](const str& body) {
    t.sh.run("l() { " + body + " }");
    t.type("x " + CX + "e");
    str rows = t.screen.row(1) + "|" + t.screen.row(2);
    t.send("\x03");
    t.finish();
    return rows;
  };
  // -E: one empty match, sorted (first) in a sorted group, last with -V.
  EXPECT_EQ(listing("compadd -E 2 b a"), "   a  b|");
  EXPECT_EQ(listing("compadd -E 1 -V g c a b"), "c  a  b|");
  // -o orders a named group only.
  EXPECT_EQ(listing("compadd -o nosort c a b"), "a  b  c|");
  EXPECT_EQ(listing("compadd -J g -o nosort c a b"), "c  a  b|");
  EXPECT_EQ(listing("compadd -X other -x msg bar baz"), "msg|bar  baz");
}

TEST(Compset, MoreForms) {
  WRig t;
  t.widget("a", "complete-word", "f");
  t.sh.run("f() { PREFIX=; compset -P '*'; got=\"$? [$IPREFIX]\" }");
  t.run("x " + CX + "a");
  EXPECT_EQ(t.var("got"), "0 []");
  t.sh.run("f() { compset -q; got=\"$? ${(j:,:)words} $CURRENT [$PREFIX] [$QIPREFIX] "
           "[$compstate[all_quotes]] [$compstate[quote]] [$compstate[quoting]]\" }");
  t.run("x a\\ b" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 a,b 2 [b] [a\\ ] [\\\\] [] []");
  t.run("x \"a\\\\ b" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 a\\ b 1 [a\\ b] [\"] [\\\"] [] []");
  t.run("x 'a \"b" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 a,\"b 2 [b] ['a \"] [\"'] [\"] [double]");
  t.run("x 'a b" + CX + "a");
  EXPECT_EQ(t.var("got"), "0 a,b 2 [b] ['a ] [\\'] [] []");
}

TEST(Compquote, PrefixAndUnsetNames) {
  WRig t;
  t.widget("a", "complete-word", "f");
  t.sh.run("f() { local a='~x' b='=y'; compquote -p a b nosuchname; got=\"$? [$a] [$b]\" }");
  t.run("x " + CX + "a");
  EXPECT_EQ(t.var("got"), "0 [\\~x] [=y]");
}

TEST(Compadd, AutoSuffixOfAMatch) {
  WRig t;
  TempDir dir;
  dir.file("qqunique1", "", s::filesystem::perms::owner_read | s::filesystem::perms::owner_write);
  s::filesystem::create_directory(dir.path / "dduniq1");
  str d = dir.path.string() + "/";
  // The space after a match and a directory's slash go before ; & | a
  // blank, a slash after the slash, or a widget that inserts nothing.
  EXPECT_EQ(t.run("echo " + d + "qquni\t|"), "echo " + d + "qqunique1|");
  EXPECT_EQ(t.run("echo " + d + "qquni\t "), "echo " + d + "qqunique1 ");
  EXPECT_EQ(t.run("echo " + d + "qquni\ta"), "echo " + d + "qqunique1 a");
  EXPECT_EQ(t.run("echo " + d + "qquni\t\x02X"), "echo " + d + "qquniqueX1");
  EXPECT_EQ(t.run("echo " + d + "qquni\t"), "echo " + d + "qqunique1");
  EXPECT_EQ(t.run("echo " + d + "dduni\t "), "echo " + d + "dduniq1 ");
  EXPECT_EQ(t.run("echo " + d + "dduni\t/"), "echo " + d + "dduniq1/");
  EXPECT_EQ(t.run("echo " + d + "dduni\ta"), "echo " + d + "dduniq1/a");
  EXPECT_EQ(t.run("echo " + d + "dduni\t"), "echo " + d + "dduniq1");
  // Widgets that insert text after it keep it (zsh's ZLE_KEEPSUFFIX).
  t.hist.add("echo prev");
  for (const str& m : {str("qqunique1 "), str("dduniq1/")}) {
    str w = m.substr(0, 5);
    EXPECT_EQ(t.run("echo xx\x17" + d + w + "\t\x19"), "echo " + d + m + "xx");
    EXPECT_EQ(t.run("echo " + d + w + "\t\x16x"), "echo " + d + m + "x");
    EXPECT_EQ(t.run("echo " + d + w + "\t\x1b."), "echo " + d + m + "prev");
    // So do kill, redisplay and argument widgets; after the last two it
    // can still go, after a kill not.
    EXPECT_EQ(t.run("echo " + d + w + "\t\x0b"), "echo " + d + m);
    EXPECT_EQ(t.run("echo " + d + w + "\t\x0b|"), "echo " + d + m + "|");
    EXPECT_EQ(t.run("echo " + d + w + "\t\x0c|"), "echo " + d + m.substr(0, m.size() - 1) + "|");
    EXPECT_EQ(t.run("echo " + d + w + "\t\x1b" "2x"), "echo " + d + m + "xx");
    // A motion widget removes it.
    EXPECT_EQ(t.run("echo " + d + w + "\t\x1b" "fZ"),
              "echo " + d + m.substr(0, m.size() - 1) + "Z");
  }
  // A widget function that sets $BUFFER, $CURSOR, … makes it stay for
  // good; one that doesn't leaves it removable.
  t.sh.run("w() { BUFFER+=x }; zle -N w; bindkey '^Xw' w");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "w|"), "echo " + d + "dduniq1/|x");
  t.sh.run("w() { CURSOR=$CURSOR }");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "w|"), "echo " + d + "dduniq1/|");
  t.sh.run("w() { : }");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "w|"), "echo " + d + "dduniq1|");
  t.sh.run("w() { CUTBUFFER=zz }");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "w|"), "echo " + d + "dduniq1|");
  t.sh.run("w() { MARK=2 }");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "w|"), "echo " + d + "dduniq1|");
  // zsh/deltochar's widgets kill, so keep it.
  t.sh.run("zmodload zsh/deltochar; bindkey '^Xz' delete-to-char");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "ze|"), "echo " + d + "dduniq1/|");
  // auto-suffix-remove takes it off; auto-suffix-retain keeps it for good.
  t.sh.run("bindkey '^Xr' auto-suffix-remove; bindkey '^Xk' auto-suffix-retain");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "r"), "echo " + d + "dduniq1");
  EXPECT_EQ(t.run("echo " + d + "qquni\t" + CX + "r" + "a"), "echo " + d + "qqunique1a");
  EXPECT_EQ(t.run("echo " + d + "dduni\t" + CX + "k|"), "echo " + d + "dduniq1/|");
  EXPECT_EQ(t.run("echo " + d + "qquni\t" + CX + "k"), "echo " + d + "qqunique1 ");
  // auto-suffix-remove is a key that inserts nothing: a -r set without
  // \- keeps the suffix, and -R's function gets its length.
  t.sh.run("c() { compadd -S ', ' -r 'x' -- foo1 }; zle -C cw complete-word c"
           "; bindkey '^Xc' cw");
  EXPECT_EQ(t.run("echo fo" + CX + "c" + CX + "r|"), "echo foo1, |");
  t.sh.run("rf() { got=$1 }; c() { compadd -S / -R rf -- foo1 }");
  EXPECT_EQ(t.run("echo fo" + CX + "c" + CX + "r"), "echo foo1/");
  EXPECT_EQ(t.var("got"), "1");
  t.sh.run("unsetopt autoremoveslash");
  EXPECT_EQ(t.run("echo " + d + "dduni\t "), "echo " + d + "dduniq1/ ");
}

}  // namespace
}  // namespace jfsh
